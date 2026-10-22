import assert from 'node:assert/strict'
import test from 'node:test'
import { installWebView2CiPolicy } from './webview2-ci-policy.mjs'

function registry(initial = {}) {
  const values = new Map(Object.entries(initial))
  return {
    values,
    get(key) { return values.get(key) ?? null },
    set(key, value) { values.set(key, value) },
    remove(key) { values.delete(key) },
  }
}
const options = { platform: 'win32', githubActions: 'true', enabled: '1', port: 50123, profile: 'C:\\fixture space\\profile' }

test('WebView2 machine policy requires Windows and explicit GitHub CI opt-in', () => {
  for (const override of [{ platform: 'linux' }, { githubActions: undefined }, { enabled: undefined }]) {
    const backend = registry()
    const restore = installWebView2CiPolicy({ ...options, ...override, registry: backend })
    assert.equal(backend.values.size, 0)
    restore()
    assert.equal(backend.values.size, 0)
  }
})

test('WebView2 CI policies affect only the test app and restore previous values and types', () => {
  const existing = { kind: 'ExpandString', value: '%TEMP%\\existing' }
  const backend = registry({ UserDataFolder: existing })
  const restore = installWebView2CiPolicy({ ...options, registry: backend })
  assert.deepEqual(backend.values.get('AdditionalBrowserArguments'), { kind: 'String', value: '--remote-debugging-port=50123' })
  assert.deepEqual(backend.values.get('UserDataFolder'), { kind: 'String', value: options.profile })
  restore()
  assert.deepEqual([...backend.values], [['UserDataFolder', existing]])
})

test('WebView2 policy installation failure restores already changed values', () => {
  const existing = { kind: 'String', value: 'old arguments' }
  const backend = registry({ AdditionalBrowserArguments: existing })
  const originalSet = backend.set
  backend.set = (key, value) => {
    if (key === 'UserDataFolder') throw new Error('registry write denied')
    originalSet(key, value)
  }
  assert.throws(() => installWebView2CiPolicy({ ...options, registry: backend }), /registry write denied/)
  assert.deepEqual([...backend.values], [['AdditionalBrowserArguments', existing]])
})

test('WebView2 CI rejects invalid debugging ports before touching policy', () => {
  for (const port of [0, -1, 65536, '123; command']) {
    const backend = registry()
    assert.throws(() => installWebView2CiPolicy({ ...options, port, registry: backend }), /invalid.*port/)
    assert.equal(backend.values.size, 0)
  }
})

test('WebView2 policy restoration failure stays visible to CI', () => {
  const backend = registry()
  const restore = installWebView2CiPolicy({ ...options, registry: backend })
  backend.remove = () => { throw new Error('registry cleanup denied') }
  assert.throws(restore, (error) => error instanceof AggregateError && error.errors.length === 2)
})
