import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createPluginLoader } from '../src/plugin-host/loader.js'
import { createSlotHost } from '../src/plugin-host/slot-host.js'

let fixtureSequence = 0

// These fixtures deliberately use only the public SDK context and manifest
// contracts. Adding Cordis inject/provide would hide SDK activation defects.
async function loadFixture(t, { providerEnabled = true, request = null, probeRequires = [], importActions = null } = {}) {
  const stateKey = `__kfPublicSdkBehavior${++fixtureSequence}`
  const state = { generation: 0, activations: [], disposals: 0, context: null, calls: [] }
  globalThis[stateKey] = state
  const previousLocation = Object.getOwnPropertyDescriptor(globalThis, 'location')
  Object.defineProperty(globalThis, 'location', {
    configurable: true,
    value: { origin: 'http://sdk-fixture.invalid' },
  })

  function descriptor(id, source, contracts = {}, enabled = true) {
    // The loader appends ?v=...; a final comment keeps that suffix inert for
    // Node's data: imports. No files or real network requests are involved.
    const code = `const state = globalThis[${JSON.stringify(stateKey)}];\n${source}\n// `
    return {
      id,
      name: id,
      version: '1.0.0',
      enabled,
      engines: { sdk: '^1.0.0' },
      frontend: {
        entry_url: `data:text/javascript,${encodeURIComponent(code)}`,
        ...contracts,
      },
    }
  }

  const manifests = [
    descriptor('sdk.consumer', `
      export default {
        name: 'sdk.consumer',
        apply(ctx) {
          const service = ctx.contracts.require('community.sdk.lookup', '^1.0.0');
          state.activations.push(service);
          ctx.on('dispose', () => { state.disposals += 1; });
        },
      };
    `, { requires: [{ contract: 'community.sdk.lookup', range: '^1.0.0' }] }),
    descriptor('sdk.provider', `
      export default {
        name: 'sdk.provider',
        apply(ctx) {
          ctx.contracts.provide('community.sdk.lookup', '1.0.0', {
            generation: ++state.generation,
          });
        },
      };
    `, { provides: [{ contract: 'community.sdk.lookup', version: '1.0.0' }] }, providerEnabled),
    descriptor('sdk.probe', `
      export default {
        name: 'sdk.probe',
        apply(ctx) { state.context = ctx; },
      };
    `, { requires: probeRequires }),
  ]
  if (importActions) manifests.push({ id: 'sdk.python-import', name: 'Python importer', actions: importActions })

  const host = createSlotHost()
  const disposeShell = host.registerShell()
  const loader = createPluginLoader({
    host,
    fetchImpl: async (url, options = {}) => {
      state.calls.push({ url, options })
      let body
      if (url === '/api/plugin-diagnostics') body = []
      else if (url === '/api/plugins') body = manifests
      else if (url.startsWith('/api/plugin-config/')) body = { config: {} }
      else if (request) body = await request(url, options)
      else throw new Error(`Unexpected SDK fixture request: ${url}`)
      return { ok: true, status: 200, json: async () => body }
    },
  })

  t.after(async () => {
    try {
      await loader.dispose()
    } finally {
      disposeShell()
      delete globalThis[stateKey]
      if (previousLocation) Object.defineProperty(globalThis, 'location', previousLocation)
      else delete globalThis.location
    }
  })

  await loader.loadAll()
  // Flush dependency effects without depending on timers or wall-clock speed.
  await settle()
  return { loader, host, state, record: (id) => loader.getSnapshot().find((item) => item.id === id) }
}

async function settle() {
  for (let index = 0; index < 5; index += 1) {
    await new Promise((resolve) => setImmediate(resolve))
  }
}

test('public SDK: manifest dependency waits and activates when its provider appears', async (t) => {
  const { loader, state, record } = await loadFixture(t, { providerEnabled: false })
  assert.equal(record('sdk.consumer').status, 'waiting', 'A missing service is a waiting dependency')
  assert.equal(state.activations.length, 0)

  await loader.setEnabled('sdk.provider', true)
  await settle()
  assert.equal(record('sdk.consumer').status, 'active')
  assert.equal(state.activations.length, 1)
  assert.equal(state.activations[0].generation, 1)
})

test('public SDK: withdrawing a contract provider disposes its manifest consumer', async (t) => {
  const { loader, state, record } = await loadFixture(t)
  assert.equal(record('sdk.consumer').status, 'active')
  assert.equal(state.activations.length, 1)

  await loader.setEnabled('sdk.provider', false)
  await settle()
  assert.equal(state.disposals, 1, 'Consumer cleanup must run when its required provider withdraws')
  assert.equal(record('sdk.consumer').status, 'waiting')
})

test('public SDK: restoring a provider reactivates consumers with the new service instance', async (t) => {
  const { loader, state } = await loadFixture(t)
  const firstService = state.activations[0]
  assert.equal(firstService.generation, 1)

  await loader.setEnabled('sdk.provider', false)
  await loader.setEnabled('sdk.provider', true)
  await settle()
  assert.equal(state.generation, 2)
  assert.equal(state.activations.length, 2, 'The consumer must activate again after its dependency returns')
  assert.notEqual(state.activations[1], firstService)
  assert.equal(state.activations[1].generation, 2)
})

test('public SDK: tasks.run returns the declared TaskHandle rather than the wire response', async (t) => {
  const { state } = await loadFixture(t, {
    request: (url, options) => {
      if (url === '/api/tasks') {
        assert.equal(options.method, 'POST')
        return { ok: true, task_id: 'sdk-task-1', state: 'queued' }
      }
      if (url === '/api/tasks/sdk-task-1') {
        return { ok: true, task_id: 'sdk-task-1', action: 'status', state: 'queued', cancelable: true }
      }
      if (url === '/api/tasks/sdk-task-1/cancel') {
        assert.equal(options.method, 'POST')
        return { ok: true, status: 'cancel_requested', state: 'queued' }
      }
      throw new Error(`Unexpected task fixture request: ${url}`)
    },
  })
  const handle = await state.context.tasks.run('status', { short: true })
  assert.deepEqual({
    id: handle.id,
    getSnapshot: typeof handle.getSnapshot,
    subscribe: typeof handle.subscribe,
    cancel: typeof handle.cancel,
  }, {
    id: 'sdk-task-1',
    getSnapshot: 'function',
    subscribe: 'function',
    cancel: 'function',
  })
  const submission = state.calls.find((call) => call.url === '/api/tasks')
  assert.equal(JSON.parse(submission.options.body).plugin_id, 'sdk.probe')

  const snapshot = handle.getSnapshot()
  assert.equal(snapshot.id, handle.id)
  assert.equal(handle.getSnapshot(), snapshot, 'A task snapshot must remain stable until its state changes')
  const unsubscribe = handle.subscribe(() => {})
  assert.equal(typeof unsubscribe, 'function')
  unsubscribe()
  unsubscribe()
  assert.equal((await handle.cancel()).status, 'cancel_requested')
  assert.ok(state.calls.some((call) => call.url === '/api/tasks/sdk-task-1/cancel'))
})

for (const method of ['analyze', 'commit', 'cancel', 'operation']) {
  test(`public SDK: imports.${method} is available on an activated plugin context`, async (t) => {
    const { state } = await loadFixture(t)
    assert.equal(typeof state.context.imports[method], 'function', `ImportsContract declares ${method}()`)
  })
}

for (const contract of ['kiforge.tasks', 'kiforge.imports']) {
  test(`public SDK: identity-bound ${contract} requires give a clear diagnostic`, async (t) => {
    const { record, state, host } = await loadFixture(t, {
      probeRequires: [{ contract, range: '^1.0.0' }],
    })
    assert.equal(record('sdk.probe').status, 'error')
    assert.equal(state.context, null)
    assert.ok(host.diagnostics.some((item) => item.code === 'contract/per-plugin-only'))
  })
}

test('public SDK: imports use declared staged-provider actions including Python-only providers', async (t) => {
  const { state } = await loadFixture(t, {
    importActions: [{ id: 'stage_part', protocol: 'staged-import-v1', analyze_action: 'inspect_part' },
      { id: 'inspect_part' }],
    request: (url, options) => {
      if (url === '/api/tasks') {
        const body = JSON.parse(options.body)
        return { ok: true, task_id: 'import-task', action: body.action, state: 'succeeded',
          result: body.action === 'inspect_part'
            ? { ok: true, lcsc_id: 'C123', symbol_names: ['Probe'] }
            : { ok: true, operation_id: 'op-1', symbol_id: 'sym-1' } }
      }
      throw new Error(`Unexpected request: ${url}`)
    },
  })
  const api = state.context.imports
  assert.equal((await api.analyze({ provider: 'sdk.python-import', externalId: 'C123' })).externalId, 'C123')
  const handle = await api.commit({ provider: 'sdk.python-import', externalId: 'C123',
    targetComponentLibraryId: 'pkg', symbolLibraryChoice: { mode: 'new', name: 'Device' },
    footprintChoice: { mode: 'new' } })
  assert.equal(handle.getSnapshot().result.operationId, 'op-1')
  const submissions = state.calls.filter((item) => item.url === '/api/tasks').map((item) => JSON.parse(item.options.body))
  assert.deepEqual(submissions.map(({ plugin_id, action }) => ({ plugin_id, action })), [
    { plugin_id: 'sdk.python-import', action: 'inspect_part' },
    { plugin_id: 'sdk.python-import', action: 'stage_part' },
  ])
  assert.equal(submissions[1].args.provider, undefined, 'Plugin routing identity must not change the host import-source label')
})

for (const actions of [[], [{ id: 'import' }],
  [{ id: 'a', protocol: 'staged-import-v1' }, { id: 'b', protocol: 'staged-import-v1' }],
  [{ id: 'import', protocol: 'staged-import-v1', analyze_action: 'absent' }]]) {
  test(`public SDK: invalid import provider actions are rejected before task submission (${JSON.stringify(actions)})`, async (t) => {
    const { state } = await loadFixture(t, { importActions: actions })
    await assert.rejects(() => state.context.imports.analyze({ provider: 'sdk.python-import', externalId: 'C123' }),
      /staged|analyze/i)
    assert.equal(state.calls.some((call) => call.url === '/api/tasks'), false)
  })
}

test('public SDK: disabling a plugin disposes its pending task handles', async (t) => {
  const { state, loader } = await loadFixture(t, {
    request: (url) => {
      if (url === '/api/tasks') return { ok: true, task_id: 'pending-task', state: 'running' }
      if (url === '/api/tasks/pending-task') return { ok: true, task_id: 'pending-task', state: 'running' }
      throw new Error(`Unexpected request: ${url}`)
    },
  })
  const tasks = state.context.tasks
  await tasks.run('status')
  await loader.setEnabled('sdk.probe', false)
  await assert.rejects(() => tasks.run('status'), /disposed/)
})

test('public SDK: model.createStore shares stable snapshots and releases subscriptions', async (t) => {
  const { state } = await loadFixture(t)
  assert.equal(typeof state.context.model.createStore, 'function')
  const initial = { count: 0 }
  const store = state.context.model.createStore(initial)
  assert.equal(store.getSnapshot(), initial)
  assert.equal(store.getSnapshot(), store.getSnapshot())
  let notifications = 0
  const off = store.subscribe(() => { notifications += 1 })
  store.set(initial)
  assert.equal(notifications, 0, 'The same snapshot reference does not notify')
  store.update((current) => ({ count: current.count + 1 }))
  assert.equal(store.getSnapshot().count, 1)
  assert.equal(notifications, 1)
  off()
  off()
  store.set({ count: 2 })
  assert.equal(notifications, 1)
  store.dispose()
  store.dispose()
  store.set({ count: 3 })
  assert.equal(store.getSnapshot().count, 2)
})

test('public SDK: model stores stop publishing when their plugin is disabled', async (t) => {
  const { state, loader } = await loadFixture(t)
  assert.equal(typeof state.context.model.createStore, 'function')
  const store = state.context.model.createStore({ count: 0 })
  let notifications = 0
  store.subscribe(() => { notifications += 1 })
  await loader.setEnabled('sdk.probe', false)
  store.set({ count: 1 })
  store.update(() => ({ count: 2 }))
  assert.equal(store.getSnapshot().count, 0)
  assert.equal(notifications, 0)
})
