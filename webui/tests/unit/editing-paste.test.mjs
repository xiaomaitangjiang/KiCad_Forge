import test from 'node:test'
import assert from 'node:assert/strict'
import {
  conflictNames, pasteBudgetViolation, pasteItems, pasteRequest, unresolvedConflicts,
} from '../../src/editing/paste-model.js'

test('conflict names accept both plan shapes and stay unique/trimmed', () => {
  assert.deepEqual(conflictNames({ conflicts: [{ name: 'A' }, 'B', { name: ' A ' }, null] }), ['A', 'B', 'A'])
  assert.deepEqual(conflictNames({}), [])
  assert.deepEqual(conflictNames(null), [])
})

test('unresolved conflicts require a non-blank rename', () => {
  assert.deepEqual(unresolvedConflicts(['A', 'B'], { A: 'A2', B: '  ' }), ['B'])
  assert.deepEqual(unresolvedConflicts(['A'], { A: 'A2' }), [])
})

test('paste items keep only host fields and drop empty nodes', () => {
  const items = pasteItems({
    items: [
      { name: ' R1 ', sexpr: '(symbol "R1")', dependency: 0, uuid: 'u', source_path: 'D:/secret', library_id: 'lib' },
      { name: 'Parent', sexpr: '(symbol "Parent")', dependency: true },
      { name: '', sexpr: '(symbol "")' },
      { name: 'NoBody', sexpr: '' },
    ],
  })
  assert.equal(items.length, 2)
  assert.deepEqual(Object.keys(items[0]).sort(), ['dependency', 'name', 'sexpr'])
  assert.equal(items[0].name, 'R1')
  assert.equal(items[0].dependency, false)
  assert.equal(items[1].dependency, true)
})

test('paste request omits digest until the preflight returns one', () => {
  const base = { targetId: 'lib-1', operationId: 'op-1', dryRun: true, renames: {}, items: [] }
  assert.equal('target_digest' in pasteRequest(base), false)
  const commit = pasteRequest({ ...base, dryRun: false, targetDigest: 'abc' })
  assert.equal(commit.dry_run, false)
  assert.equal(commit.target_digest, 'abc')
  assert.equal(commit.target_library_id, 'lib-1')
})

test('paste budget rejects oversized batches before any request', () => {
  const many = Array.from({ length: 201 }, (_, index) => ({ name: 'S' + index, sexpr: '(symbol "S")' }))
  assert.equal(pasteBudgetViolation(many, {}), 'items')
  const huge = [{ name: 'Huge', sexpr: 'x'.repeat(5 * 1024 * 1024) }]
  assert.equal(pasteBudgetViolation(huge, {}), 'bytes')
  assert.equal(pasteBudgetViolation([{ name: 'R', sexpr: '(symbol "R")' }], {}), null)
})
