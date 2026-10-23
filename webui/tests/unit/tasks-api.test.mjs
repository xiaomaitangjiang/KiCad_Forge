import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createTasksApi, createImportsApi } from '../../src/plugin-host/tasks-api.js'

function fakeFetch(handler) {
  const calls = []
  const fetchImpl = async (path, options) => {
    calls.push({ path, options })
    const result = await handler(path, options)
    return {
      ok: result.status >= 200 && result.status < 300,
      status: result.status,
      async json() {
        return result.body
      },
    }
  }
  return { fetchImpl, calls }
}

test('task client injects the owning plugin identity and never accepts another id', async (t) => {
  const { fetchImpl, calls } = fakeFetch(() => ({ status: 200, body: { ok: true, task_id: 't1' } }))
  const tasks = createTasksApi('plugin.one', fetchImpl)
  t.after(() => tasks.dispose())
  await tasks.run('import', { target: 'lib' })
  assert.equal(calls[0].path, '/api/tasks')
  assert.deepEqual(JSON.parse(calls[0].options.body), {
    plugin_id: 'plugin.one',
    action: 'import',
    args: { target: 'lib' },
  })
})

test('task get/cancel/list use encoded ids', async () => {
  const { fetchImpl, calls } = fakeFetch((path) => ({ status: 200, body: { ok: true, path } }))
  const tasks = createTasksApi('plugin/one', fetchImpl)
  await tasks.get('task 1')
  await tasks.cancel('task 1')
  await tasks.list()
  assert.equal(calls[0].path, '/api/tasks/task%201')
  assert.equal(calls[1].path, '/api/tasks/task%201/cancel')
  assert.equal(calls[2].path, '/api/plugins/plugin%2Fone/tasks')
  assert.equal(calls[1].options.method, 'POST')
})

test('operation lookup is read-only and surfaces host errors', async () => {
  const { fetchImpl, calls } = fakeFetch((path) =>
    path.endsWith('/missing')
      ? { status: 404, body: { ok: false, error: 'operation_not_found' } }
      : { status: 200, body: { ok: true, operation_id: 'op' } },
  )
  const tasks = createTasksApi('plugin.one', fetchImpl)
  assert.deepEqual(await tasks.operation('op-1'), { ok: true, operation_id: 'op' })
  await assert.rejects(
    () => tasks.operation('missing'),
    (error) => error.status === 404 && error.body?.error === 'operation_not_found',
  )
  assert.equal(calls[0].path, '/api/imports/operations/op-1')
})

test('cancel statuses are passed through without local state invention', async () => {
  const { fetchImpl } = fakeFetch(() => ({
    status: 200,
    body: { ok: true, status: 'not_cancelable' },
  }))
  const tasks = createTasksApi('plugin.one', fetchImpl)
  assert.equal((await tasks.cancel('t1')).status, 'not_cancelable')
})

test('get and list expose public task snapshots rather than HTTP DTOs', async () => {
  const wire = { task_id: 't1', action: 'analyze', state: 'succeeded', cancelable: false,
    result: { ok: true }, cancel_requested: true }
  const { fetchImpl } = fakeFetch((path) => ({ status: 200,
    body: path.endsWith('/tasks') ? { ok: true, tasks: [wire] } : { ok: true, ...wire } }))
  const tasks = createTasksApi('owner', fetchImpl)
  const snapshot = await tasks.get('t1')
  assert.equal(snapshot.id, 't1')
  assert.equal(snapshot.cancelRequested, true)
  assert.equal('task_id' in snapshot, false)
  const list = await tasks.list()
  assert.ok(Array.isArray(list))
  assert.deepEqual(list, [snapshot])
})

test('get returns null only for a missing task and preserves other host errors', async () => {
  const { fetchImpl } = fakeFetch((path) => path.endsWith('/missing')
    ? { status: 404, body: { error_code: 'task_not_found', error: 'Task not found' } }
    : { status: 503, body: { error_code: 'unavailable', error: 'Service unavailable' } })
  const tasks = createTasksApi('owner', fetchImpl)
  assert.equal(await tasks.get('missing'), null)
  await assert.rejects(() => tasks.get('other'),
    (error) => error.status === 503 && error.code === 'unavailable')
})

const provider = 'com.kicad_forge.lcsc_import'

function importFixture({ initialState = 'succeeded', analysisFailure = false } = {}) {
  const records = new Map()
  const analysis = { ok: true, lcsc_id: 'C123', symbol_names: ['Probe'],
    footprint_name: 'Probe_FP', model_name: 'Probe_3D',
    candidates: [{ id: 'fp-1', name: 'Probe_FP', library: 'Package', pad_count: 8,
      footprint_status: 'stale', footprint_reason: 'missing_file' }], warning: '' }
  const result = { ok: true, noop: true, replayed: true, operation_id: 'op-1', lcsc_id: 'C123',
    symbol_id: 'sym-1', symbol_name: 'Probe', footprint_id: 'fp-1',
    footprint_reference: 'Package:Probe_FP', mode: 'reuse', created_library: false, warnings: ['notice'] }
  const fixture = fakeFetch((path, options) => {
    if (path === '/api/tasks') {
      const request = JSON.parse(options.body)
      const id = `t${records.size + 1}`
      const record = { task_id: id, action: request.action, state: initialState, cancelable: false,
        result: request.action === 'analyze' ? analysis : result }
      if (analysisFailure && request.action === 'analyze') {
        record.state = 'failed'
        record.result = { ok: false, error: 'conversion failed' }
        record.error = 'conversion failed'
      }
      records.set(id, record)
      return { status: 200, body: { ok: true, ...record } }
    }
    if (path.startsWith('/api/imports/operations/')) {
      return path.endsWith('/missing')
        ? { status: 404, body: { error_code: 'operation_not_found', error: 'Not found' } }
        : { status: 200, body: { ok: true, operation_id: 'op-1', result } }
    }
    const record = records.get(path.split('/')[3])
    if (record) return { status: 200, body: { ok: true, ...record, state: 'succeeded' } }
    throw new Error(`Unexpected fixture request: ${path}`)
  })
  const tasks = createTasksApi('community.consumer', fixture.fetchImpl)
  const imports = createImportsApi(tasks, fixture.fetchImpl)
  return { ...fixture, tasks, imports }
}

test('imports analyze selects the provider and normalizes the real Python response', async (t) => {
  const fixture = importFixture()
  t.after(() => { fixture.tasks.dispose?.(); fixture.imports.dispose?.() })
  const analysis = await fixture.imports.analyze({ provider, externalId: 'C123' })
  assert.deepEqual(JSON.parse(fixture.calls[0].options.body), {
    plugin_id: provider, action: 'analyze', args: { lcsc_id: 'C123' },
  })
  assert.deepEqual(analysis, { externalId: 'C123', symbolNames: ['Probe'], footprintName: 'Probe_FP',
    modelName: 'Probe_3D', candidates: [{ footprintId: 'fp-1', name: 'Probe_FP', library: 'Package', padCount: 8,
      footprintStatus: 'stale', reason: 'missing_file' }], warning: '' })
  await fixture.tasks.run('status')
  assert.equal(JSON.parse(fixture.calls.at(-1).options.body).plugin_id, 'community.consumer',
    'Imports provider selection cannot change the caller tasks identity')
})

for (const symbolLibraryChoice of [{ mode: 'new', name: 'Device' }, { mode: 'existing', path: 'sub/Device.kicad_sym' }]) {
  for (const footprintChoice of [{ mode: 'reuse', footprintId: 'fp-1' }, { mode: 'new' }]) {
    test(`imports commit maps ${symbolLibraryChoice.mode} symbol library / ${footprintChoice.mode} footprint and every result field`, async (t) => {
      const fixture = importFixture()
      t.after(() => { fixture.tasks.dispose?.(); fixture.imports.dispose?.() })
      const handle = await fixture.imports.commit({ provider, externalId: 'C123',
        targetComponentLibraryId: 'pkg-1', symbolLibraryChoice, footprintChoice, operationId: 'op-1' })
      assert.deepEqual(JSON.parse(fixture.calls[0].options.body), {
        plugin_id: provider, action: 'import', args: {
          lcsc_id: 'C123', target_component_library_id: 'pkg-1',
          symbol_library_choice: symbolLibraryChoice,
          footprint_choice: footprintChoice.mode === 'reuse'
            ? { mode: 'reuse', footprint_id: 'fp-1' } : { mode: 'new' }, operation_id: 'op-1',
        },
      })
      const snapshot = handle.getSnapshot()
      assert.equal(snapshot.id, 't1')
      assert.equal(typeof handle.subscribe, 'function')
      assert.equal(typeof handle.cancel, 'function')
      assert.equal((await handle.done), snapshot, 'done and getSnapshot expose the same mapped result')
      assert.deepEqual(snapshot.result, { ok: true, noop: true, replayed: true, operationId: 'op-1', externalId: 'C123',
        symbolId: 'sym-1', symbolName: 'Probe', footprintId: 'fp-1',
        footprintReference: 'Package:Probe_FP', mode: 'reuse', createdLibrary: false, warnings: ['notice'] })
    })
  }
}

test('imports operation uses the same public result mapping and returns null for an unknown operation', async (t) => {
  const fixture = importFixture()
  t.after(() => fixture.imports.dispose?.())
  const result = await fixture.imports.operation('op-1')
  assert.equal(result.operationId, 'op-1')
  assert.equal(result.footprintReference, 'Package:Probe_FP')
  assert.equal('operation_id' in result, false)
  assert.equal(await fixture.imports.operation('missing'), null)
})

test('imports analyze propagates failed task results', async (t) => {
  const fixture = importFixture({ analysisFailure: true })
  t.after(() => fixture.imports.dispose?.())
  await assert.rejects(() => fixture.imports.analyze({ provider, externalId: 'C123' }), /conversion failed/)
})

test('disposing a task client stops its handles and rejects new work', async (t) => {
  const fixture = importFixture({ initialState: 'running' })
  const handle = await fixture.tasks.run('status')
  t.after(() => handle.dispose?.())
  let published = 0
  handle.subscribe(() => { published += 1 })
  fixture.tasks.dispose()
  await new Promise((resolve) => setTimeout(resolve, 20))
  assert.equal(fixture.calls.length, 1, 'No pending timer may read task state after disposal')
  assert.equal(published, 0)
  await assert.rejects(() => fixture.tasks.run('status'), /disposed/)
})

test('imports normalize polled results before notifying subscribers and settling done', async (t) => {
  const fixture = importFixture({ initialState: 'queued' })
  t.after(() => fixture.imports.dispose())
  const handle = await fixture.imports.commit({ provider, externalId: 'C123',
    targetComponentLibraryId: 'pkg', symbolLibraryChoice: { mode: 'new', name: 'Device' },
    footprintChoice: { mode: 'reuse', footprintId: 'fp-1' } })
  const snapshots = []
  handle.subscribe(() => snapshots.push(handle.getSnapshot()))
  // A bounded watchdog keeps Node alive while the implementation's polling
  // timer remains unref'ed, and makes a broken settlement fail explicitly.
  let watchdog
  const terminal = await Promise.race([handle.done, new Promise((_, reject) => {
    watchdog = setTimeout(() => reject(new Error('Task did not reach its terminal state')), 1000)
  })]).finally(() => clearTimeout(watchdog))
  assert.equal(terminal.state, 'succeeded')
  assert.equal(terminal.result.operationId, 'op-1')
  assert.equal(terminal.result.footprintId, 'fp-1')
  assert.equal(snapshots.at(-1), terminal)
  assert.equal(handle.getSnapshot(), terminal)
})

test('disposal aborts an in-flight task read and ignores its late result', async (t) => {
  let resolveRead
  let readSignal
  let observedRead
  const started = new Promise((resolve) => { observedRead = resolve })
  const { fetchImpl } = fakeFetch(async (path, options) => {
    if (path === '/api/tasks') return { status: 200,
      body: { ok: true, task_id: 'slow', state: 'running' } }
    readSignal = options.signal
    observedRead()
    return new Promise((resolve) => { resolveRead = resolve })
  })
  const tasks = createTasksApi('owner', fetchImpl)
  t.after(() => tasks.dispose())
  const handle = await tasks.run('status')
  let notifications = 0
  handle.subscribe(() => { notifications += 1 })
  let watchdog
  await Promise.race([started, new Promise((_, reject) => {
    watchdog = setTimeout(() => reject(new Error('Task polling did not start')), 1000)
  })]).finally(() => clearTimeout(watchdog))
  tasks.dispose()
  assert.equal(readSignal.aborted, true)
  resolveRead({ status: 200, body: { task_id: 'slow', state: 'succeeded' } })
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(notifications, 0)
  assert.equal(handle.getSnapshot().state, 'running')
})
