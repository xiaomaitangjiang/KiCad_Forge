import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createTasksApi, createImportsApi } from '../../src/plugin-host/tasks-api.js'

function fakeRuntime(result) {
  const calls = []
  let sequence = 0
  const fetchImpl = async (path, options = {}) => {
    calls.push({ path, options })
    let body
    if (path === '/api/tasks') {
      sequence += 1
      body = { ok: true, task_id: `task-${sequence}`, state: 'queued' }
    } else if (path.startsWith('/api/tasks/')) {
      body = { ok: true, task_id: path.split('/').pop(), state: 'succeeded', result }
    } else if (path.startsWith('/api/imports/operations/')) {
      body = { ok: true, operation_id: path.split('/').pop(), result }
    } else {
      throw new Error(`unexpected ${path}`)
    }
    return { ok: true, status: 200, json: async () => body }
  }
  return { fetchImpl, calls }
}

test('imports maps SDK selection/provider to the host/Python shape', async () => {
  const { fetchImpl, calls } = fakeRuntime({ ok: true, lcsc_id: 'C1', symbol_names: ['S'] })
  const tasks = createTasksApi('consumer', fetchImpl)
  const imports = createImportsApi(tasks, fetchImpl, 'consumer')
  const analysis = await imports.analyze({ provider: 'lcsc', externalId: 'C1' })
  assert.equal(analysis.externalId, 'C1')
  assert.deepEqual(analysis.symbolNames, ['S'])
  const submit = JSON.parse(calls.find((call) => call.path === '/api/tasks').options.body)
  assert.equal(submit.plugin_id, 'com.kicad_forge.lcsc_import')
  assert.deepEqual(submit.args, { lcsc_id: 'C1' })
})

test('imports.commit maps camelCase choices and normalizes the result handle', async () => {
  const { fetchImpl, calls } = fakeRuntime({
    ok: true, operation_id: 'op-1', symbol_id: 's1', footprint_id: 'f1',
    footprint_reference: 'L:C1', mode: 'reuse', warnings: [],
  })
  const tasks = createTasksApi('consumer', fetchImpl)
  const imports = createImportsApi(tasks, fetchImpl, 'consumer')
  const handle = await imports.commit({
    provider: 'com.kicad_forge.lcsc_import',
    externalId: 'C1',
    targetComponentLibraryId: 'pkg',
    symbolLibraryChoice: { mode: 'existing', path: 'Device.kicad_sym' },
    footprintChoice: { mode: 'reuse', footprintId: 'f1' },
    operationId: 'op-1',
  })
  const submit = JSON.parse(calls.find((call) => call.path === '/api/tasks').options.body)
  assert.equal(submit.plugin_id, 'com.kicad_forge.lcsc_import')
  assert.deepEqual(submit.args, {
    lcsc_id: 'C1',
    target_component_library_id: 'pkg',
    operation_id: 'op-1',
    symbol_library_choice: { mode: 'existing', path: 'Device.kicad_sym' },
    footprint_choice: { mode: 'reuse', footprint_id: 'f1' },
  })
  const snapshot = await handle.done
  assert.equal(snapshot.result.operationId, 'op-1')
  assert.equal(snapshot.result.symbolId, 's1')
  assert.equal(snapshot.result.footprintReference, 'L:C1')
})

test('tasks get/list expose normalized snapshots; get 404 is null', async () => {
  const calls = []
  const fetchImpl = async (path) => {
    calls.push(path)
    if (path === '/api/tasks/missing') {
      return { ok: false, status: 404, json: async () => ({ ok: false, error: 'task_not_found' }) }
    }
    if (path === '/api/tasks/t1') {
      return { ok: true, status: 200, json: async () => ({ ok: true, task_id: 't1', state: 'running', cancelable: true }) }
    }
    return { ok: true, status: 200, json: async () => ({ ok: true, tasks: [{ task_id: 't2', state: 'succeeded' }] }) }
  }
  const tasks = createTasksApi('p', fetchImpl)
  assert.equal((await tasks.get('t1')).id, 't1')
  assert.equal(await tasks.get('missing'), null)
  assert.deepEqual((await tasks.list()).map((item) => item.id), ['t2'])
})
