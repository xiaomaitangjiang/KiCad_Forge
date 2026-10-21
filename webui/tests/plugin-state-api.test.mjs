import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createPluginStateApi } from '../src/plugin-host/plugin-state-api.js'

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

test('state client encodes ids and injects revision', async () => {
  const { fetchImpl, calls } = fakeFetch(() => ({
    status: 200,
    body: { ok: true, plugin_id: 'plugin/one', enabled: false, revision: 3 },
  }))
  const state = createPluginStateApi(fetchImpl)
  const reply = await state.set('plugin/one', false, 2)
  assert.equal(calls[0].path, '/api/plugins/plugin%2Fone/state')
  assert.deepEqual(JSON.parse(calls[0].options.body), { enabled: false, if_revision: 2 })
  assert.equal(reply.revision, 3)
})

test('state read and backend errors are surfaced', async () => {
  const { fetchImpl, calls } = fakeFetch((path) =>
    path.includes('missing')
      ? { status: 404, body: { ok: false, error: 'plugin_unknown' } }
      : { status: 200, body: { ok: true, enabled: true, revision: 0 } },
  )
  const state = createPluginStateApi(fetchImpl)
  assert.equal((await state.get('ok')).enabled, true)
  await assert.rejects(
    () => state.get('missing'),
    (error) => error.status === 404 && error.body?.error === 'plugin_unknown',
  )
  assert.equal(calls[0].path, '/api/plugins/ok/state')
})
