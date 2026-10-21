import test from 'node:test'
import assert from 'node:assert/strict'
import { createSnapshotStore } from '@kiforge/plugin-sdk'
import { createLcscModel } from '../../plugins/lcsc_import/frontend/src/model.js'

const deferred = () => { let resolve; const promise = new Promise(yes => { resolve = yes }); return { promise, resolve } }
const tick = () => new Promise(resolve => setImmediate(resolve))
const library = { id: '包-id', name: '元件库', symbol_path: 'C:/库/SYMS.KICAD_SYM', footprint_path: 'C:/库/封装' }
const catalog = { ok: true, can_create: false, libraries: [{ id: '', name: '空子库', path: '子目录/空.KICAD_SYM' }] }
const analyzed = id => ({ ok: true, plugin_result: { ok: true, lcsc_id: id, symbol_names: ['器件'], candidates: [], footprint_name: '封装' } })
function setup(overrides = {}) {
  const calls = []
  const api = { compLibraries: async () => [library], componentSymbolLibraries: async () => catalog,
    searchFootprints: async () => [],
    executePlugin: async (_plugin, body) => { calls.push(body); return body.action === 'analyze' ? analyzed(body.lcsc_id) : { ok: true, plugin_result: { ok: true, mode: 'new' } } },
    ...overrides.api,
  }
  const model = createLcscModel({ compLibraries: [library],
    pluginAction: { plugin: { id: 'lcsc' }, action: { name: 'Import', analyze_action: 'analyze' } },
    t: key => key, onRefresh: async () => {}, ...overrides, api })
  return { model, calls }
}
async function ready(model) {
  model.changePartNumber('C123'); await model.runLookup()
  model.selectTarget(library); model.confirmTarget(); await tick()
  model.selectSymbolLibrary({ mode: 'existing', path: '子目录/空.KICAD_SYM' }); model.confirmSymbolLibrary(); model.pickNew()
}

test('LCSC lookup drops reverse replies and synchronously prevents duplicate requests', async () => {
  const pending = [], { model } = setup({ api: { executePlugin: (_id, body) => { const p = deferred(); pending.push({ ...p, body }); return p.promise } } })
  model.changePartNumber('C1'); const a = model.runLookup(); await model.runLookup()
  assert.equal(pending.length, 1)
  model.changePartNumber('C2'); const b = model.runLookup()
  pending[1].resolve(analyzed('C2')); await b
  pending[0].resolve(analyzed('C1')); await a
  assert.equal(model.getSnapshot().device.id, 'C2')
  assert.equal(model.getSnapshot().looking, false)
  model.dispose()
})

test('LCSC preserves package/sub-library identity and retries only refresh after a committed write', async () => {
  let refreshes = 0
  const { model, calls } = setup({ onRefresh: async () => { if (++refreshes === 1) throw new Error('REFRESH-DOWN') } })
  await ready(model); await model.runImport()
  const imports = calls.filter(call => call.action === 'import')
  assert.equal(imports.length, 1)
  assert.equal(imports[0].target_component_library_id, '包-id')
  assert.equal(imports[0].symbol_library_choice.path, '子目录/空.KICAD_SYM')
  assert.equal(model.getSnapshot().completed, true)
  assert.equal(model.getSnapshot().refreshError, 'REFRESH-DOWN')
  await model.runImport(); await model.refreshAfterImport()
  assert.equal(calls.filter(call => call.action === 'import').length, 1)
  assert.equal(refreshes, 2)
  model.dispose()
})

test('LCSC candidate UUID and query guard isolate same-name candidates and late search', async () => {
  const pending = deferred()
  const { model } = setup({ api: { searchFootprints: () => pending.promise } })
  await ready(model)
  model.pickCandidate({ id: 'uuid-B', name: 'same', footprint_status: 'available' }, 'manual')
  assert.equal(model.getSnapshot().choice.id, 'uuid-B')
  const searching = model.runManualSearch('old')
  model.changePartNumber('C999')
  pending.resolve([{ id: 'uuid-A', name: 'same', footprint_status: 'available' }]); await searching
  assert.equal(model.getSnapshot().manualResults, null)
  assert.equal(model.getSnapshot().choice, null)
  model.dispose()
})

test('LCSC view subscriptions may close while the committed task and refresh finish', async () => {
  const writing = deferred()
  let refreshes = 0
  const { model } = setup({ api: { executePlugin: async (_id, body) => body.action === 'analyze' ? analyzed('C123') : writing.promise }, onRefresh: async () => { refreshes++ } })
  await ready(model)
  let updates = 0
  const off = model.subscribe(() => updates++)
  const pending = model.runImport(); await tick(); off()
  const before = updates
  writing.resolve({ ok: true, plugin_result: { ok: true } }); await pending
  assert.equal(updates, before)
  assert.equal(model.getSnapshot().completed, true)
  assert.equal(refreshes, 1)
  model.dispose()
})

test('LCSC suspend/replay invalidates old lookup; late dispose never publishes', async () => {
  const pending = deferred()
  const { model } = setup({ api: { executePlugin: () => pending.promise } })
  model.changePartNumber('C1'); const query = model.runLookup()
  model.suspend(); model.activate()
  pending.resolve(analyzed('C1')); await query
  assert.equal(model.getSnapshot().device, null)
  const snapshot = model.getSnapshot()
  model.dispose(); model.changePartNumber('C2')
  assert.equal(model.getSnapshot(), snapshot)
})

test('LCSC begins a new explicit import intent only after the committed write refresh succeeds', async () => {
  let refreshes = 0
  const { model, calls } = setup({ onRefresh: async () => { if (++refreshes === 1) throw new Error('REFRESH-DOWN') } })
  await ready(model); await model.runImport()
  const firstOperation = calls.find(call => call.action === 'import').operation_id
  assert.equal(typeof model.beginNewIntent, 'function')
  assert.equal(model.beginNewIntent(), false, 'A failed refresh must preserve the committed intent')
  assert.equal(model.getSnapshot().completed, true)
  await model.refreshAfterImport()
  const libraries = model.getSnapshot().compLibraries
  assert.equal(model.beginNewIntent(), true)
  const fresh = model.getSnapshot()
  assert.equal(fresh.completed, false)
  assert.equal(fresh.frozen, false)
  assert.equal(fresh.partNumber, '')
  assert.equal(fresh.device, null)
  assert.equal(fresh.target, null)
  assert.equal(fresh.choice, null)
  assert.equal(fresh.task, null)
  assert.equal(fresh.compLibraries, libraries)
  await ready(model); await model.runImport()
  const imports = calls.filter(call => call.action === 'import')
  assert.equal(imports.length, 2)
  assert.notEqual(imports[1].operation_id, firstOperation)
  model.dispose()
})

test('LCSC rejects late lookup handles so task display and cancellation follow the current request', async () => {
  const pending = []
  const { model } = setup({ api: { executePlugin: (_id, body, onTask) => {
    const request = deferred(); pending.push({ ...request, body, onTask }); return request.promise
  } } })
  function handle(id) {
    const store = createSnapshotStore({ id, action: 'analyze', state: 'running', cancelable: true })
    let cancellations = 0
    return { ...store, id, cancel: async () => { cancellations++ }, get cancellations() { return cancellations } }
  }
  model.changePartNumber('C1'); const first = model.runLookup()
  model.changePartNumber('C2'); const second = model.runLookup()
  const current = handle('C2'), stale = handle('C1')
  pending[1].onTask(current)
  assert.equal(model.getSnapshot().task.id, 'C2')
  pending[0].onTask(stale)
  assert.equal(model.getSnapshot().task.id, 'C2')
  await model.cancelTask()
  assert.equal(current.cancellations, 1)
  assert.equal(stale.cancellations, 0)
  pending[1].resolve(analyzed('C2')); await second
  pending[0].resolve(analyzed('C1')); await first
  assert.equal(model.getSnapshot().device.id, 'C2')
  assert.equal(model.getSnapshot().task.id, 'C2')
  model.dispose()
})

test('LCSC releases synchronous terminal subscriptions and ignores old import handles after reset', async () => {
  let subscriptions = 0, oldImportCallback
  const task = { id: 'instant', action: 'import', state: 'succeeded', cancelable: false }
  const handle = { id: task.id, getSnapshot: () => task, subscribe(listener) {
    subscriptions++; listener()
    let released = false
    return () => { if (!released) { released = true; subscriptions-- } }
  } }
  const { model } = setup({ api: { executePlugin: async (_id, body, onTask) => {
    if (body.action === 'analyze') return analyzed(body.lcsc_id)
    oldImportCallback = onTask
    onTask(handle)
    return { ok: true, plugin_result: { ok: true } }
  } } })
  await ready(model); await model.runImport()
  assert.equal(subscriptions, 0)
  assert.equal(model.getSnapshot().task.id, 'instant')
  assert.equal(model.beginNewIntent(), true)
  oldImportCallback(handle)
  assert.equal(model.getSnapshot().task, null)
  assert.equal(subscriptions, 0)
  model.dispose(); oldImportCallback(handle)
  assert.equal(subscriptions, 0)
})

test('LCSC keeps the committed intent frozen until an in-flight refresh finishes', async () => {
  const refresh = deferred()
  const { model } = setup({ onRefresh: () => refresh.promise })
  await ready(model)
  const writing = model.runImport(); await tick()
  assert.equal(model.getSnapshot().completed, true)
  assert.equal(model.getSnapshot().refreshing, true)
  assert.equal(model.beginNewIntent(), false)
  refresh.resolve(); await writing
  assert.equal(model.beginNewIntent(), true)
  model.dispose()
})

test('LCSC keeps no-op snapshot references and does not restart reads after disposal', async () => {
  let reads = 0, updates = 0
  const { model } = setup({ api: { compLibraries: async () => { reads++; return [] } } })
  const off = model.subscribe(() => { updates++; model.getSnapshot() })
  model.changePartNumber('C1')
  assert.equal(updates, 1)
  const snapshot = model.getSnapshot()
  model.changePartNumber('C1')
  assert.equal(model.getSnapshot(), snapshot)
  assert.equal(updates, 1)
  off(); model.dispose(); await model.start()
  assert.equal(reads, 0)
  assert.equal(model.getSnapshot(), snapshot)
})
