// 桌面会话心跳的真实调度行为（不是源码字符串匹配）：
// 立即开跳 / 重复调度 / 清理后不再调用（含在途请求晚完成）/ 卸载不发 bye /
// StrictMode 挂载-清理-重挂后恢复。
import test from 'node:test'
import assert from 'node:assert/strict'

import { startDesktopSession } from '../src/lib/desktopSession.js'

function fakeTimers() {
  let nextId = 1
  const pending = new Map()
  return {
    setTimer(fn, ms) { const id = nextId++; pending.set(id, { fn, ms }); return id },
    clearTimer(id) { pending.delete(id) },
    tick() { const jobs = [...pending.values()]; pending.clear(); jobs.forEach(j => j.fn()) },
    get count() { return pending.size },
    intervals() { return [...pending.values()].map(j => j.ms) },
  }
}

function fakeWindow() {
  const listeners = {}
  return {
    addEventListener(type, handler) { (listeners[type] ||= []).push(handler) },
    removeEventListener(type, handler) {
      listeners[type] = (listeners[type] || []).filter(h => h !== handler)
    },
    dispatch(type) { (listeners[type] || []).forEach(h => h({ type })) },
    listenerCount(type) { return (listeners[type] || []).length },
  }
}

test('心跳立即启动并按间隔重复调度', () => {
  const calls = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: (url) => { calls.push(url); return Promise.resolve() },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    intervalMs: 1000,
    windowLike: null,
    navigatorLike: null,
  })

  assert.deepEqual(calls, ['/api/status'])
  assert.equal(timers.count, 1)
  assert.deepEqual(timers.intervals(), [1000])

  timers.tick()
  assert.deepEqual(calls, ['/api/status', '/api/status'])
  assert.equal(timers.count, 1) // 下一拍已经排好

  timers.tick()
  assert.equal(calls.length, 3)
  session.stop()
})

test('心跳请求失败也不会中断调度', () => {
  const calls = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: (url) => { calls.push(url); return Promise.reject(new Error('boom')) },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
  })

  assert.equal(calls.length, 1)
  assert.equal(timers.count, 1)
  timers.tick()
  assert.equal(calls.length, 2) // reject 之后仍按原间隔继续
  assert.equal(timers.count, 1)
  session.stop()
})

test('cleanup 之后不再有任何调用，也没有遗留 timer', async () => {
  let resolveFetch
  const calls = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: (url) => { calls.push(url); return new Promise(resolve => { resolveFetch = resolve }) },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
  })

  session.stop()
  assert.equal(timers.count, 0)

  // 在途请求晚完成：不能借它的完成重新调度
  resolveFetch?.({ ok: true })
  await Promise.resolve()
  await Promise.resolve()
  timers.tick()
  assert.equal(calls.length, 1)
  assert.equal(timers.count, 0)
  assert.equal(session.active, false)
})

test('beforeunload 不发送 /api/bye 信标（刷新页面不等于关闭本地应用）', () => {
  const beacons = []
  const timers = fakeTimers()
  const win = fakeWindow()
  const session = startDesktopSession({
    fetchFn: () => Promise.resolve(),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: win,
    navigatorLike: { sendBeacon: (url) => { beacons.push(url) } },
  })

  win.dispatch('beforeunload')
  win.dispatch('unload')
  assert.deepEqual(beacons, [])   // 一个信标都不许发
  assert.equal(session.active, true) // 卸载事件不影响心跳调度

  session.stop()
  assert.equal(win.listenerCount('beforeunload'), 0) // 清理不遗留监听
})

test('每一拍先中止上一拍的在途请求：stop 之后没有悬挂请求、没有遗留 timer', () => {
  const signals = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    // 永不 settle：只有 abort 信号能证明请求被取消了
    fetchFn: (url, init) => { signals.push(init.signal); return new Promise(() => {}) },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
  })

  assert.equal(signals.length, 1)
  assert.equal(signals[0].aborted, false)

  timers.tick()                       // 下一拍
  assert.equal(signals.length, 2)
  assert.equal(signals[0].aborted, true)   // 上一拍已被中止，不是被覆盖后失联
  assert.equal(signals[1].aborted, false)

  session.stop()
  assert.equal(signals[1].aborted, true)   // 最后一拍也被中止
  assert.equal(timers.count, 0)            // 没有遗留 timer
})

test('StrictMode 组合：旧会话的在途请求晚 settle 不影响新会话的调度', async () => {
  const calls = []
  const resolvers = []
  const timers = fakeTimers()
  const options = {
    fetchFn: (url) => { calls.push(url); return new Promise(resolve => { resolvers.push(resolve) }) },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
  }

  const first = startDesktopSession(options)   // 请求 1 一直挂着
  first.stop()                                 // StrictMode 的第一次清理
  const second = startDesktopSession(options)  // 请求 2 也挂着
  assert.equal(resolvers.length, 2)
  assert.equal(timers.count, 1)

  // 旧会话的请求晚 settle：不得给旧会话排 timer，也不得动新会话
  resolvers[0]({ ok: true })
  await Promise.resolve()
  await Promise.resolve()
  assert.equal(timers.count, 1)

  const before = calls.length
  timers.tick()                                // 只有新会话的下一拍
  assert.equal(calls.length, before + 1)
  assert.equal(timers.count, 1)

  resolvers[1]({ ok: true })
  await Promise.resolve()
  await Promise.resolve()
  assert.equal(timers.count, 1)

  second.stop()
  assert.equal(timers.count, 0)
  assert.equal(second.active, false)
})

test('StrictMode 挂载-清理-重挂：新会话继续心跳，旧会话不再影响', () => {
  const calls = []
  const timers = fakeTimers()
  const options = {
    fetchFn: (url) => { calls.push(url); return Promise.resolve() },
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
  }

  const first = startDesktopSession(options)
  first.stop()                       // StrictMode 的第一次清理
  const second = startDesktopSession(options)

  const afterRemount = calls.length
  assert.equal(afterRemount, 2)      // 两个会话各跳了一次
  assert.equal(timers.count, 1)      // 旧会话的 timer 已被清掉

  timers.tick()
  assert.equal(calls.length, afterRemount + 1)
  assert.equal(timers.count, 1)

  second.stop()
  timers.tick()
  assert.equal(calls.length, afterRemount + 1)
})

// ---- 心跳响应发布：把 /api/status 的有效响应体交给统计状态模型 ----

async function flushMicrotasks() {
  for (let i = 0; i < 8; i += 1) await Promise.resolve()
}

test('心跳解析响应体并带发起时的 token 交给 onStatus', async () => {
  const events = []
  const timers = fakeTimers()
  let issued = 0
  const session = startDesktopSession({
    fetchFn: () => Promise.resolve({ ok: true, json: async () => ({ symbols: 4, footprints: 2 }) }),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
    issueStatusToken: () => { issued += 1; return issued },
    onStatus: (event) => events.push(event),
  })

  await flushMicrotasks()
  assert.equal(events.length, 1)
  assert.equal(events[0].token, 1)
  assert.deepEqual(events[0].payload, { symbols: 4, footprints: 2 })

  timers.tick()
  await flushMicrotasks()
  assert.equal(events.length, 2)
  assert.equal(events[1].token, 2)
  session.stop()
})

test('没有响应体的桩响应只报告可达（payload 为 null），不抛错', async () => {
  const events = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: () => Promise.resolve({ ok: true }),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
    onStatus: (event) => events.push(event),
  })

  await flushMicrotasks()
  assert.equal(events.length, 1)
  // 没有 issueStatusToken 时事件显式带 null，模型按「发起时刻未知」自行归属
  assert.equal(events[0].token, null)
  assert.equal(events[0].payload, null)
  session.stop()
})

test('响应体解析失败：抛给 onStatus 的 error，不产生未处理的 rejection', async () => {
  const events = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: () => Promise.resolve({ ok: true, json: () => Promise.reject(new Error('bad json')) }),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
    onStatus: (event) => events.push(event),
  })

  await flushMicrotasks()
  assert.equal(events.length, 1)
  assert.equal(events[0].error.message, 'bad json')
  session.stop()
})

test('HTTP 失败：onStatus 收到错误事件，而不是把空统计当成成功', async () => {
  const events = []
  const timers = fakeTimers()
  const session = startDesktopSession({
    fetchFn: () => Promise.resolve({ ok: false, status: 503 }),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
    onStatus: (event) => events.push(event),
  })

  await flushMicrotasks()
  assert.equal(events.length, 1)
  assert.match(events[0].error.message, /503/)
  session.stop()
})

test('stop 之后晚到的响应不再交给 onStatus', async () => {
  const events = []
  const timers = fakeTimers()
  let resolveFetch
  const session = startDesktopSession({
    fetchFn: () => new Promise((resolve) => { resolveFetch = resolve }),
    setTimer: timers.setTimer,
    clearTimer: timers.clearTimer,
    windowLike: null,
    navigatorLike: null,
    onStatus: (event) => events.push(event),
  })

  session.stop()
  resolveFetch({ ok: true, json: async () => ({ symbols: 4, footprints: 2 }) })
  await flushMicrotasks()
  assert.equal(events.length, 0)
})
