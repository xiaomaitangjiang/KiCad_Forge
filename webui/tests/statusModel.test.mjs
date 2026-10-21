// 全库统计（symbols / footprints）的专用状态模型：
// 忙态、缺字段、格式错误、读取失败都不得把最后一份有效统计清空或伪造成 0；
// 显式刷新与桌面心跳共用同一套校验、请求归属与失效保护。
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  createStatusModel,
  readStatusCounts,
  startDesktopSession,
  statusCountText,
} from '../src/lib/desktopSession.js'

const full = (symbols, footprints) => ({ ok: true, symbols, footprints })
const busy = Object.freeze({ ok: true, busy: true })

function activeModel() {
  const model = createStatusModel()
  model.activate()
  return model
}

function flush() {
  let pending = Promise.resolve()
  for (let i = 0; i < 8; i += 1) pending = pending.then(() => {})
  return pending
}

test('readStatusCounts：只接受完整、非忙、非负整数统计', () => {
  assert.deepEqual(readStatusCounts({ symbols: 0, footprints: 0 }), { symbols: 0, footprints: 0 })
  assert.deepEqual(
    readStatusCounts({ ok: true, busy: false, symbols: 4, footprints: 2 }),
    { symbols: 4, footprints: 2 },
  )

  const rejected = [
    null,
    undefined,
    'busy',
    4,
    [],
    {},
    { symbols: 4 },
    { footprints: 2 },
    { symbols: 4, footprints: 2, busy: true },
    { symbols: 4, footprints: 2, ok: false },
    { symbols: -1, footprints: 2 },
    { symbols: 1.5, footprints: 2 },
    { symbols: '4', footprints: 2 },
    { symbols: 4, footprints: null },
  ]
  for (const payload of rejected) {
    assert.equal(readStatusCounts(payload), null, JSON.stringify(payload))
  }
})

test('4 → busy → 4：忙态保留最后有效统计，不发布新快照也不伪称已刷新', () => {
  const model = activeModel()
  const first = model.beginRead()
  assert.equal(model.publish(first, full(4, 2)).status, 'published')
  assert.equal(model.getSnapshot().symbols, 4)

  const refreshing = model.beginRead()
  assert.equal(model.getSnapshot().updating, true)
  const outcome = model.publish(refreshing, busy)
  assert.equal(outcome.status, 'failed')
  assert.equal(outcome.reason, 'busy')

  const snap = model.getSnapshot()
  assert.equal(snap.known, true)
  assert.equal(snap.symbols, 4)
  assert.equal(snap.footprints, 2)
  assert.equal(snap.stale, true)
  assert.equal(snap.reason, 'busy')
  assert.equal(snap.updating, false)
  assert.equal(statusCountText(snap, 'symbols'), '4')
  assert.equal(statusCountText(snap, 'footprints'), '2')

  const beat = model.beginHeartbeat()
  assert.equal(model.observeHeartbeat({ token: beat, payload: full(4, 2) }).status, 'published')
  assert.equal(model.getSnapshot().stale, false)
  assert.equal(model.getSnapshot().alive, true)
})

test('4 → 读取失败 → 4：网络错误只标记过期，不清空旧值', () => {
  const model = activeModel()
  model.publish(model.beginRead(), full(4, 2))

  const token = model.beginRead()
  const outcome = model.fail(token, { kind: 'network', message: '无法连接本地后台' })
  assert.equal(outcome.status, 'failed')
  assert.equal(outcome.error.message, '无法连接本地后台')

  const snap = model.getSnapshot()
  assert.equal(snap.symbols, 4)
  assert.equal(snap.footprints, 2)
  assert.equal(snap.stale, true)
  assert.equal(snap.reason, 'error')
  assert.equal(snap.error, '无法连接本地后台')
  assert.equal(statusCountText(snap, 'symbols'), '4')
})

test('0 → busy → 0：真实的 0 是已知统计，不被当成未知或空值', () => {
  const model = activeModel()
  model.publish(model.beginRead(), full(0, 0))
  assert.equal(model.getSnapshot().known, true)
  assert.equal(statusCountText(model.getSnapshot(), 'symbols'), '0')

  model.publish(model.beginRead(), busy)
  const snap = model.getSnapshot()
  assert.equal(snap.symbols, 0)
  assert.equal(snap.footprints, 0)
  assert.equal(statusCountText(snap, 'symbols'), '0')
  assert.equal(statusCountText(snap, 'footprints'), '0')
})

test('首次 busy：统计未知显示省略号，不伪造 0，也不伪称已刷新', () => {
  const model = activeModel()
  const outcome = model.publish(model.beginRead(), busy)
  assert.equal(outcome.status, 'failed')
  assert.equal(outcome.reason, 'busy')

  const snap = model.getSnapshot()
  assert.equal(snap.known, false)
  assert.equal(snap.symbols, null)
  assert.equal(snap.footprints, null)
  assert.equal(snap.stale, false)
  assert.equal(statusCountText(snap, 'symbols'), '…')
  assert.equal(statusCountText(snap, 'footprints'), '…')

  model.observeHeartbeat({ token: model.beginHeartbeat(), payload: busy })
  assert.equal(model.getSnapshot().known, false)
  assert.equal(model.getSnapshot().symbols, null)
})

test('缺字段或格式错误：保留旧值并标记过期，不被当成新统计', () => {
  const model = activeModel()
  model.publish(model.beginRead(), full(4, 2))

  for (const payload of [{ symbols: 5 }, { symbols: '5', footprints: 2 }, { symbols: -1, footprints: 2 }]) {
    const outcome = model.publish(model.beginRead(), payload)
    assert.equal(outcome.status, 'failed')
    assert.equal(outcome.reason, 'invalid')
    const snap = model.getSnapshot()
    assert.equal(snap.symbols, 4)
    assert.equal(snap.footprints, 2)
    assert.equal(snap.stale, true)
  }
})

test('健康心跳恢复统计：首次读取失败后，心跳的完整响应成为新快照', () => {
  const model = activeModel()
  model.beginRead()
  model.fail(model.beginRead(), { kind: 'network', message: 'offline' })
  // 上面多开了一个 token，先发布一份旧的失败不会影响后续恢复
  assert.equal(model.getSnapshot().known, false)

  const beat = model.beginHeartbeat()
  const outcome = model.observeHeartbeat({ token: beat, payload: full(3, 1) })
  assert.equal(outcome.status, 'published')
  const snap = model.getSnapshot()
  assert.equal(snap.known, true)
  assert.equal(snap.symbols, 3)
  assert.equal(snap.footprints, 1)
  assert.equal(snap.stale, false)
  assert.equal(snap.alive, true)
  assert.equal(statusCountText(snap, 'symbols'), '3')
})

test('旧请求迟到：较新的归属已发布后，旧响应不得覆盖新值', () => {
  const model = activeModel()
  const older = model.beginRead()
  const newer = model.beginRead()

  assert.equal(model.publish(newer, full(9, 9)).status, 'published')
  const late = model.publish(older, full(1, 1))
  assert.equal(late.status, 'superseded')

  const snap = model.getSnapshot()
  assert.equal(snap.symbols, 9)
  assert.equal(snap.footprints, 9)
})

test('旧心跳迟到：显式刷新已发布后，早先发起的心跳响应不得覆盖', () => {
  const model = activeModel()
  const oldBeat = model.beginHeartbeat()
  const read = model.beginRead()
  model.publish(read, full(7, 7))

  const late = model.observeHeartbeat({ token: oldBeat, payload: full(2, 2) })
  assert.equal(late.status, 'superseded')
  assert.equal(model.getSnapshot().symbols, 7)
})

test('卸载后晚到的响应不通知、不落地；重挂后从新请求恢复', () => {
  const model = activeModel()
  let notifications = 0
  const unsubscribe = model.subscribe(() => { notifications += 1 })
  const token = model.beginRead()
  const before = notifications

  model.deactivate()
  assert.equal(model.publish(token, full(5, 5)).status, 'aborted')
  assert.equal(notifications, before)
  assert.equal(model.getSnapshot().known, false)

  model.activate()
  assert.equal(model.publish(model.beginRead(), full(5, 5)).status, 'published')
  assert.equal(model.getSnapshot().symbols, 5)
  assert.ok(notifications > before)
  unsubscribe()
})

test('有效发布才递增 revision 并通知订阅者；忙态保留旧数字', () => {
  const model = activeModel()
  const revisions = []
  const unsubscribe = model.subscribe(snap => revisions.push(snap.revision))

  model.publish(model.beginRead(), full(4, 2))
  model.publish(model.beginRead(), busy)
  model.publish(model.beginRead(), full(4, 2))

  // 每次 beginRead 会发布「刷新中」，每次 publish 再发布结果，共 6 次通知。
  assert.equal(revisions.length, 6)
  assert.equal(revisions.at(-1), 6)
  assert.equal(statusCountText(model.getSnapshot(), 'symbols'), '4')
  unsubscribe()
})

test('startDesktopSession 的心跳结果进入同一统计模型：忙态不清空、健康心跳恢复', async () => {
  const bodies = [busy, full(4, 2)]
  const model = activeModel()
  model.publish(model.beginRead(), full(4, 2))

  let scheduled = null
  startDesktopSession({
    fetchFn: () => Promise.resolve({ ok: true, json: async () => bodies.shift() ?? busy }),
    setTimer: (fn) => { scheduled = fn; return 1 },
    clearTimer: () => {},
    windowLike: null,
    navigatorLike: null,
    issueStatusToken: () => model.beginHeartbeat(),
    onStatus: (event) => model.observeHeartbeat(event),
  })

  await flush()
  assert.equal(model.getSnapshot().symbols, 4)
  assert.equal(model.getSnapshot().stale, true)

  scheduled()
  await flush()
  assert.equal(model.getSnapshot().stale, false)
  assert.equal(model.getSnapshot().alive, true)
  assert.equal(statusCountText(model.getSnapshot(), 'symbols'), '4')
})

test('心跳与在途统计读取共用归属：忙态心跳不会把正在进行的读取判成过期', () => {
  const model = activeModel()
  const read = model.beginRead()
  const beat = model.beginHeartbeat()
  assert.equal(beat, read, '读取在途时心跳复用同一归属')

  // 心跳先看到忙态：保留旧值并标记过期，但这次读取还没结束。
  assert.equal(model.observeHeartbeat({ token: beat, payload: busy }).status, 'failed')
  assert.equal(model.getSnapshot().updating, true)

  // 读取随后成功：同一归属的更新结果仍然可以发布，不被心跳判成过期。
  assert.equal(model.publish(read, full(4, 2)).status, 'published')
  const snap = model.getSnapshot()
  assert.equal(snap.symbols, 4)
  assert.equal(snap.stale, false)
  assert.equal(snap.updating, false)
})
