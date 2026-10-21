import assert from 'node:assert/strict'
import test from 'node:test'
import { createTaskOverview, MAX_TASK_OVERVIEW } from '../src/plugin-host/task-overview.js'

function fakeHandle(id, snapshot) {
  let value = snapshot
  const listeners = new Set()
  let resolveDone
  const done = new Promise(resolve => { resolveDone = resolve })
  return {
    id,
    done,
    getSnapshot: () => value,
    subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener) },
    set(next) {
      value = next
      for (const listener of [...listeners]) listener()
      if (['succeeded', 'failed', 'cancelled'].includes(next.state)) resolveDone(next)
    },
  }
}

test('task overview tracks a handle without owning its lifecycle', () => {
  const overview = createTaskOverview({ now: () => 7 })
  const handle = fakeHandle('t1', { id: 't1', action: 'import', state: 'running', phase: 'fetch', cancelable: true })
  const detach = overview.track('com.example.lcsc', handle)
  assert.deepEqual(overview.getSnapshot().tasks.map(task => [task.id, task.pluginId, task.state, task.phase]), [
    ['t1', 'com.example.lcsc', 'running', 'fetch'],
  ])
  assert.equal(overview.getSnapshot().tasks[0].updatedAt, 7)

  handle.set({ id: 't1', action: 'import', state: 'succeeded', cancelable: false })
  assert.equal(overview.getSnapshot().tasks[0].state, 'succeeded')
  assert.equal(overview.getSnapshot().tasks.length, 1, 'terminal task stays as a read-only record')
  detach()
  detach()
})

test('task overview bounds records and clearFinished preserves active work', () => {
  const overview = createTaskOverview({ max: 3, now: () => 1 })
  for (let index = 0; index < 5; index++) {
    const handle = fakeHandle(`t${index}`, { id: `t${index}`, action: 'status', state: 'succeeded', cancelable: false })
    overview.track('plugin', handle)
  }
  assert.equal(overview.getSnapshot().tasks.length, 3)
  assert.deepEqual(overview.getSnapshot().tasks.map(task => task.id), ['t4', 't3', 't2'])
  overview.clearFinished()
  assert.equal(overview.getSnapshot().tasks.length, 0)

  const running = fakeHandle('running', { id: 'running', action: 'import', state: 'running', cancelable: true })
  overview.track('plugin', running)
  overview.clearFinished()
  assert.deepEqual(overview.getSnapshot().tasks.map(task => task.id), ['running'])
  overview.dismiss('running')
  assert.equal(overview.getSnapshot().tasks.length, 0)
})

test('task overview ignores updates after detach and exposes a stable empty snapshot', () => {
  const overview = createTaskOverview()
  const handle = fakeHandle('t', { id: 't', action: 'x', state: 'running', cancelable: true })
  const before = overview.getSnapshot()
  const detach = overview.track('p', handle)
  assert.notEqual(overview.getSnapshot(), before)
  detach()
  const after = overview.getSnapshot()
  handle.set({ id: 't', action: 'x', state: 'succeeded', cancelable: false })
  assert.equal(overview.getSnapshot(), after)
  assert.equal(MAX_TASK_OVERVIEW, 20)
})
