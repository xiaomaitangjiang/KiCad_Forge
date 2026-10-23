import test from 'node:test'
import assert from 'node:assert/strict'
import {
  emptySelection, selectionCount, isSelected, selectOnly, toggleSelection,
  extendSelection, selectAllIn, normalizeSelection, removeFromSelection,
} from '../../src/editing/selection-model.js'

test('selectOnly replaces the whole set and moves the anchor', () => {
  const next = selectOnly('a')
  assert.deepEqual(next, { ids: ['a'], activeId: 'a', anchorId: 'a' })
  assert.equal(selectionCount(next), 1)
  assert.ok(isSelected(next, 'a'))
  assert.ok(!isSelected(next, 'b'))
})

test('toggleSelection adds and removes a uuid without duplicating it', () => {
  let s = selectOnly('a')
  s = toggleSelection(s, 'b')
  assert.deepEqual(s.ids, ['a', 'b'])
  assert.equal(s.activeId, 'b')
  s = toggleSelection(s, 'b')
  assert.deepEqual(s.ids, ['a'])
  assert.equal(s.activeId, 'a')
  s = toggleSelection(s, 'a')
  assert.deepEqual(s.ids, [])
  assert.equal(s.activeId, null)
  assert.deepEqual(toggleSelection(emptySelection(), 'a').ids, ['a'])
})

test('extendSelection selects the ordered range in both directions', () => {
  const order = ['a', 'b', 'c', 'd']
  let s = selectOnly('b')
  s = extendSelection(s, 'd', order)
  assert.deepEqual(s.ids, ['b', 'c', 'd'])
  assert.equal(s.activeId, 'd')
  assert.equal(s.anchorId, 'b')
  // 反向 Shift 仍按排序取连续段，锚点不移动
  const back = extendSelection(s, 'a', order)
  assert.deepEqual(back.ids, ['a', 'b'])
  assert.equal(back.activeId, 'a')
  assert.equal(back.anchorId, 'b')
})

test('extendSelection falls back to single selection when anchor or target is missing', () => {
  const order = ['a', 'b', 'c']
  assert.deepEqual(extendSelection(emptySelection(), 'c', order), selectOnly('c'))
  const stale = { ids: ['x'], activeId: 'x', anchorId: 'x' }
  assert.deepEqual(extendSelection(stale, 'c', order), selectOnly('c'))
  const s = { ids: ['a'], activeId: 'a', anchorId: 'a' }
  assert.deepEqual(extendSelection(s, 'zz', order), selectOnly('zz'))
})

test('selectAllIn uses the loaded order and clears the detail object', () => {
  const s = selectAllIn(['a', 'b', 'c'])
  assert.deepEqual(s, { ids: ['a', 'b', 'c'], activeId: null, anchorId: null })
  assert.deepEqual(selectAllIn(null).ids, [])
})

test('normalizeSelection drops vanished uuids but keeps the set identity when nothing changed', () => {
  const s = { ids: ['a', 'b'], activeId: 'b', anchorId: 'a' }
  assert.equal(normalizeSelection(s, new Set(['a', 'b', 'c'])), s)
  const pruned = normalizeSelection(s, new Set(['b']))
  assert.deepEqual(pruned.ids, ['b'])
  assert.equal(pruned.activeId, 'b')
  assert.equal(pruned.anchorId, 'b')
  const empty = normalizeSelection({ ids: ['a'], activeId: 'a', anchorId: 'a' }, new Set())
  assert.deepEqual(empty, { ids: [], activeId: null, anchorId: null })
})

test('removeFromSelection only removes the deleted object', () => {
  const s = { ids: ['a', 'b', 'c'], activeId: 'b', anchorId: 'a' }
  const next = removeFromSelection(s, 'b')
  assert.deepEqual(next.ids, ['a', 'c'])
  assert.equal(next.activeId, 'c')
  assert.equal(next.anchorId, 'a')
  assert.equal(removeFromSelection(s, 'zz'), s)
})
