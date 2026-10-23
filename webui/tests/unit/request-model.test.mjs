import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createRequestModel } from '../../src/models/request.js'

test('inactive callbacks cannot start reads; StrictMode activation resumes the same model', async () => {
  const model = createRequestModel()
  let reads = 0
  const read = async () => ++reads
  model.invalidate()
  assert.equal((await model.loadTracked(read)).status, 'superseded')
  assert.equal(reads, 0)
  model.activate()
  assert.equal((await model.loadTracked(read)).status, 'published')
  assert.equal(reads, 1)
  model.dispose()
})

function deferred() {
  let resolve, reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

test('request model publishes the latest read and legacy load returns each raw response', async () => {
  const model = createRequestModel()
  const a = deferred(), b = deferred()
  const old = model.load(() => a.promise)
  const current = model.loadTracked(() => b.promise)
  b.resolve(['B'])
  assert.deepEqual(await current, { status: 'published', data: ['B'] })
  a.resolve(['A'])
  assert.deepEqual(await old, ['A'])
  assert.deepEqual(model.getSnapshot().data, ['B'])
  assert.equal(model.getSnapshot().status, 'ready')
})

test('read arguments and validators stay attached to their own request', async () => {
  const model = createRequestModel()
  const args = []
  const source = (...values) => { args.push(values); return values }
  assert.deepEqual(await model.load(source, ['Unicode 子库', '']), ['Unicode 子库', ''])
  assert.deepEqual(args, [['Unicode 子库', '']])
  const result = await model.loadTracked(() => null, [], Array.isArray)
  assert.equal(result.status, 'failed')
  assert.match(result.error.message, /读取结果格式无效/)
  assert.deepEqual(model.getSnapshot().data, ['Unicode 子库', ''])
})

test('reloading and stale failure preserve current error until a successful publish', async () => {
  const model = createRequestModel()
  const kept = new Error('kept')
  assert.equal(await model.load(() => { throw kept }), null)
  const a = deferred(), b = deferred()
  const old = model.loadTracked(() => a.promise)
  const current = model.loadTracked(() => b.promise)
  assert.equal(model.getSnapshot().error, kept)
  assert.equal(model.getSnapshot().status, 'loading')
  a.reject(new Error('stale'))
  assert.equal((await old).status, 'superseded')
  assert.equal(model.getSnapshot().error, kept)
  assert.equal(model.getSnapshot().status, 'loading')
  b.resolve('recovered')
  await current
  assert.equal(model.getSnapshot().error, null)
})

test('setData and setError updater semantics use the latest store, including failed reads', async () => {
  const model = createRequestModel()
  model.setData(() => ['one'])
  model.setData((current) => [...current, 'two'])
  model.setError(new Error('first'))
  model.setError((current) => new Error(current.message + '-updated'))
  assert.deepEqual(model.getSnapshot().data, ['one', 'two'])
  assert.equal(model.getSnapshot().error.message, 'first-updated')
  const pending = deferred()
  const read = model.loadTracked(() => pending.promise)
  model.setData((current) => [...current, 'during-read'])
  pending.reject(new Error('failed'))
  assert.equal((await read).status, 'failed')
  assert.deepEqual(model.getSnapshot().data, ['one', 'two', 'during-read'])
})

test('superseded invalid data keeps raw legacy return and skips current-only validation', async () => {
  const model = createRequestModel()
  const pending = deferred()
  let validations = 0
  const stale = model.load(() => pending.promise, [], () => { validations++; return false })
  await model.load(() => ['latest'])
  pending.resolve('raw-old-invalid-data')
  assert.equal(await stale, 'raw-old-invalid-data')
  assert.equal(validations, 0)
  assert.deepEqual(model.getSnapshot().data, ['latest'])
})

test('invalidate during an ignoring read preserves the store and prevents late publication', async () => {
  const model = createRequestModel()
  await model.load(() => ['kept'])
  const pending = deferred()
  const stale = model.loadTracked(() => pending.promise)
  model.invalidate()
  const idle = model.getSnapshot()
  assert.equal(idle.status, 'idle')
  pending.resolve(['late'])
  assert.equal((await stale).status, 'superseded')
  assert.equal(model.getSnapshot(), idle)
  model.activate()
  await model.load(() => ['new mount'])
  assert.deepEqual(model.getSnapshot().data, ['new mount'])
})

test('notifications are released and repeated writes of the same value keep snapshot identity', async () => {
  const model = createRequestModel()
  let notifications = 0
  const off = model.subscribe(() => notifications++)
  const before = model.getSnapshot()
  model.setData(null); model.setError(null)
  assert.equal(model.getSnapshot(), before)
  assert.equal(notifications, 0)
  off(); off()
  await model.load(() => 1)
  assert.equal(notifications, 0)
  model.dispose(); model.dispose()
  assert.equal((await model.loadTracked(() => assert.fail('disposed read'))).status, 'superseded')
})

test('200 mount/read/invalidate cycles retain one snapshot source and no active subscriptions', async () => {
  const model = createRequestModel()
  let notifications = 0
  for (let index = 0; index < 200; index++) {
    model.activate()
    const off = model.subscribe(() => notifications++)
    await model.load(() => index)
    off()
    const previous = notifications
    model.invalidate()
    assert.equal(notifications, previous)
  }
  assert.equal(model.getSnapshot().data, 199)
  model.dispose()
})
