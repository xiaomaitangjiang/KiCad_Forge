import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createTaskApi, waitTask } from '../../../plugins/lcsc_import/frontend/src/task-adapter.js'
import { parseSymbolLibraries, validateTargetLibrary } from '../../../plugins/lcsc_import/frontend/src/lcscImport.js'

function fakeTasks({ states = ['running', 'succeeded'], result = { ok: true }, error = '' } = {}) {
  const calls = []
  return {
    calls,
    async run(action, args) {
      calls.push({ action, args })
      const finalState = states[states.length - 1]
      return {
        id: 'task-1',
        getSnapshot: () => ({ id: 'task-1', state: finalState, result, error }),
        subscribe: () => () => {},
        cancel: async () => ({ status: 'not_cancelable' }),
        done: Promise.resolve({ id: 'task-1', state: finalState, result, error }),
      }
    },
    async get() {
      return { task_id: 'task-1', state: states[states.length - 1], result, error }
    },
  }
}

test('adapter strips action and forwards the rest as task args', async () => {
  const tasks = fakeTasks()
  const api = createTaskApi({ tasks, libraries: {}, footprints: {} })
  const wrapper = await api.executePlugin('plugin', {
    action: 'analyze',
    lcsc_id: 'C123',
    target_component_library_id: 'pkg',
  })
  assert.deepEqual(tasks.calls[0], {
    action: 'analyze',
    args: { lcsc_id: 'C123', target_component_library_id: 'pkg' },
  })
  assert.deepEqual(wrapper, { ok: true, plugin_result: { ok: true } })
})

test('legacy wait helper follows SDK snapshots using id on every poll', async () => {
  const requested = []
  const tasks = {
    async get(id) {
      requested.push(id)
      assert.equal(id, 'sdk-task')
      return { id, state: requested.length > 1 ? 'succeeded' : 'running' }
    },
  }
  const result = await waitTask(tasks, { id: 'sdk-task', state: 'queued' })
  assert.equal(result.state, 'succeeded')
  assert.deepEqual(requested, ['sdk-task', 'sdk-task'])
})

test('failed and cancelled tasks never look successful', async () => {
  for (const state of ['failed', 'cancelled']) {
    const tasks = fakeTasks({ states: [state], error: `${state} reason` })
    const api = createTaskApi({ tasks, libraries: {}, footprints: {} })
    const wrapper = await api.executePlugin('plugin', { action: 'import', lcsc_id: 'C1' })
    assert.equal(wrapper.ok, false)
    assert.match(wrapper.error, new RegExp(state))
  }
})

test('task submission errors are surfaced as plugin errors', async () => {
  const tasks = { run: async () => { throw new Error('plugin_disabled') }, get: async () => null }
  const api = createTaskApi({ tasks, libraries: {}, footprints: {} })
  const wrapper = await api.executePlugin('plugin', { action: 'import' })
  assert.deepEqual(wrapper, { ok: false, error: 'plugin_disabled' })
})

test('symbol library normalization preserves snake_case parse shape', async () => {
  const libraries = {
    async componentSymbolLibraries() {
      return { libraries: [{ name: 'Device', path: 'Device.kicad_sym' }], canCreate: false }
    },
  }
  const api = createTaskApi({ tasks: {}, libraries, footprints: {} })
  const value = await api.componentSymbolLibraries('pkg')
  assert.equal(value.can_create, false)
  assert.ok(Array.isArray(value.libraries))
  assert.equal(parseSymbolLibraries(value).ok, true)
})

test('SDK catalog bridge preserves single-file limits and unregistered empty ids', async () => {
  const row = { id: '', name: '空库', path: '目录/空库.kicad_sym' }
  const api = createTaskApi({ tasks: {}, footprints: {}, libraries: {
    componentSymbolLibraries: async () => ({ libraries: [row], canCreate: false, caseSensitive: false }),
  } })
  const catalog = parseSymbolLibraries(await api.componentSymbolLibraries('pkg'))
  assert.deepEqual(catalog, { ok: true, libraries: [row], canCreate: false, caseSensitive: false })
  assert.equal(catalog.libraries[0].id, '')
})

test('SDK component library bridge keeps target validation and false flags correct', async () => {
  const value = { id: 'pkg', name: 'Components', symbolPath: 'D:/组件/库.kicad_sym',
    footprintPath: 'D:/组件/库.pretty', enabled: true, locked: false, isProtected: false }
  const before = structuredClone(value)
  const api = createTaskApi({ tasks: {}, footprints: {}, libraries: {
    componentLibraries: async () => [value],
  } })
  const [legacy] = await api.compLibraries()
  assert.equal(validateTargetLibrary(legacy).ok, true)
  assert.equal(legacy.symbol_path, value.symbolPath)
  assert.equal(legacy.footprint_path, value.footprintPath)
  assert.equal(legacy.is_protected, false)
  assert.deepEqual(value, before)
})

test('SDK footprint bridge restores selectable legacy identities and unavailable reasons', async () => {
  const value = { footprintId: 'fp-1', name: 'SOIC', padCount: 0,
    footprintStatus: 'stale', reason: 'missing_file', score: 0 }
  const before = structuredClone(value)
  const api = createTaskApi({ tasks: {}, libraries: {}, footprints: {
    search: async (...args) => { assert.deepEqual(args, ['SOIC', 'sym-1']); return [value] },
  } })
  const [legacy] = await api.searchFootprints('SOIC', 'sym-1')
  assert.equal(legacy.id, 'fp-1')
  assert.equal(legacy.pad_count, 0)
  assert.equal(legacy.footprint_status, 'stale')
  assert.equal(legacy.footprint_reason, 'missing_file')
  assert.deepEqual(value, before)
})

test('adapter exposes the task handle before the final result (UI phase/cancelable)', async () => {
  const tasks = fakeTasks({ states: ['running', 'succeeded'] })
  const api = createTaskApi({ tasks, libraries: {}, footprints: {} })
  let seen = null
  const wrapper = await api.executePlugin('plugin', { action: 'import', lcsc_id: 'C1' }, (handle) => {
    seen = handle.getSnapshot()
  })
  assert.equal(seen?.id, 'task-1')
  assert.equal(wrapper.ok, true)
})

function standardHandle() {
  let snapshot = { id: 'public-task', action: 'import', state: 'running', cancelable: true }
  const listeners = new Set()
  return {
    id: snapshot.id,
    getSnapshot: () => snapshot,
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener) },
    cancel: async () => ({ status: 'cancel_requested' }),
    publish(value) { snapshot = { ...snapshot, ...value }; for (const notify of listeners) notify() },
    listenerCount: () => listeners.size,
  }
}

test('adapter waits through the public TaskHandle subscription without internal done', async () => {
  const handle = standardHandle()
  const api = createTaskApi({ tasks: { run: async () => handle }, libraries: {}, footprints: {} })
  let completed = false
  const result = api.executePlugin('plugin', { action: 'import' }).then((value) => { completed = true; return value })
  await Promise.resolve()
  await Promise.resolve()
  assert.equal(completed, false)
  assert.equal(handle.listenerCount(), 1)
  handle.publish({ state: 'succeeded', result: { ok: true, symbol_id: 'sym-1' } })
  assert.deepEqual(await result, { ok: true, plugin_result: { ok: true, symbol_id: 'sym-1' } })
  assert.equal(handle.listenerCount(), 0)
})

test('releasing the bridge removes pending subscriptions without cancelling a submitted task', async () => {
  const handle = standardHandle()
  let cancelled = false
  handle.cancel = async () => { cancelled = true }
  const api = createTaskApi({ tasks: { run: async () => handle }, libraries: {}, footprints: {} })
  const pending = api.executePlugin('plugin', { action: 'import' })
  await Promise.resolve()
  assert.equal(handle.listenerCount(), 1)
  api.dispose()
  assert.equal(handle.listenerCount(), 0)
  assert.equal((await pending).ok, false)
  assert.equal(cancelled, false)
})

test('bridge cleanup tolerates React effect replay without reviving earlier submissions', async () => {
  const handle = standardHandle()
  let releaseSubmission
  let submissions = 0
  const api = createTaskApi({ libraries: {}, footprints: {}, tasks: {
    run: async () => {
      submissions += 1
      if (submissions === 1) await new Promise((resolve) => { releaseSubmission = resolve })
      return handle
    },
  } })
  const earlier = api.executePlugin('plugin', { action: 'import' })
  api.dispose()
  releaseSubmission()
  assert.equal((await earlier).ok, false)
  assert.equal(handle.listenerCount(), 0)
  handle.publish({ state: 'succeeded', result: { ok: true } })
  assert.equal((await api.executePlugin('plugin', { action: 'import' })).ok, true)
})

test('waitTask stops at a terminal state', async () => {
  let calls = 0
  const tasks = {
    async get() {
      calls += 1
      return { task_id: 't', state: calls > 1 ? 'succeeded' : 'running', result: { n: calls } }
    },
  }
  const final = await waitTask(tasks, { task_id: 't', state: 'running' })
  assert.equal(final.state, 'succeeded')
})
