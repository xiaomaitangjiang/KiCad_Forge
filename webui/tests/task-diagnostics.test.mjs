import test from 'node:test'
import assert from 'node:assert/strict'
import {
  MAX_TASK_DIAGNOSTIC_ERROR_LENGTH,
  MAX_TASK_DIAGNOSTIC_LENGTH,
  formatTaskDiagnosticTime,
  formatTaskDiagnostics,
  taskDiagnosticFields,
} from '../src/lib/task-diagnostics.js'

const sample = {
  id: 'task-42',
  pluginId: 'com.example.git',
  action: 'commit',
  state: 'failed',
  uncertain: false,
  phase: 'write',
  updatedAt: Date.UTC(2026, 9, 20, 8, 30, 0),
  error: 'pre-commit hook rejected the change',
  result: { output: 'SECRET FULL RESULT', patch: 'SECRET PATCH' },
}

test('formatTaskDiagnostics copies only the selected task identity and bounded fields', () => {
  const text = formatTaskDiagnostics(sample)
  assert.match(text, /^task: task-42$/m)
  assert.match(text, /^plugin: com\.example\.git$/m)
  assert.match(text, /^action: commit$/m)
  assert.match(text, /^state: failed$/m)
  assert.match(text, /^uncertain: false$/m)
  assert.match(text, /^phase: write$/m)
  assert.match(text, /^time: 2026-10-20T08:30:00\.000Z$/m)
  assert.match(text, /^error: pre-commit hook rejected the change$/m)
  assert.equal(text.includes('SECRET'), false, 'the full result must never be copied')
  assert.equal(text.includes('"result"'), false)
})

test('formatTaskDiagnostics is bound to one task and never mixes in another task', () => {
  const other = { ...sample, id: 'task-99', pluginId: 'other.plugin', action: 'status', error: 'other failure' }
  const first = formatTaskDiagnostics(sample)
  const second = formatTaskDiagnostics(other)
  assert.equal(first.includes('task-99'), false)
  assert.equal(first.includes('other.plugin'), false)
  assert.equal(first.includes('other failure'), false)
  assert.equal(second.includes('task-42'), false)
  assert.equal(second.includes('com.example.git'), false)
})

test('uncertain is explicit, survives missing/false input, and changes the copied text', () => {
  const base = { ...sample, state: 'failed', error: 'write timed out' }
  const unverified = formatTaskDiagnostics({ ...base, uncertain: true })
  const verified = formatTaskDiagnostics({ ...base, uncertain: false })
  assert.match(unverified, /^uncertain: true$/m)
  assert.match(verified, /^uncertain: false$/m)
  assert.notEqual(unverified, verified)
  assert.equal(taskDiagnosticFields({ ...base, uncertain: true }).uncertain, true)
  assert.equal(taskDiagnosticFields({ ...base, uncertain: false }).uncertain, false)
  assert.equal(taskDiagnosticFields(base).uncertain, false, 'missing uncertain must not be treated as true')
})

test('long fields keep every critical line and only shorten the trailing error', () => {
  const longError = 'e'.repeat(4000)
  const fields = taskDiagnosticFields({ ...sample, error: longError })
  assert.equal(fields.error.length, MAX_TASK_DIAGNOSTIC_ERROR_LENGTH)
  assert.equal(fields.error.endsWith('…'), true)

  const text = formatTaskDiagnostics({
    ...sample,
    id: 'i'.repeat(2000),
    pluginId: 'p'.repeat(2000),
    action: 'a'.repeat(2000),
    phase: 'f'.repeat(2000),
    uncertain: true,
    error: longError,
  })
  assert.ok(text.length <= MAX_TASK_DIAGNOSTIC_LENGTH, `diagnostic text used ${text.length} chars`)
  assert.equal(text.endsWith('…'), true)
  for (const key of ['task', 'plugin', 'action', 'state', 'uncertain', 'phase', 'time', 'error']) {
    assert.match(text, new RegExp(`^${key}: `, 'm'), `${key} line must survive the total bound`)
  }
  assert.match(text, /^state: failed$/m)
  assert.match(text, /^uncertain: true$/m)
  assert.match(text, /^error: e+…$/m)
})

test('missing values use explicit placeholders and invalid timestamps are not fabricated', () => {
  const fields = taskDiagnosticFields({})
  assert.deepEqual(fields, {
    id: '', pluginId: '', action: '', state: '', uncertain: false, phase: '', time: '', error: '',
  })
  const text = formatTaskDiagnostics({})
  assert.match(text, /^state: unknown$/m)
  assert.match(text, /^uncertain: false$/m)
  assert.match(text, /^time: -$/m)
  assert.equal(formatTaskDiagnosticTime(0), '1970-01-01T00:00:00.000Z')
  assert.equal(formatTaskDiagnosticTime('not-a-time'), '')
  assert.equal(formatTaskDiagnosticTime(undefined), '')
})
