import { test } from 'node:test'
import assert from 'node:assert/strict'
import * as sdk from '../packages/plugin-sdk/dist/index.js'

test('public SDK exports the same React-free snapshot factory for standalone models', () => {
  assert.equal(typeof sdk.createSnapshotStore, 'function')
  const initial = { count: 0 }
  const store = sdk.createSnapshotStore(initial, { freeze: true })
  assert.equal(store.getSnapshot(), initial)
  assert.equal(store.disposed, false)
  assert.ok(Object.isFrozen(store.getSnapshot()))
  let notifications = 0
  store.subscribe(() => { notifications += 1 })
  store.set(initial)
  assert.equal(notifications, 0)
  store.update((current) => ({ count: current.count + 1 }))
  assert.equal(store.getSnapshot().count, 1)
  assert.ok(Object.isFrozen(store.getSnapshot()))
  assert.equal(notifications, 1)
  store.dispose()
  store.dispose()
  assert.equal(store.disposed, true)
  store.set({ count: 2 })
  assert.equal(store.getSnapshot().count, 1)
  assert.equal(notifications, 1)
})
