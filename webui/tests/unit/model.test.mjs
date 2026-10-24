import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createResourceModel, createSnapshotStore } from '../../src/plugin-host/model.js'

function deferred() {
  let resolve
  let reject
  const promise = new Promise((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

test('snapshot store keeps identity until a real commit', () => {
  const store = createSnapshotStore({ value: 1 }, { freeze: true })
  const first = store.getSnapshot()
  store.set({ value: 1 })
  assert.notEqual(store.getSnapshot(), first)
  const second = store.getSnapshot()
  store.set(second)
  assert.equal(store.getSnapshot(), second)
  assert.ok(Object.isFrozen(second))
})

test('subscription release is idempotent and stops notifications', () => {
  const store = createSnapshotStore(0)
  let calls = 0
  const off = store.subscribe(() => calls++)
  store.set(1)
  off()
  off()
  store.set(2)
  assert.equal(calls, 1)
})

test('keyed resource drops stale success and stale error', async () => {
  const calls = new Map()
  const model = createResourceModel({
    load: (params) => {
      const slot = deferred()
      calls.set(params.id, slot)
      return slot.promise
    },
  })

  const first = model.request('symbol', { id: 'A' })
  const second = model.request('symbol', { id: 'B' })
  calls.get('A').resolve({ id: 'A' })
  calls.get('B').resolve({ id: 'B' })

  assert.equal((await first).status, 'superseded')
  assert.equal((await second).status, 'published')
  assert.deepEqual(model.get('symbol').getSnapshot().data, { id: 'B' })

  const third = model.request('symbol', { id: 'C' })
  const fourth = model.request('symbol', { id: 'D' })
  calls.get('C').reject(new Error('old failure'))
  calls.get('D').resolve({ id: 'D' })
  assert.equal((await third).status, 'superseded')
  assert.equal((await fourth).status, 'published')
  assert.equal(model.get('symbol').getSnapshot().status, 'ready')
  assert.deepEqual(model.get('symbol').getSnapshot().data, { id: 'D' })
})

test('keys are isolated and dispose aborts the in-flight request', async () => {
  const signals = new Map()
  const model = createResourceModel({
    load: (params, { signal }) => {
      const slot = deferred()
      signals.set(params.id, signal)
      signal?.addEventListener('abort', () => slot.reject(new Error('aborted')), { once: true })
      return slot.promise
    },
  })

  const pending = model.request('workspace-a:symbol-1', { id: 'A' })
  assert.equal(model.get('workspace-a:symbol-1').getSnapshot().status, 'loading')
  assert.equal(model.get('workspace-b:symbol-2').getSnapshot().status, 'idle')

  model.dispose('workspace-a:symbol-1')
  assert.equal(signals.get('A').aborted, true)
  assert.equal(model.has('workspace-a:symbol-1'), false)
  const outcome = await pending
  assert.equal(outcome.status, 'superseded')
})

test('disposeAll disposes ready and idle stores, not only in-flight ones', async () => {
  const model = createResourceModel({ load: async () => ({ ready: true }) })
  await model.request('ready', {})
  const ready = model.get('ready')
  const idle = model.get('idle')
  let calls = 0
  ready.subscribe(() => calls++)
  model.disposeAll()
  assert.equal(ready.disposed, true)
  assert.equal(idle.disposed, true)
  ready.set({ changed: true })
  assert.equal(calls, 0)
  assert.equal(model.has('ready'), false)
  model.disposeAll()
})

test('external abort before an ignoring load resolves publishes neither ready nor loading', async () => {
  const pending = deferred()
  let calls = 0
  const model = createResourceModel({ load: () => (++calls === 1 ? Promise.resolve({ id: 'previous' }) : pending.promise) })
  await model.request('symbol', { id: 'previous' })
  const controller = new AbortController()
  const current = model.request('symbol', { id: 'next' }, { signal: controller.signal })
  controller.abort()
  pending.resolve({ id: 'late' })
  assert.equal((await current).status, 'aborted')
  const snapshot = model.get('symbol').getSnapshot()
  assert.equal(snapshot.status, 'idle')
  assert.deepEqual(snapshot.data, { id: 'previous' })
})

test('external abort listeners are released after completion and cancellation', async () => {
  class TrackingSignal {
    aborted = false
    listeners = new Set()
    addEventListener(_name, listener) { this.listeners.add(listener) }
    removeEventListener(_name, listener) { this.listeners.delete(listener) }
    abort() { this.aborted = true; for (const listener of [...this.listeners]) listener() }
  }
  const first = new TrackingSignal()
  const model = createResourceModel({ load: async () => ({ ok: true }) })
  await model.request('complete', {}, { signal: first })
  assert.equal(first.listeners.size, 0, 'completed request must remove its external signal listener')

  const second = new TrackingSignal()
  const pending = deferred()
  const model2 = createResourceModel({ load: () => pending.promise })
  const request = model2.request('cancel', {}, { signal: second })
  second.abort()
  pending.resolve({ late: true })
  assert.equal((await request).status, 'aborted')
  assert.equal(second.listeners.size, 0, 'cancelled request must remove its external signal listener')
})

test('reusing a disposed key cannot be affected by the old late result', async () => {
  const pending = deferred()
  let generation = 0
  const model = createResourceModel({
    load: () => {
      generation += 1
      return generation === 1 ? pending.promise : Promise.resolve({ generation })
    },
  })
  const old = model.request('symbol', { id: 'old' })
  model.dispose('symbol')
  const fresh = model.request('symbol', { id: 'new' })
  assert.equal((await fresh).status, 'published')
  pending.resolve({ id: 'old-late' })
  assert.equal((await old).status, 'superseded')
  assert.deepEqual(model.get('symbol').getSnapshot().data, { generation: 2 })
})

test('dispose releases the external listener even if the load never settles', () => {
  const listeners = new Set()
  const signal = {
    aborted: false,
    addEventListener: (_name, listener) => listeners.add(listener),
    removeEventListener: (_name, listener) => listeners.delete(listener),
  }
  const model = createResourceModel({ load: () => new Promise(() => {}) })
  model.request('never', {}, { signal })
  assert.equal(listeners.size, 1)
  model.disposeAll()
  assert.equal(listeners.size, 0)
})

test('invalidate aborts current work, preserves the snapshot store and allows replay', async () => {
  const pending = deferred()
  let count = 0
  const model = createResourceModel({ load: () => ++count === 1 ? pending.promise : Promise.resolve('new') })
  const store = model.get('one')
  const old = model.request('one', {})
  model.invalidate('one')
  assert.equal(model.get('one'), store)
  assert.equal(store.getSnapshot().status, 'idle')
  assert.equal((await model.request('one', {})).status, 'published')
  pending.resolve('old')
  assert.equal((await old).status, 'superseded')
  assert.equal(store.getSnapshot().data, 'new')
})
