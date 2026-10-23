// 刷新必须读“最新”的查询状态：操作等待期间用户改了关键词/目标库，
// 完成后的刷新要用新值；workspace 切换先同步清空、完成时再读最新。
import test from 'node:test'
import assert from 'node:assert/strict'

import { createSymbolRefresher, switchWorkspaceQuery, selectAllQuery } from '../../src/lib/symbolRefresh.js'

test('刷新把关键词和目标库一起传给 load，且读的是调用时的最新值', async () => {
  const calls = []
  const load = (search, lib) => { calls.push([search, lib]); return Promise.resolve('fresh') }
  const query = { search: 'res', targetLib: 'lib-1' }
  const refresh = createSymbolRefresher(load, () => query)

  assert.equal(await refresh(), 'fresh')    // 返回 load 的结果（load 是异步的）
  assert.deepEqual(calls.at(-1), ['res', 'lib-1'])

  query.search = 'cap'
  query.targetLib = 'lib-2'
  await refresh()
  assert.deepEqual(calls.at(-1), ['cap', 'lib-2']) // 两次调用各自用当次的最新值

  query.search = ''
  query.targetLib = ''
  await refresh()
  assert.deepEqual(calls.at(-1), ['', ''])  // 空查询也要显式传两个参数
})

test('All：关键词保留，只清目标库并按最新 ref 刷新', async () => {
  const calls = []
  const query = { search: 'res', targetLib: 'lib-1' }
  const load = (search, lib) => { calls.push([search, lib]); return Promise.resolve('ok') }
  const refresh = createSymbolRefresher(load, () => query)

  // App 的 All 接线就是这一对：setTargetLib 同步改 ref，然后 refresh 读最新
  await selectAllQuery({ resetTargetLib: (v) => { query.targetLib = v }, refresh })

  assert.deepEqual(calls.at(-1), ['res', ''])  // 关键词保留，目标库已清空
  assert.equal(query.search, 'res')            // 关键词没有被顺手清掉
  assert.equal(query.targetLib, '')
})

test('查询缺失时按空查询处理，不会把 undefined 传给后端', () => {
  const calls = []
  const load = (search, lib) => { calls.push([search, lib]); return Promise.resolve() }
  createSymbolRefresher(load, () => undefined)()
  assert.deepEqual(calls.at(-1), ['', ''])
})

test('等待中的操作完成后刷新：期间用户改了查询就读新查询', async () => {
  const calls = []
  const query = { search: 'old', targetLib: 'lib-old' }
  const load = (search, lib) => { calls.push([search, lib]); return Promise.resolve() }
  const refresh = createSymbolRefresher(load, () => query)

  // 模拟一次 classify/delete/import：await 期间用户改了查询
  let release
  const pendingOperation = new Promise(resolve => { release = resolve })
  const flow = (async () => {
    await pendingOperation
    refresh()                                // 操作完成后的刷新
  })()

  query.search = 'q2'
  query.targetLib = 'lib2'
  release()
  await flow

  assert.deepEqual(calls.at(-1), ['q2', 'lib2']) // 不是发起操作时的 old/lib-old
})

test('workspace 切换：起始同步清空查询，完成时按最新查询刷新（不被空查询覆盖）', async () => {
  const events = []
  const query = { search: 'q1', targetLib: 'lib1' }
  const load = (search, lib) => { events.push(['load', search, lib]); return Promise.resolve() }
  const refresh = createSymbolRefresher(load, () => query)

  let release
  const save = () => new Promise(resolve => { release = resolve })

  const switching = switchWorkspaceQuery({
    resetQuery: (search, lib) => {
      query.search = search
      query.targetLib = lib
      events.push(['reset', search, lib])
    },
    loadFresh: (search, lib) => load(search, lib),
    save,
    refresh,
  })

  // 起始：同步清空 UI 查询与 ref，并立刻取一次空查询
  assert.deepEqual(events[0], ['reset', '', ''])
  assert.deepEqual(query, { search: '', targetLib: '' })
  await new Promise(resolve => setImmediate(resolve)) // 等 save() 真正发出

  // 用户在等待保存期间输入 q2 / 选择 lib2
  query.search = 'q2'
  query.targetLib = 'lib2'
  release()
  await switching

  assert.deepEqual(events.at(-1), ['load', 'q2', 'lib2']) // 完成时读最新，不被 '' 覆盖
  assert.deepEqual(
    events.filter(e => e[0] === 'load').map(e => [e[1], e[2]]),
    [['', ''], ['q2', 'lib2']],
  )
})
