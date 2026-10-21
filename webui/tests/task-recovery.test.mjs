import test from 'node:test'
import assert from 'node:assert/strict'
import { createTaskOverview } from '../src/plugin-host/task-overview.js'
import { recoverTasks, resetTaskRecovery } from '../src/plugin-host/task-recovery.js'

function reply(body, ok = true, status = 200) {
  return { ok, status, async json() { return body } }
}

function waitFor(predicate, timeout = 3000) {
  return new Promise((resolve, reject) => {
    const started = Date.now()
    const tick = () => {
      let value
      try { value = predicate() } catch (error) { reject(error); return }
      if (value) { resolve(value); return }
      if (Date.now() - started > timeout) { reject(new Error('timeout waiting for condition')); return }
      setTimeout(tick, 20)
    }
    tick()
  })
}

test('recovery records terminal tasks and observes live ones without a second list scan', async () => {
  resetTaskRecovery()
  const overview = createTaskOverview()
  const calls = { list: 0, t1: 0, t2: 0 }
  let live = { task_id: 't2', action: 'import', state: 'running', phase: 'write' }
  const fetchImpl = async (url) => {
    if (url === '/api/plugins/demo.plugin/tasks') {
      calls.list += 1
      return reply({ ok: true, tasks: [
        live,
        { task_id: 't1', action: 'status', state: 'succeeded', result: { output: 'first' } },
      ] })
    }
    if (url === '/api/tasks/t1') { calls.t1 += 1; return reply({ ok: true, task_id: 't1', action: 'status', state: 'succeeded' }) }
    if (url === '/api/tasks/t2') {
      calls.t2 += 1
      if (calls.t2 >= 2) live = { ...live, state: 'succeeded', result: { output: 'second' } }
      return reply({ ok: true, ...live })
    }
    return reply({ ok: false, error: 'unexpected ' + url }, false, 404)
  }

  await recoverTasks(['demo.plugin'], { fetchImpl, overview })
  assert.equal(calls.list, 1)
  assert.equal(overview.hasTask('t1'), true)
  assert.equal(overview.hasTask('t2'), true)
  assert.equal(overview.getSnapshot().tasks.find(task => task.id === 't1')?.result?.output, 'first')

  const finished = await waitFor(() => overview.getSnapshot().tasks.find(task => task.id === 't2' && task.state === 'succeeded'))
  assert.equal(finished.pluginId, 'demo.plugin')
  assert.equal(finished.result.output, 'second')
  assert.equal(calls.t1, 0, 'terminal recovered task must not be polled')

  await recoverTasks(['demo.plugin'], { fetchImpl, overview })
  assert.equal(calls.list, 1, 'a second panel open must not rescan a recovered plugin')
  assert.equal(calls.t2, 2, 'the live task should use the normal read adapter exactly until terminal')
  resetTaskRecovery()
})
