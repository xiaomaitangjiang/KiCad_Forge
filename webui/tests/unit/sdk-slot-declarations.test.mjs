import { test } from 'node:test'
import assert from 'node:assert/strict'
import { declareSlot, defineSlot, inspectorTab, sidebarPanel } from '../../packages/plugin-sdk/dist/index.js'

test('official declaration defaults agree with the exported slot handles', () => {
  assert.deepEqual(declareSlot('inspector.tab'), inspectorTab)
  assert.deepEqual(declareSlot('sidebar.panel'), sidebarPanel)
  assert.equal(declareSlot('inspector.tab').kind, 'keyed')
  assert.ok(Object.isFrozen(declareSlot('sidebar.panel')))
})

test('community declarations retain list defaults and explicit kinds', () => {
  assert.deepEqual(declareSlot('demo.community'), { name: 'demo.community', kind: 'list' })
  assert.deepEqual(declareSlot('demo.community', 'chain'), { name: 'demo.community', kind: 'chain' })
  assert.deepEqual(defineSlot('demo.legacy', 'keyed'), { name: 'demo.legacy', kind: 'keyed' })
})

test('strict declarations retain existing name validation', () => {
  assert.throws(() => declareSlot('bad name'), /Invalid slot name/)
  assert.throws(() => defineSlot('bad/name'), /Invalid slot name/)
})
