import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  configDefaults,
  createConfigApi,
  mergeConfig,
  validateConfig,
} from '../../src/plugin-host/config.js'

const schema = {
  type: 'object',
  required: ['enabled'],
  properties: {
    enabled: { type: 'boolean', default: true },
    mode: { type: 'string', enum: ['fast', 'safe'], default: 'safe' },
    retries: { type: 'integer', default: 2 },
  },
}

function remote() {
  const values = new Map()
  return {
    values,
    async get(id) {
      return values.get(id)
    },
    async set(id, value) {
      values.set(id, value)
    },
  }
}

test('defaults and schema validation', () => {
  assert.deepEqual(configDefaults(schema), { enabled: true, mode: 'safe', retries: 2 })
  assert.equal(validateConfig(schema, { enabled: true }).length, 0)
  const errors = validateConfig(schema, { enabled: 'yes', mode: 'other' })
  assert.equal(errors.length, 2)
  assert.match(errors[0].path, /enabled/)
})

test('patch merges, validates and publishes quickly', async () => {
  const store = remote()
  const api = createConfigApi({ pluginId: 'demo.plugin', schema, remote: store })
  let notifications = 0
  const off = api.subscribe(() => notifications++)
  assert.deepEqual(api.get(), { enabled: true, mode: 'safe', retries: 2 })

  await api.set({ mode: 'fast' })
  assert.equal(api.get().mode, 'fast')
  assert.equal(notifications, 1)
  assert.deepEqual(store.values.get('demo.plugin'), api.get())

  await assert.rejects(() => api.set({ retries: 1.5 }), (error) => error.code === 'config/invalid')
  assert.equal(api.get().retries, 2)
  off()
  await api.set({ retries: 4 })
  assert.equal(notifications, 1)
})

test('namespace isolation and persisted load', async () => {
  const store = remote()
  const first = createConfigApi({ pluginId: 'one', schema, remote: store })
  const second = createConfigApi({ pluginId: 'two', schema, remote: store })
  await first.set({ mode: 'fast' })
  assert.equal(first.get().mode, 'fast')
  assert.equal(second.get().mode, 'safe')

  const reloaded = createConfigApi({ pluginId: 'one', schema, remote: store })
  await reloaded.load()
  assert.equal(reloaded.get().mode, 'fast')
})

test('config size cap rejects oversized patches', async () => {
  const api = createConfigApi({
    pluginId: 'big',
    schema: { type: 'object', properties: { blob: { type: 'string' } } },
  })
  await assert.rejects(
    () => api.set({ blob: 'x'.repeat(65 * 1024) }),
    (error) => error.code === 'config/too-large',
  )
})

test('nested merge keeps unspecified fields', () => {
  const nested = {
    type: 'object',
    properties: { net: { type: 'object', properties: { timeout: { type: 'number', default: 5 } } } },
  }
  assert.deepEqual(mergeConfig(nested, { net: { timeout: 5 } }, { net: { timeout: 9 } }), {
    net: { timeout: 9 },
  })
})

function deferred() {
  let resolve
  let reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}

test('concurrent patches merge when written and subscribers receive the committed snapshot', async () => {
  const firstWrite = deferred()
  const started = deferred()
  const writes = []
  const api = createConfigApi({ pluginId: 'queued', schema, remote: {
    async set(id, value) {
      writes.push(value)
      if (writes.length === 1) { started.resolve(); await firstWrite.promise }
    },
  } })
  const snapshots = []
  api.subscribe((value) => snapshots.push(value))
  const first = api.set({ enabled: false })
  await started.promise
  const second = api.set({ retries: 4 })
  firstWrite.resolve()
  await Promise.all([first, second])
  assert.deepEqual(api.get(), { enabled: false, mode: 'safe', retries: 4 })
  assert.equal(writes.length, 2)
  assert.equal(writes[1].enabled, false)
  assert.deepEqual(snapshots, writes)
  assert.equal(snapshots[1], api.getSnapshot())
})

test('failed queued writes leave the prior snapshot intact and do not block the next patch', async () => {
  const firstWrite = deferred()
  const started = deferred()
  let calls = 0
  const api = createConfigApi({ pluginId: 'retry', schema, remote: {
    async set() {
      if (++calls === 1) { started.resolve(); await firstWrite.promise }
    },
  } })
  const first = api.set({ enabled: false })
  const failed = assert.rejects(first, /disk full/)
  await started.promise
  const second = api.set({ retries: 4 })
  firstWrite.reject(new Error('disk full'))
  await failed
  await second
  assert.deepEqual(api.get(), { enabled: true, mode: 'safe', retries: 4 })
})

test('a pending load finishes before a following patch is merged', async () => {
  const read = deferred()
  const api = createConfigApi({ pluginId: 'load-first', schema, remote: {
    get: () => read.promise,
    async set() {},
  } })
  const loading = api.load()
  const writing = api.set({ retries: 4 })
  read.resolve({ enabled: false, mode: 'fast' })
  await Promise.all([loading, writing])
  assert.deepEqual(api.get(), { enabled: false, mode: 'fast', retries: 4 })
})

test('a following load observes a preceding successful write', async () => {
  const write = deferred()
  let stored = { enabled: true }
  const api = createConfigApi({ pluginId: 'set-first', schema, remote: {
    async get() { return stored },
    async set(id, value) { await write.promise; stored = value },
  } })
  const writing = api.set({ enabled: false })
  const loading = api.load()
  write.resolve()
  await Promise.all([writing, loading])
  assert.equal(api.get().enabled, false)
  assert.equal(api.loaded, true)
})

test('a failed load does not block a following write', async () => {
  const api = createConfigApi({ pluginId: 'load-failure', schema, remote: {
    async get() { throw new Error('read failed') },
    async set() {},
  } })
  const loading = api.load()
  const failed = assert.rejects(loading, /read failed/)
  const writing = api.set({ retries: 4 })
  await failed
  await writing
  assert.equal(api.get().retries, 4)
  assert.equal(api.loaded, false)
})

test('the config limit counts UTF-8 bytes and accepts its exact boundary', async () => {
  let writes = 0
  const api = createConfigApi({ pluginId: 'utf8', remote: { async set() { writes++ } } })
  const overhead = new TextEncoder().encode(JSON.stringify({ blob: '' })).length
  const ascii = 'x'.repeat(64 * 1024 - overhead)
  await api.set({ blob: ascii })
  assert.equal(writes, 1)
  await assert.rejects(() => api.set({ blob: `${ascii}x` }), (error) => error.code === 'config/too-large')
  await assert.rejects(() => api.set({ blob: '元'.repeat(24 * 1024) }), (error) => error.code === 'config/too-large')
  await assert.rejects(() => api.set({ blob: '😀'.repeat(18 * 1024) }), (error) => error.code === 'config/too-large')
  assert.equal(writes, 1)
  assert.equal(api.get().blob, ascii)
})
