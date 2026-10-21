import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createPluginLoader } from '../src/plugin-host/loader.js'
import { createSlotHost } from '../src/plugin-host/slot-host.js'

let sequence = 0
function deferred() {
  let resolve
  let reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

async function until(predicate) {
  for (let index = 0; index < 30; index++) {
    if (predicate()) return
    await new Promise((resolve) => setImmediate(resolve))
  }
  assert.fail('Fixture did not reach the expected asynchronous boundary')
}

function fixture(t, { delayImport = false, configGates = [], stateApi = null, enabled = true, apply = '', externalDependency = false } = {}) {
  const key = `__kfLifecycle${++sequence}`
  const state = { applied: 0, disposed: 0, contexts: [], configs: 0, requests: [], importGate: deferred(), applyGate: deferred() }
  globalThis[key] = state
  const previousLocation = Object.getOwnPropertyDescriptor(globalThis, 'location')
  const previousDocument = Object.getOwnPropertyDescriptor(globalThis, 'document')
  Object.defineProperty(globalThis, 'location', { configurable: true, value: { origin: 'http://lifecycle.invalid' } })
  const styles = new Set()
  Object.defineProperty(globalThis, 'document', { configurable: true, value: {
    createElement() { return { dataset: {}, remove() { styles.delete(this) } } },
    head: { append(node) { styles.add(node) } },
  } })
  const source = `const state = globalThis[${JSON.stringify(key)}];
    ${delayImport ? 'await state.importGate.promise;' : ''}
    export default { name: '${key}', apply(ctx) {
      state.applied++; state.contexts.push(ctx);
      ctx.on('dispose', () => state.disposed++);
      ${apply}
    } }; // `
  const manifest = { id: key, name: key, version: '1.0.0', enabled, engines: { sdk: '^1.0.0' },
    icon_url: `/api/plugins/${key}/icon/icon.svg`,
    frontend: { entry_url: `data:text/javascript,${encodeURIComponent(source)}`, styles: ['/fixture.css'],
      requires: externalDependency ? [{ contract: 'community.lifecycle', range: '^1.0.0' }] : [] } }
  const host = createSlotHost()
  const off = host.registerShell()
  const loader = createPluginLoader({ host, stateApi, fetchImpl: async (url, options = {}) => {
    state.requests.push({ url, options })
    let body
    if (url === '/api/plugin-diagnostics') body = []
    else if (url === '/api/plugins') body = [manifest]
    else if (url.startsWith('/api/plugin-config/')) {
      const index = state.configs++
      if (configGates[index]) await configGates[index].promise
      body = { config: {} }
    } else throw new Error(`Unexpected lifecycle request: ${url}`)
    return { ok: true, status: 200, json: async () => body }
  } })
  const withdraw = externalDependency
    ? loader.contracts.provide({ id: 'fixture.host' }, 'community.lifecycle', '1.0.0', {})
    : () => {}
  const loading = loader.loadAll()
  t.after(async () => {
    state.importGate.resolve()
    state.applyGate.resolve()
    for (const gate of configGates) gate.resolve()
    await loading.catch(() => {})
    await loader.dispose()
    off()
    delete globalThis[key]
    for (const [name, descriptor] of [['location', previousLocation], ['document', previousDocument]]) {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor)
      else delete globalThis[name]
    }
  })
  return { id: key, loader, host, state, styles, loading, withdraw,
    record: () => loader.getSnapshot().find((entry) => entry.id === key) }
}

for (const action of ['disable', 'dispose']) {
  test(`${action} while import is pending prevents config reads, apply and UI creation`, async (t) => {
    const f = fixture(t, { delayImport: true })
    await until(() => f.record()?.status === 'loading')
    if (action === 'disable') await f.loader.setEnabled(f.id, false)
    else await f.loader.dispose()
    f.state.importGate.resolve()
    await f.loading
    assert.equal(f.state.configs, 0)
    assert.equal(f.state.applied, 0)
    assert.equal(f.styles.size, 0)
    assert.equal(f.record()?.status, action === 'disable' ? 'disabled' : undefined)
  })

  test(`${action} while config is pending prevents late apply and task-client creation`, async (t) => {
    const config = deferred()
    const f = fixture(t, { configGates: [config] })
    await until(() => f.state.configs === 1)
    if (action === 'disable') await f.loader.setEnabled(f.id, false)
    else await f.loader.dispose()
    config.resolve()
    await f.loading
    assert.equal(f.state.applied, 0)
    assert.equal(f.state.contexts.length, 0)
    assert.equal(f.styles.size, 0)
    assert.equal(f.record()?.status, action === 'disable' ? 'disabled' : undefined)
  })
}

test('the manifest icon URL is registered before plugin activation', async (t) => {
  const f = fixture(t)
  await f.loading
  const panel = { name: 'demo.prototype.aux', kind: 'list' }
  const dispose = f.host.register(panel, { id: 'panel', title: 'Panel', render: () => null },
    { id: f.id, version: '1.0.0' })
  const [entry] = f.host.entriesWithIcons(panel)
  assert.equal(entry.icon.props.src, `/api/plugins/${f.id}/icon/icon.svg`)
  dispose()
})

test('rapid disable then enable activates only the latest generation after config resolves', async (t) => {
  const first = deferred()
  const second = deferred()
  const f = fixture(t, { configGates: [first, second] })
  await until(() => f.state.configs === 1)
  await f.loader.setEnabled(f.id, false)
  const enabling = f.loader.setEnabled(f.id, true)
  first.resolve()
  await until(() => f.state.configs === 2)
  assert.equal(f.state.applied, 0)
  second.resolve()
  await Promise.all([f.loading, enabling])
  assert.equal(f.record().status, 'active')
  assert.equal(f.state.applied, 1)
  assert.equal(f.styles.size, 1)
})

test('an older enable completion cannot activate after a newer disable request', async (t) => {
  const requests = []
  const f = fixture(t, { enabled: false, stateApi: { set(id, enabled) {
    const response = deferred()
    requests.push({ enabled, response })
    return response.promise
  } } })
  await f.loading
  const enabling = f.loader.setEnabled(f.id, true)
  const disabling = f.loader.setEnabled(f.id, false)
  await until(() => requests.length === 1)
  requests[0].response.resolve({ enabled: true, revision: 1 })
  await enabling
  assert.equal(f.state.applied, 0)
  await until(() => requests.length === 2)
  requests[1].response.resolve({ enabled: false, revision: 2 })
  await disabling
  assert.equal(f.record().enabled, false)
  assert.equal(f.record().status, 'disabled')
  assert.equal(f.state.applied, 0)
})

test('a rejected stale state request does not replace newer state or add a diagnostic', async (t) => {
  const requests = []
  const f = fixture(t, { enabled: false, stateApi: { set() {
    const response = deferred(); requests.push(response); return response.promise
  } } })
  await f.loading
  const enabling = f.loader.setEnabled(f.id, true)
  const disabling = f.loader.setEnabled(f.id, false)
  await until(() => requests.length === 1)
  requests[0].reject(Object.assign(new Error('stale conflict'), { status: 409 }))
  await enabling
  await until(() => requests.length === 2)
  requests[1].resolve({ enabled: false, revision: 2 })
  await disabling
  assert.equal(f.record().status, 'disabled')
  assert.equal(f.host.diagnostics.some((item) => item.code === 'plugin/state-failed'), false)
})

test('a rejected current toggle preserves the active plugin and allows a later retry', async (t) => {
  let failures = 1
  const f = fixture(t, { stateApi: { async set(id, enabled) {
    if (failures-- > 0) throw Object.assign(new Error('unavailable'), { status: 503 })
    return { enabled, revision: 1 }
  } } })
  await f.loading
  await assert.rejects(() => f.loader.setEnabled(f.id, false), /unavailable/)
  assert.equal(f.record().enabled, true)
  assert.equal(f.record().status, 'active')
  await f.loader.setEnabled(f.id, false)
  assert.equal(f.record().status, 'disabled')
  assert.equal(f.state.disposed, 1)
})

test('a failed activation can be retried without losing the new generation', async (t) => {
  const f = fixture(t, { apply: "if (state.applied === 1) throw new Error('first apply failed');" })
  await f.loading
  assert.equal(f.record().status, 'error')
  await f.loader.setEnabled(f.id, true)
  assert.equal(f.record().status, 'active')
  assert.equal(f.state.applied, 2)
})

test('a late apply rejection cannot clear a newer active generation', async (t) => {
  const f = fixture(t, { apply: 'if (state.applied === 1) return state.applyGate.promise;' })
  await until(() => f.state.applied === 1)
  const disabling = f.loader.setEnabled(f.id, false)
  const enabling = f.loader.setEnabled(f.id, true)
  f.state.applyGate.reject(new Error('old apply failed'))
  await Promise.all([f.loading, disabling, enabling])
  assert.equal(f.state.applied, 2)
  assert.equal(f.record().status, 'active')
  assert.equal(f.record().error, null)
  assert.equal(f.styles.size, 1)
  assert.equal(f.state.disposed, 1)
})

test('many rapid enable toggles during import create only one live generation', async (t) => {
  const f = fixture(t, { delayImport: true })
  await until(() => f.record()?.status === 'loading')
  const toggles = []
  for (let index = 0; index < 10; index++) {
    toggles.push(f.loader.setEnabled(f.id, false), f.loader.setEnabled(f.id, true))
  }
  f.state.importGate.resolve()
  await Promise.all([f.loading, ...toggles])
  assert.equal(f.state.applied, 1)
  assert.equal(f.record().status, 'active')
  assert.equal(f.styles.size, 1)
})

test('the loader config adapter writes the committed object instead of the plugin id', async (t) => {
  const f = fixture(t)
  await f.loading
  await f.state.contexts[0].config.set({ enabled: false, retries: 4 })
  const write = f.state.requests.find(({ options }) => options.method === 'POST')
  assert.equal(write.url, `/api/plugin-config/${encodeURIComponent(f.id)}`)
  assert.deepEqual(JSON.parse(write.options.body), { enabled: false, retries: 4 })
})

test('dependency cleanup finishing after a disable does not replace the disabled state', async (t) => {
  const f = fixture(t, { externalDependency: true,
    apply: 'ctx.effect(() => () => { state.cleanupStarted = true; return state.applyGate.promise; });' })
  await f.loading
  f.withdraw()
  await until(() => f.state.cleanupStarted)
  await f.loader.setEnabled(f.id, false)
  assert.equal(f.record().status, 'disabled')
  f.state.applyGate.resolve()
  for (let index = 0; index < 3; index++) await new Promise((resolve) => setImmediate(resolve))
  assert.equal(f.record().status, 'disabled')
})

test('rapid persisted toggles keep backend revisions ordered and preserve the latest intent', async (t) => {
  const firstReply = deferred()
  let calls = 0
  const persisted = { enabled: false, revision: 0 }
  const revisions = []
  const f = fixture(t, { enabled: false, stateApi: { async set(id, enabled, revision) {
    revisions.push(revision)
    if (revision !== persisted.revision) throw Object.assign(new Error('revision conflict'), { status: 409 })
    persisted.enabled = enabled
    persisted.revision++
    const reply = { ...persisted }
    if (++calls === 1) await firstReply.promise
    return reply
  } } })
  await f.loading
  const enabling = f.loader.setEnabled(f.id, true)
  const disabling = f.loader.setEnabled(f.id, false)
  const done = Promise.all([enabling, disabling])
  firstReply.resolve()
  await done
  assert.deepEqual(revisions, [0, 1])
  assert.deepEqual(persisted, { enabled: false, revision: 2 })
  assert.equal(f.record().status, 'disabled')
  assert.equal(f.state.applied, 0)
})

function pythonFixture(t, stateApi) {
  const manifest = { id: 'python-only', name: 'Python only', enabled: true, revision: 4,
    actions: [{ id: 'run', name: 'Run' }] }
  const host = createSlotHost()
  const loader = createPluginLoader({ host, stateApi, fetchImpl: async (url) => {
    assert.ok(url === '/api/plugin-diagnostics' || url === '/api/plugins')
    return { ok: true, json: async () => url === '/api/plugins' ? [manifest] : [] }
  } })
  t.after(() => loader.dispose())
  return { loader, host }
}

test('Python-only setEnabled persists through the same CAS queue without importing a frontend', async (t) => {
  const gate = deferred()
  const calls = []
  const { loader } = pythonFixture(t, { async set(id, enabled, revision) {
    calls.push({ id, enabled, revision })
    if (calls.length === 1) await gate.promise
    return { enabled, revision: revision + 1 }
  } })
  await loader.loadAll()
  const disabling = loader.setEnabled('python-only', false)
  const enabling = loader.setEnabled('python-only', true)
  await until(() => calls.length === 1)
  gate.resolve()
  const replies = await Promise.all([disabling, enabling])
  assert.deepEqual(calls, [
    { id: 'python-only', enabled: false, revision: 4 },
    { id: 'python-only', enabled: true, revision: 5 },
  ])
  assert.deepEqual(replies, [{ enabled: false, revision: 5 }, { enabled: true, revision: 6 }])
  assert.deepEqual(loader.getSnapshot(), [])
})

test('Python-only state failures reject and preserve the revision for a later retry', async (t) => {
  const calls = []
  const { loader, host } = pythonFixture(t, { async set(id, enabled, revision) {
    calls.push(revision)
    if (calls.length === 1) throw Object.assign(new Error('write failed'), { status: 503 })
    return { enabled, revision: revision + 1 }
  } })
  await loader.loadAll()
  await assert.rejects(loader.setEnabled('python-only', false), /write failed/)
  await loader.setEnabled('python-only', false)
  assert.deepEqual(calls, [4, 4])
  assert.equal(host.diagnostics.filter((d) => d.code === 'plugin/state-failed').length, 1)
  assert.equal(host.diagnostics.some((d) => /frontend entry/.test(d.message)), false)
})

test('network failure cannot publish a successful local enable state', async (t) => {
  const f = fixture(t, { stateApi: { async set() { throw new TypeError('Failed to fetch') } } })
  await f.loading
  await assert.rejects(f.loader.setEnabled(f.id, false), /Failed to fetch/)
  assert.equal(f.record().enabled, true)
  assert.equal(f.record().status, 'active')
})
