import test from 'node:test'
import assert from 'node:assert/strict'
import { createInspectorModel } from '../src/models/inspector.js'

const deferred = () => {
  let resolve, reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}
const symbol = id => ({ id, footprint: `${id}:old` })
const candidate = { id: 'fp-uuid', footprint_status: 'available' }
function setup(overrides = {}, refresh = async () => {}) {
  const calls = []
  const service = Object.fromEntries(['getBinding', 'getMatches', 'searchFootprints', 'searchModels', 'bindFootprint', 'bindModel'].map(method => [method, async (...args) => {
    calls.push({ method, args })
    if (overrides[method]) return overrides[method](...args)
    return method === 'getBinding' ? { footprint_id: 'fp-bound', footprint_name: 'old' }
      : method.startsWith('bind') ? { ok: true } : []
  }]))
  return { model: createInspectorModel({ service, refresh }), calls }
}

test('selection aborts old reads; reverse replies and clearing cannot restore old selection', async () => {
  const reads = [deferred(), deferred()]
  let index = 0
  const { model, calls } = setup({ getBinding: () => reads[index++].promise })
  const a = model.select(symbol('A'))
  const b = model.select(symbol('B'))
  assert.equal(calls[0].args.at(-1).signal.aborted, true)
  reads[1].resolve({ footprint_name: 'B' })
  await b
  reads[0].resolve({ footprint_name: 'A' })
  await a
  assert.equal(model.getSnapshot().binding.data.footprint_name, 'B')
  model.select(null)
  assert.equal(model.getSnapshot().selected, null)
  assert.equal(model.getSnapshot().binding.data, null)
})

test('binding reload failure preserves valid data and exposes the real error', async () => {
  let count = 0
  const { model } = setup({ getBinding: async () => {
    if (++count > 1) throw new Error('DB-DOWN')
    return { footprint_name: 'valid' }
  } })
  await model.select(symbol('A'))
  await model.refresh()
  assert.equal(model.getSnapshot().binding.data.footprint_name, 'valid')
  assert.equal(model.getSnapshot().binding.error.message, 'DB-DOWN')
})

test('search aborts superseded reads and closing blocks late success and failure', async () => {
  const requests = []
  const { model, calls } = setup({ searchFootprints: () => {
    const pending = deferred(); requests.push(pending); return pending.promise
  } })
  await model.select(symbol('A'))
  const first = model.openPicker('footprint')
  const second = model.search('new')
  const searches = calls.filter(c => c.method === 'searchFootprints')
  assert.equal(searches[0].args.at(-1).signal.aborted, true)
  requests[1].resolve([candidate]); await second
  requests[0].resolve([{ id: 'old' }]); await first
  assert.deepEqual(model.getSnapshot().search.data, [candidate])
  const third = model.search('late')
  model.closePicker()
  requests[2].reject(new Error('OLD'))
  await third
  assert.equal(model.getSnapshot().picker, null)
  assert.equal(model.getSnapshot().search.error, null)
})

test('write uses candidate identity once and refreshes once; invalid footprint is rejected', async () => {
  let refreshes = 0
  const pending = deferred()
  const { model, calls } = setup({ bindFootprint: () => pending.promise }, async () => { refreshes++ })
  await model.select(symbol('A'))
  await model.openPicker('footprint')
  assert.equal((await model.selectCandidate({ id: 'bad', footprint_status: 'missing' })).status, 'unavailable')
  const write = model.selectCandidate(candidate)
  assert.equal((await model.selectCandidate(candidate)).status, 'busy')
  pending.resolve({ ok: true })
  assert.equal((await write).status, 'published')
  assert.equal(calls.filter(c => c.method === 'bindFootprint').length, 1)
  assert.deepEqual(calls.find(c => c.method === 'bindFootprint').args, ['A', 'fp-uuid'])
  assert.equal(refreshes, 1)
  assert.equal(model.getSnapshot().picker, null)
})

test('reselection including same id and reopened picker isolate late writes', async () => {
  for (const transition of ['other', 'same-id', 'reopen', 'clear']) {
    let refreshes = 0
    const pending = deferred()
    const { model } = setup({ bindFootprint: () => pending.promise }, async () => { refreshes++ })
    await model.select(symbol('A')); await model.openPicker('footprint')
    const write = model.selectCandidate(candidate)
    if (transition === 'reopen') { model.closePicker(); await model.openPicker('footprint') }
    else await model.select(transition === 'clear' ? null : symbol(transition === 'same-id' ? 'A' : 'B'))
    pending.resolve({ ok: true })
    assert.equal((await write).status, 'superseded', transition)
    assert.equal(refreshes, 0, transition)
    assert.equal(model.getSnapshot().picker, transition === 'reopen' ? 'footprint' : null)
    model.dispose()
  }
})

test('successful write with failed refresh reports refresh failure without repeating write', async () => {
  let refreshes = 0
  const { model, calls } = setup({}, async () => { refreshes++; throw new Error('REFRESH-DOWN') })
  await model.select(symbol('A')); await model.openPicker('footprint')
  const outcome = await model.selectCandidate(candidate)
  assert.equal(outcome.status, 'refresh-failed')
  assert.equal(outcome.error.message, 'REFRESH-DOWN')
  assert.equal(calls.filter(c => c.method === 'bindFootprint').length, 1)
  assert.equal(refreshes, 1)
  assert.equal(model.getSnapshot().picker, null)
})

test('business rejection preserves picker, real error and releases submission lock', async () => {
  const { model } = setup({ bindFootprint: async () => ({ ok: false, error: 'READONLY' }) })
  await model.select(symbol('A')); await model.openPicker('footprint')
  const outcome = await model.selectCandidate(candidate)
  assert.equal(outcome.status, 'failed')
  assert.equal(outcome.error.message, 'READONLY')
  assert.equal(model.getSnapshot().picker, 'footprint')
  assert.equal(model.getSnapshot().busy, false)
})

test('model binding requires an actual footprint and uses its UUID', async () => {
  const { model, calls } = setup({ getBinding: async () => ({ footprint_id: 'bound-uuid' }) })
  await model.select(symbol('A')); await model.openPicker('model')
  assert.equal((await model.selectCandidate({ id: 'model-uuid' })).status, 'published')
  assert.deepEqual(calls.find(c => c.method === 'bindModel').args, ['bound-uuid', 'model-uuid'])
  const empty = setup({ getBinding: async () => ({}) }).model
  await empty.select(symbol('A')); await empty.openPicker('model')
  assert.equal((await empty.selectCandidate({ id: 'model-uuid' })).status, 'missing-footprint')
})

test('matches are lazy, clear on selection and reject late replies after close', async () => {
  const pending = deferred()
  const { model, calls } = setup({ getMatches: () => pending.promise })
  await model.select(symbol('A'))
  assert.equal(calls.filter(c => c.method === 'getMatches').length, 0)
  const matches = model.findMatches()
  model.closeMatches()
  pending.resolve([{ footprint_id: 'old' }]); await matches
  assert.equal(model.getSnapshot().matchesOpen, false)
  assert.equal(model.getSnapshot().matches.data, null)
})

test('StrictMode setup-cleanup-setup and 200 selections keep bounded reads and listeners', async () => {
  const { model, calls } = setup()
  let notifications = 0
  let off = model.subscribe(() => notifications++)
  await model.select(symbol('A'))
  off(); model.select(null)
  off = model.subscribe(() => notifications++)
  for (let i = 0; i < 200; i++) await model.select(symbol(String(i)))
  assert.equal(calls.filter(c => c.method === 'getBinding').length, 201)
  const before = notifications
  off(); model.dispose(); model.dispose()
  await model.select(symbol('after-dispose'))
  assert.equal(notifications, before)
  assert.equal(calls.filter(c => c.method === 'getBinding').length, 201)
})
