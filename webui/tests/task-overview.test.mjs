import assert from 'node:assert/strict'
import test from 'node:test'
import { createTaskOverview, estimateTaskBytes, MAX_TASK_OVERVIEW, MAX_TASK_OVERVIEW_BYTES, MAX_TASK_TEXT_CHARS } from '../src/plugin-host/task-overview.js'

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
  assert.ok(MAX_TASK_OVERVIEW_BYTES > 0)
})

function terminalHandle(id, extra = {}) {
  const snapshot = { id, action: 'status', state: 'succeeded', cancelable: false, ...extra }
  return {
    id,
    done: Promise.resolve(snapshot),
    getSnapshot: () => snapshot,
    subscribe: () => () => {},
  }
}

function flush() { return new Promise(resolve => setImmediate(resolve)) }

test('terminal records are evicted from the map, not only sliced from the display', async () => {
  const overview = createTaskOverview({ max: 20 })
  for (let index = 1; index <= 100; index++) overview.track('plugin', terminalHandle(`t${index}`))
  await flush()
  const retained = Array.from({ length: 100 }, (_, index) => `t${index + 1}`).filter(overview.hasTask)
  assert.equal(overview.getSnapshot().tasks.length, 20)
  assert.equal(retained.length, 20, 'the entries map must really evict old terminal records')
  assert.equal(overview.hasTask('t100'), true)
  assert.equal(overview.hasTask('t1'), false)
  assert.equal(retained.includes('t1'), false)
})

test('a long terminal history stays bounded at 1000 records', async () => {
  const overview = createTaskOverview({ max: 20 })
  for (let index = 1; index <= 1000; index++) overview.track('plugin', terminalHandle(`long-${index}`))
  await flush()
  const retained = Array.from({ length: 1000 }, (_, index) => `long-${index + 1}`).filter(overview.hasTask)
  assert.equal(retained.length, 20)
  assert.equal(overview.getSnapshot().tasks.length, 20)
  assert.equal(overview.hasTask('long-1000'), true)
  assert.equal(overview.hasTask('long-1'), false)
})

test('large results are not cached: only a bounded summary remains', async () => {
  const overview = createTaskOverview()
  const hugeOutput = 'x'.repeat(200000)
  const hugePatch = 'y'.repeat(200000)
  overview.track('plugin', terminalHandle('big', {
    result: { output: hugeOutput, patch: hugePatch, files: [{ path: 'a', patch: hugePatch }] },
  }))
  await flush()
  const task = overview.getSnapshot().tasks[0]
  assert.equal('result' in task, false, 'the overview must not retain task.result')
  assert.equal(task.summary.length, MAX_TASK_TEXT_CHARS)
  assert.equal(task.summary, 'x'.repeat(MAX_TASK_TEXT_CHARS - 1) + '…')
  assert.ok(estimateTaskBytes(task) < 2000)
  assert.equal(JSON.stringify(overview.getSnapshot()).includes('y'.repeat(100)), false)
})

test('an older active task finishing late stays retained; active work is never evicted or cancelled', async () => {
  const overview = createTaskOverview({ max: 3 })
  let cancels = 0
  const old = fakeHandle('old', { id: 'old', action: 'history', state: 'running', cancelable: true })
  old.cancel = () => { cancels += 1 }
  const live = fakeHandle('live', { id: 'live', action: 'status', state: 'running', cancelable: true })
  overview.track('plugin', old)
  overview.track('plugin', live)
  for (let index = 0; index < 5; index++) overview.track('plugin', terminalHandle(`t${index}`))
  await flush()
  assert.equal(overview.hasTask('old'), true, 'a running task must not be evicted with terminal records')
  assert.equal(overview.hasTask('live'), true)
  assert.equal(overview.getSnapshot().tasks.filter(task => task.state === 'succeeded').length, 3)

  old.set({ id: 'old', action: 'history', state: 'succeeded', result: { output: 'late' } })
  await flush()
  assert.equal(overview.hasTask('old'), true, 'a late terminal update is newer than the earlier terminal records')
  assert.equal(overview.hasTask('live'), true)
  assert.equal(overview.hasTask('t2'), false, 'the oldest terminal record is evicted instead of the active task')
  const terminalIds = overview.getSnapshot().tasks.filter(task => task.state === 'succeeded').map(task => task.id)
  assert.deepEqual(terminalIds.slice().sort(), ['old', 't3', 't4'].sort())
  assert.equal(cancels, 0, 'eviction must not cancel business work')
})

test('the retained payload obeys the byte budget, evicting oldest terminal records first', async () => {
  const overview = createTaskOverview({ max: 20, maxBytes: 1200 })
  for (let index = 0; index < 10; index++) {
    overview.track('plugin', terminalHandle(`payload-${index}`, { result: { output: 'z'.repeat(400) } }))
  }
  await flush()
  const tasks = overview.getSnapshot().tasks
  const bytes = tasks.reduce((total, task) => total + estimateTaskBytes(task), 0)
  assert.ok(tasks.length < 10)
  assert.ok(tasks.length >= 1)
  assert.ok(bytes <= 1200, `retained payload used ${bytes} bytes`)
  assert.equal(overview.hasTask('payload-9'), true)
  assert.equal(overview.hasTask('payload-0'), false)
})

test('the retained byte budget is measured in UTF-8 bytes, not UTF-16 code units', async () => {
  const make = (id, error) => ({
    id, pluginId: '', action: '', state: '', phase: '', cancelable: false,
    uncertain: false, error, summary: '', updatedAt: undefined,
  })
  const tasks = [
    make('ascii', 'a'.repeat(100)),
    make('chinese', '中'.repeat(100)),
    make('emoji', '🔥'.repeat(100)),
    make('mixed', '中a🔥'.repeat(50)),
  ]
  for (const task of tasks) {
    assert.equal(estimateTaskBytes(task), Buffer.byteLength(JSON.stringify(task), 'utf8'))
  }
  assert.ok(estimateTaskBytes(tasks[1]) > JSON.stringify(tasks[1]).length,
    'Chinese text must cost more bytes than UTF-16 code units')
  assert.ok(estimateTaskBytes(tasks[2]) > JSON.stringify(tasks[2]).length,
    'emoji must cost more bytes than UTF-16 code units')
})

test('a 4096-byte budget keeps Chinese terminal records under the real UTF-8 limit', async () => {
  const maxBytes = 4096
  const overview = createTaskOverview({ max: 20, maxBytes })
  for (let index = 0; index < 20; index++) {
    const snapshot = {
      id: `done-${index}`, action: 'history', state: 'failed',
      error: '错误'.repeat(200), result: { output: '摘要'.repeat(200) },
    }
    overview.track('review', terminalHandle(snapshot.id, {
      state: snapshot.state, error: snapshot.error, result: snapshot.result,
    }))
  }
  await flush()
  const retained = overview.getSnapshot().tasks
  const utf8Bytes = retained.reduce((sum, value) => sum + Buffer.byteLength(JSON.stringify(value), 'utf8'), 0)
  assert.ok(retained.length > 0)
  assert.ok(utf8Bytes <= maxBytes, `retained ${utf8Bytes} UTF-8 bytes exceeds ${maxBytes}`)
  assert.ok(estimateTaskBytes(retained[0]) > JSON.stringify(retained[0]).length)
})

test('mixed-script terminal payloads obey the budget and evict the oldest records first', async () => {
  const maxBytes = 900
  const overview = createTaskOverview({ max: 20, maxBytes })
  const errors = ['a'.repeat(300), '中'.repeat(120), '🔥'.repeat(80)]
  for (let index = 0; index < 12; index++) {
    overview.track('plugin', terminalHandle(`mix-${index}`, {
      state: 'failed', error: errors[index % errors.length], result: { output: '输出'.repeat(30) },
    }))
  }
  await flush()
  const tasks = overview.getSnapshot().tasks
  const bytes = tasks.reduce((sum, task) => sum + estimateTaskBytes(task), 0)
  assert.ok(tasks.length >= 1)
  assert.ok(bytes <= maxBytes, `retained payload used ${bytes} bytes`)
  assert.equal(overview.hasTask('mix-11'), true)
  assert.equal(overview.hasTask('mix-0'), false)
})

test('active records are protected from the terminal byte budget', async () => {
  const overview = createTaskOverview({ max: 20, maxBytes: 500 })
  const active = fakeHandle('active-big', {
    id: 'active-big', action: 'import', state: 'running', cancelable: true,
    error: '进行中'.repeat(80), result: { output: '摘要'.repeat(80) },
  })
  overview.track('plugin', active)
  for (let index = 0; index < 5; index++) {
    overview.track('plugin', terminalHandle(`small-${index}`, { state: 'succeeded' }))
  }
  await flush()
  const tasks = overview.getSnapshot().tasks
  assert.equal(overview.hasTask('active-big'), true, 'active work must survive even when it exceeds the budget alone')
  assert.ok(tasks.some(task => task.id === 'active-big'))
  assert.ok(tasks.filter(task => task.state === 'succeeded').length <= 1,
    'the terminal allowance must shrink by the active bytes')
})

test('snapshots are frozen and clearFinished/dismiss preserve active records', async () => {
  const overview = createTaskOverview()
  const active = fakeHandle('active', { id: 'active', action: 'import', state: 'running', cancelable: true })
  overview.track('plugin', active)
  overview.track('plugin', terminalHandle('done'))
  await flush()
  const snapshot = overview.getSnapshot()
  assert.equal(Object.isFrozen(snapshot), true)
  assert.equal(Object.isFrozen(snapshot.tasks), true)
  assert.equal(Object.isFrozen(snapshot.tasks[0]), true)
  assert.throws(() => { snapshot.tasks.push({}) }, TypeError)

  overview.clearFinished()
  assert.deepEqual(overview.getSnapshot().tasks.map(task => task.id), ['active'])
  overview.dismiss('active')
  assert.equal(overview.getSnapshot().tasks.length, 0)
})
