import assert from 'node:assert/strict'
import test from 'node:test'

import {
  blockingMoveConflicts, cutSignature, envelopeMatchesCut,
  moveDirectItems, moveNameConflicts, moveRequest,
} from '../src/editing/move-model.js'

const item = (uuid, name, extra = {}) => ({
  uuid, name, source_digest: 'digest-' + uuid, dependency: false, sexpr: '(symbol ...)', ...extra,
})

test('cutSignature 忽略依赖项、保留顺序', () => {
  const items = [item('a', 'A'), item('d', 'Parent', { dependency: true }), item('b', 'B')]
  assert.equal(cutSignature(items), 'a\ndigest-a\nA\nb\ndigest-b\nB')
  assert.equal(cutSignature([]), '')
  assert.equal(cutSignature(null), '')
})

test('envelopeMatchesCut 只认完全相同的直接项身份', () => {
  const cut = { items: [item('a', 'A'), item('d', 'Parent', { dependency: true })] }
  const same = { items: [item('d', 'Parent', { dependency: true }), item('a', 'A')] }
  assert.equal(envelopeMatchesCut(same, cut), true)
  assert.equal(envelopeMatchesCut({ items: [item('a', 'A'), item('b', 'B')] }, cut), false)
  const renamed = { items: [item('a', 'A', { name: 'A2' })] }
  assert.equal(envelopeMatchesCut(renamed, cut), false)
  const changedDigest = { items: [item('a', 'A', { source_digest: 'other' })] }
  assert.equal(envelopeMatchesCut(changedDigest, cut), false)
  assert.equal(envelopeMatchesCut(null, cut), false)
  assert.equal(envelopeMatchesCut({ items: [] }, cut), false)
})

test('moveNameConflicts 只收集需要改名的冲突并去重', () => {
  const plan = {
    conflicts: [
      { name: 'A', reason: 'name_conflict' },
      { name: 'A', reason: 'name_conflict' },
      { name: 'B', reason: 'unmoved_derivatives' },
      { name: 'C' },
      'D',
    ],
  }
  assert.deepEqual(moveNameConflicts(plan), ['A', 'C', 'D'])
  assert.deepEqual(moveNameConflicts({}), [])
})

test('blockingMoveConflicts 返回不能靠改名解决的原因', () => {
  const plan = {
    conflicts: [
      { name: 'A', reason: 'name_conflict' },
      { name: 'B', reason: 'missing_parent' },
      { name: 'C', reason: 'already_in_target' },
    ],
  }
  assert.deepEqual(blockingMoveConflicts(plan).map(c => c.reason), ['missing_parent', 'already_in_target'])
})

test('moveDirectItems 只保留直接选中项', () => {
  assert.deepEqual(moveDirectItems([item('a', 'A'), item('p', 'P', { dependency: true })]).map(i => i.uuid), ['a'])
})

test('moveRequest 提交才带目标摘要，预检不带', () => {
  const base = { token: 't', targetId: 'lib', operationId: 'op', renames: { A: 'A2' } }
  const dry = moveRequest({ ...base, dryRun: true, targetDigest: 'abc' })
  assert.deepEqual(dry, {
    token: 't', target_library_id: 'lib', operation_id: 'op', dry_run: true, renames: { A: 'A2' },
  })
  const commit = moveRequest({ ...base, dryRun: false, targetDigest: 'abc' })
  assert.equal(commit.target_digest, 'abc')
  assert.equal(commit.dry_run, false)
  const fresh = moveRequest({ ...base, dryRun: false })
  assert.equal('target_digest' in fresh, false)
})
