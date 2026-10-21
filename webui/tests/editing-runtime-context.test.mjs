// Formal regression tests for the editing runtime/adapters (R1/R2).
// The review probes in docs/AgentDocs/review-2026-10-20 are migrated here so they
// run with the rest of webui/tests instead of only as ad-hoc evidence.
import test from 'node:test'
import assert from 'node:assert/strict'
import { createEditingRuntime } from '../src/editing/runtime.js'

function fixture(t) {
  const savedDocument = globalThis.document
  const savedNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator')
  let clipboard = ''
  let pendingRead = null
  let dialog = null
  const listeners = new Map()
  const doc = {
    activeElement: null,
    addEventListener(type, listener) {
      if (!listeners.has(type)) listeners.set(type, new Set())
      listeners.get(type).add(listener)
    },
    removeEventListener(type, listener) {
      listeners.get(type)?.delete(listener)
    },
    emit(type, event = {}) {
      for (const listener of listeners.get(type) || []) listener(event)
    },
    querySelectorAll: () => (dialog ? [dialog] : []),
    execCommand(command, _show, content) {
      const host = doc.activeElement
      if (!host) return false
      const start = host.selectionStart
      const end = host.selectionEnd
      if (command === 'insertText' || command === 'delete') {
        host.value = host.value.slice(0, start) + (command === 'delete' ? '' : content) + host.value.slice(end)
        return true
      }
      return false
    },
  }
  const input = {
    nodeType: 1,
    tagName: 'INPUT',
    type: 'text',
    isConnected: true,
    isContentEditable: false,
    disabled: false,
    readOnly: false,
    value: 'abcdef',
    selectionStart: 1,
    selectionEnd: 3,
    getClientRects: () => [{}],
    closest: () => null,
    getAttribute: () => null,
    focus() { doc.activeElement = input },
    contains: () => false,
    setSelectionRange(start, end) {
      input.selectionStart = start
      input.selectionEnd = end
    },
  }
  globalThis.document = doc
  Object.defineProperty(globalThis, 'navigator', {
    configurable: true,
    value: {
      clipboard: {
        writeText: async (value) => { clipboard = value },
        readText: () => new Promise((resolve) => { pendingRead = resolve }),
      },
    },
  })
  const runtime = createEditingRuntime({ platform: 'other' })
  runtime.setScopeKey('workspace-A:home')
  input.focus()
  runtime.rememberTextTarget(input)
  t?.after(() => {
    globalThis.document = savedDocument
    if (savedNavigator) Object.defineProperty(globalThis, 'navigator', savedNavigator)
    else delete globalThis.navigator
  })
  return {
    runtime,
    input,
    doc,
    clipboard: () => clipboard,
    resolveRead: (value) => {
      assert.equal(typeof pendingRead, 'function')
      pendingRead(value)
    },
    install() {
      runtime.install(doc)
    },
    emitSelectionChange() {
      doc.emit('selectionchange', {})
    },
    openDialog() {
      dialog = { isConnected: true, getClientRects: () => [{}] }
      doc.activeElement = { nodeType: 1, tagName: 'BUTTON', closest: () => null }
    },
  }
}


test('editing runtime: an external host write freezes object commands until it ends', async (t) => {
  const f = fixture(t)
  let undoCalls = 0
  f.runtime.setObjectProvider({
    getState: () => ({
      domain: 'symbols', count: 1, total: 1, supported: true, writable: true,
      clipboard: { kind: 'kforge-symbols', version: 1, bytes: 10, items: 1 },
      history: { undoable: true, redoable: true },
    }),
    handlers: {
      undo: async () => { undoCalls += 1; return { status: 'done' } },
    },
  })
  const frozen = f.runtime.freeze('objects')
  f.runtime.setExternalBusy(true)
  assert.equal(f.runtime.getState('undo', frozen).reason, 'busy')
  const blocked = await f.runtime.execute('undo', frozen)
  assert.equal(blocked.status, 'disabled')
  assert.equal(undoCalls, 0)
  f.runtime.setExternalBusy(false)
  assert.equal(f.runtime.getState('undo', frozen).disabled, false)
})

test('editing runtime: control copy reads the frozen selection', async (t) => {
  const f = fixture(t)
  const result = await f.runtime.execute('copy', f.runtime.freeze())
  assert.equal(result.status, 'done')
  assert.equal(f.clipboard(), 'bc')
})

test('editing runtime: select-all keeps an immediately following cut valid', async (t) => {
  const f = fixture(t)
  f.install()
  const selected = await f.runtime.execute('selectAll', f.runtime.freeze())
  assert.equal(selected.status, 'done')
  const cutSnapshot = f.runtime.freeze()
  const pending = f.runtime.execute('cut', cutSnapshot)
  f.emitSelectionChange()
  const cut = await pending
  assert.equal(cut.status, 'done')
  assert.equal(f.clipboard(), 'abcdef')
  assert.equal(f.input.value, '')
})

test('editing runtime: a foreground dialog blocks background object cut', async (t) => {
  const f = fixture(t)
  let cutCalls = 0
  f.runtime.clearTextTarget()
  f.runtime.setObjectProvider({
    getState: () => ({ domain: 'symbols', count: 1, total: 1, supported: true, writable: true }),
    handlers: { cut: async () => { cutCalls += 1; return { status: 'done' } } },
  })
  f.openDialog()
  const result = await f.runtime.execute('cut')
  assert.equal(result.status, 'disabled')
  assert.equal(cutCalls, 0)
})

test('editing runtime: async paste aborts after scope A -> B -> A', async (t) => {
  const f = fixture(t)
  const before = f.input.value
  const pending = f.runtime.execute('paste', f.runtime.freeze())
  f.runtime.setScopeKey('workspace-B:settings')
  f.runtime.setScopeKey('workspace-A:home')
  f.resolveRead('PASTED')
  await pending
  assert.equal(f.input.value, before)
})

test('editing runtime: async paste aborts after an edit even when the value is restored', async (t) => {
  const f = fixture(t)
  const before = f.input.value
  const pending = f.runtime.execute('paste', f.runtime.freeze())
  f.input.value = 'changed'
  f.runtime.bumpVersion(f.input)
  f.input.value = before
  f.runtime.bumpVersion(f.input)
  f.resolveRead('PASTED')
  await pending
  assert.equal(f.input.value, before)
})

test('editing runtime: a frozen menu selection is used instead of the current one', async (t) => {
  const f = fixture(t)
  const frozen = f.runtime.freeze()
  f.input.setSelectionRange(3, 5)
  const result = await f.runtime.execute('copy', frozen)
  assert.ok(result.status !== 'done' || f.clipboard() === 'bc')
})
