import test from 'node:test'
import assert from 'node:assert/strict'
import {
  CLIPBOARD_PREFIX, buildEnvelope, serializeEnvelope, parseEnvelope,
  budgetViolation, copyEnvelope, readClipboardText, writeClipboardText,
} from '../src/editing/clipboard-model.js'
import { MAX_CLIPBOARD_ITEMS } from '../src/editing/limits.js'

const item = (uuid = 'u-1', extra = {}) => ({
  uuid, kf_id: 'kf-' + uuid, name: 'R_' + uuid, library_id: 'lib-1', library_name: 'Lib',
  source_path: 'D:/libs/device.kicad_sym', source_digest: 'abc', dependency: false,
  sexpr: '(symbol "R_' + uuid + '" (property "Reference" "R"))', ...extra,
})

function withClipboard(impl) {
  const previous = globalThis.navigator
  Object.defineProperty(globalThis, 'navigator', { value: { clipboard: impl }, configurable: true })
  return () => Object.defineProperty(globalThis, 'navigator', { value: previous, configurable: true })
}

test('envelope round-trips through the prefixed text payload', () => {
  const envelope = buildEnvelope([item('a'), item('b')], { summary: '2 symbols' })
  const text = serializeEnvelope(envelope)
  assert.ok(text.startsWith(CLIPBOARD_PREFIX))
  const parsed = parseEnvelope(text)
  assert.equal(parsed.kind, 'kforge.symbols')
  assert.equal(parsed.version, 1)
  assert.equal(parsed.items.length, 2)
  assert.equal(parsed.items[0].sexpr, envelope.items[0].sexpr)
  assert.equal(parsed.summary, '2 symbols')
})

test('Windows CRLF line endings before the payload are accepted', () => {
  const envelope = buildEnvelope([item('a')], { summary: '1 symbol' })
  const text = serializeEnvelope(envelope).replace('\n', '\r\n')
  const parsed = parseEnvelope(text)
  assert.equal(parsed.items.length, 1)
  assert.equal(parsed.items[0].sexpr, envelope.items[0].sexpr)
})

test('foreign clipboard text is not treated as symbol data', () => {
  assert.equal(parseEnvelope('plain text'), null)
  assert.equal(parseEnvelope(''), null)
  assert.equal(parseEnvelope(undefined), null)
})

test('broken or incompatible envelopes fail with clipboardFormat', () => {
  assert.throws(() => parseEnvelope(CLIPBOARD_PREFIX + '{'), (error) => error.code === 'clipboardFormat')
  assert.throws(() => parseEnvelope(CLIPBOARD_PREFIX + JSON.stringify({ kind: 'other', version: 1, items: [{}] })),
    (error) => error.code === 'clipboardFormat')
  assert.throws(() => parseEnvelope(CLIPBOARD_PREFIX + JSON.stringify({ kind: 'kforge.symbols', version: 2, items: [{}] })),
    (error) => error.code === 'clipboardFormat')
  assert.throws(() => parseEnvelope(CLIPBOARD_PREFIX + JSON.stringify({ kind: 'kforge.symbols', version: 1, items: [] })),
    (error) => error.code === 'clipboardFormat')
})

test('envelope rejects empty input and item-count overflow', () => {
  assert.throws(() => buildEnvelope([]), (error) => error.code === 'clipboardEmpty')
  const many = Array.from({ length: MAX_CLIPBOARD_ITEMS + 1 }, (_, index) => item('x' + index))
  assert.throws(() => buildEnvelope(many), (error) => error.code === 'clipboardTooLarge' && error.limit === 'items')
})

test('budgetViolation reports byte overflow before writing', async () => {
  const huge = buildEnvelope([item('big', { sexpr: 'x'.repeat(5 * 1024 * 1024) })])
  assert.equal(budgetViolation(huge), 'bytes')
  await assert.rejects(copyEnvelope(huge), (error) => error.code === 'clipboardTooLarge' && error.limit === 'bytes')
})

test('copyEnvelope writes prefixed text through the clipboard API', async () => {
  const writes = []
  const restore = withClipboard({ writeText: async (text) => { writes.push(text) } })
  try {
    await copyEnvelope(buildEnvelope([item('a')]))
    assert.equal(writes.length, 1)
    assert.ok(writes[0].startsWith(CLIPBOARD_PREFIX))
    assert.equal(parseEnvelope(writes[0]).items[0].uuid, 'a')
  } finally {
    restore()
  }
})

test('clipboard failures are classified and never silently succeed', async () => {
  const restoreMissing = withClipboard(undefined)
  try {
    await assert.rejects(writeClipboardText('x'), (error) => error.code === 'clipboardUnavailable')
  } finally {
    restoreMissing()
  }

  const denied = async () => { throw new Error('denied') }
  const restoreDenied = withClipboard({ writeText: denied, readText: denied })
  try {
    await assert.rejects(writeClipboardText('x'), (error) => error.code === 'clipboardDenied')
    await assert.rejects(readClipboardText(), (error) => error.code === 'clipboardDenied')
  } finally {
    restoreDenied()
  }
})
