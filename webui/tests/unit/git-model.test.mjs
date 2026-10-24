import test from 'node:test'
import assert from 'node:assert/strict'
import { createSnapshotStore } from '@kiforge/plugin-sdk'
import { createTasksApi } from '../../src/plugin-host/tasks-api.js'
import { createGitModel, componentBase, HISTORY_MAX_COMMITS } from '../../../plugins/git_version/frontend/src/model.ts'

const tick = () => new Promise((resolve) => setImmediate(resolve))

function handle(action, cancelable = true) {
  const store = createSnapshotStore({ id: String(Math.random()), action, state: 'running', cancelable })
  let subscriptions = 0, cancellations = 0
  return {
    ...store,
    subscribe(listener) {
      subscriptions++
      const off = store.subscribe(listener)
      return () => { off(); subscriptions-- }
    },
    async cancel() { cancellations++; return { status: 'cancel_requested' } },
    finish(result = { ok: true, initialized: true, branch: 'main', files: [] }) {
      store.set({ ...store.getSnapshot(), state: 'succeeded', result })
    },
    fail(message) {
      store.set({ ...store.getSnapshot(), state: 'failed', error: message })
    },
    get subscriptions() { return subscriptions },
    get cancellations() { return cancellations },
  }
}

function commit(oid, parents = []) {
  return {
    oid, parents, subject: `subject ${oid}`, author_name: 'Xiaom',
    author_time: '2026-10-20T10:00:00+08:00', committer_time: '2026-10-20T10:00:00+08:00',
  }
}

function historyResult(commits, { cursor = null, hasMore = false } = {}) {
  return {
    ok: true,
    schema_version: 'graph_v1',
    repository_identity: 'C:/One',
    head_oid: commits[0]?.oid ?? '',
    refs: commits[0] ? [{ name: 'main', kind: 'local', target_oid: commits[0].oid, is_head: true }] : [],
    commits,
    page: { snapshot_id: 'snap-1', next_cursor: cursor, has_more: hasMore },
  }
}

function detailResult(oid, parents = [], files = []) {
  return { ok: true, schema_version: 'commit_detail_v1', commit: commit(oid, parents), files }
}

function fixture({ failHistory = () => false } = {}) {
  const calls = [], handles = []
  let libraryReads = 0
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: {
      async componentLibraries() {
        libraryReads++
        return [
          { id: 'one', name: 'One', symbolPath: 'C:/One/symbols', footprintPath: 'C:/One/footprints' },
          { id: 'two', name: 'Two', symbolPath: 'C:/Two/symbols', footprintPath: 'C:/Two/footprints' },
        ]
      },
    },
    tasks: {
      async run(action, args) {
        calls.push({ action, args })
        if (action === 'history' && failHistory()) throw new Error('HISTORY_DOWN')
        const h = handle(action, !['init', 'commit'].includes(action))
        handles.push(h)
        return h
      },
    },
  })
  return { model, calls, handles, libraryReads: () => libraryReads }
}

async function ready(f, { branch = 'main' } = {}) {
  await f.model.start()
  await tick()
  f.handles[0].finish({ ok: true, initialized: true, branch, files: [] })
  await tick()
}

test('Git starts once across StrictMode and synchronously prevents duplicate reads and writes', async () => {
  const f = fixture()
  await f.model.start()
  await f.model.start()
  await tick()
  assert.equal(f.libraryReads(), 1)
  assert.equal(f.calls.length, 1)
  assert.equal(f.calls[0].action, 'status')

  f.model.selectTab('history')
  f.model.selectTab('history')
  assert.equal(f.calls.length, 2)
  assert.equal(f.calls[1].action, 'history')
  assert.equal(f.calls[1].args.format, 'graph_v1')
  assert.equal(f.calls[1].args.limit, 50)

  f.model.runStatus()
  f.model.runStatus()
  assert.equal(f.calls.filter((call) => call.action === 'status').length, 1)

  f.model.setMessage('commit once')
  f.model.runCommit()
  f.model.runCommit()
  assert.equal(f.calls.filter((call) => call.action === 'commit').length, 1)
  f.model.dispose()
})

test('Git target changes cancel only the old read and the newest status wins', async () => {
  const f = fixture()
  await f.model.start()
  await tick()
  assert.equal(f.calls.length, 1)
  f.model.setTarget('C:/Two')
  f.model.setTarget('C:/Three')
  await tick()
  assert.equal(f.handles[0].cancellations, 1)
  assert.equal(f.handles[1].cancellations, 1)
  assert.equal(f.calls[2].args.target_library, 'C:/Three')
  f.handles[0].finish({ ok: true, initialized: true, branch: 'OLD', files: [] })
  await tick()
  assert.equal(f.model.getSnapshot().status.data, null)
  f.handles[2].finish({ ok: true, initialized: true, branch: 'NEW', files: [] })
  await tick()
  assert.equal(f.model.getSnapshot().status.data.branch, 'NEW')
  f.model.dispose()
})

test('Git write tasks survive UI subscription release, keep their message and cannot be actively cancelled', async () => {
  const f = fixture()
  await ready(f)
  f.model.setMessage('kept message')
  f.model.runCommit()
  await tick()
  assert.equal(f.calls[1].action, 'commit')
  let updates = 0
  const off = f.model.subscribe(() => updates++)
  off()
  f.model.setTarget('C:/Two')
  await f.model.cancel()
  assert.equal(f.handles[1].cancellations, 0)
  assert.equal(f.handles[1].subscriptions, 1)
  f.handles[1].finish({ ok: true })
  await tick()
  assert.equal(updates, 0)
  assert.equal(f.model.getSnapshot().message, 'kept message')
  assert.ok(f.calls.some((call) => call.action === 'status' && call.args.target_library === 'C:/Two'))
  f.model.dispose()
  for (const h of f.handles) assert.equal(h.subscriptions, 0)
})

test('Git late handles after disposal cannot republish or keep subscriptions', async () => {
  let resolve
  const pending = new Promise((yes) => { resolve = yes })
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: { componentLibraries: async () => [] },
    tasks: { run: () => pending },
  })
  model.setTarget('C:/One')
  const h = handle('status')
  model.dispose()
  resolve(h)
  await tick()
  assert.equal(h.subscriptions, 0)
  assert.equal(h.cancellations, 0)
})

test('Git rejects malformed status data and recovers on the next read', async () => {
  const f = fixture()
  await f.model.start()
  await tick()
  f.handles[0].finish({ ok: true, files: [{ status: 'M', path: 42 }] })
  await tick()
  assert.equal(f.model.getSnapshot().status.data, null)
  assert.equal(f.model.getSnapshot().status.phase, 'error')
  assert.match(f.model.getSnapshot().status.error, /status result/i)
  f.model.runStatus()
  await tick()
  f.handles[1].finish()
  await tick()
  assert.equal(f.model.getSnapshot().status.data.initialized, true)
  assert.equal(f.model.getSnapshot().status.phase, 'ready')
  f.model.dispose()
})

test('Git common base preserves single-file roots, Unicode and unrelated drive paths', () => {
  assert.equal(componentBase({ symbolPath: 'C:/库/MAIN.KICAD_SYM', footprintPath: 'C:/库/封装' }), 'C:/库')
  assert.equal(componentBase({ symbolPath: 'C:/Symbols', footprintPath: 'D:/Footprints' }), 'C:/Symbols')
})

test('Git retries a failed start when reopened while sharing parallel and successful starts', async () => {
  let reads = 0
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: {
      async componentLibraries() {
        if (++reads === 1) throw new Error('TRANSIENT')
        return []
      },
    },
    tasks: { run: async () => { throw new Error('No target should run') } },
  })
  await Promise.all([model.start(), model.start()])
  assert.equal(reads, 1)
  assert.equal(model.getSnapshot().loadError, 'TRANSIENT')
  await Promise.all([model.start(), model.start()])
  assert.equal(reads, 2)
  assert.equal(model.getSnapshot().loadError, '')
  await model.start()
  assert.equal(reads, 2)
  model.dispose()
})

test('Git releases a handle that reports terminal state synchronously during subscribe', async () => {
  let subscriptions = 0
  const task = {
    id: 'instant', action: 'status', state: 'succeeded', cancelable: true,
    result: { ok: true, initialized: true, branch: 'main', files: [] },
  }
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: { componentLibraries: async () => [] },
    tasks: {
      run: async () => ({
        id: task.id,
        getSnapshot: () => task,
        subscribe(listener) {
          subscriptions++
          listener()
          let released = false
          return () => { if (!released) { released = true; subscriptions-- } }
        },
      }),
    },
  })
  model.setTarget('C:/One')
  await tick()
  assert.equal(model.getSnapshot().running, false)
  assert.equal(model.getSnapshot().status.phase, 'ready')
  assert.equal(model.getSnapshot().status.data.branch, 'main')
  assert.equal(subscriptions, 0)
  model.dispose()
})

test('Git history keeps old results while refreshing and remembers scroll (S2/S3)', async () => {
  const f = fixture()
  await ready(f)
  f.model.setScrollTop(42)
  f.model.selectTab('history')
  await tick()
  assert.equal(f.calls[1].action, 'history')
  f.handles[1].finish(historyResult([commit('a', ['b']), commit('b')], { cursor: 'c1', hasMore: true }))
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 2)
  assert.equal(snap.scrollTop, 42)

  f.model.selectTab('history')
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'loading')
  assert.equal(snap.history.data.commits.length, 2)
  f.handles[2].finish(historyResult([commit('c', ['a']), commit('a', ['b']), commit('b')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 3)
  assert.equal(snap.scrollTop, 42)

  // A status refresh is an independent resource and must not hide history.
  f.model.runStatus()
  await tick()
  f.handles[3].finish()
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 3)
  f.model.dispose()
})

test('Git history range changes cancel the old request and ignore its late result', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult([commit('a')]))
  await tick()
  f.model.selectTab('history')
  await tick()
  const stale = f.handles[2]
  f.model.setHistoryRange('branch', 'main')
  await tick()
  assert.equal(stale.cancellations, 1)
  const call = f.calls[f.calls.length - 1]
  assert.equal(call.action, 'history')
  assert.equal(call.args.range, 'branch')
  assert.equal(call.args.branch_ref, 'main')
  stale.finish(historyResult([commit('old')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.data, null)
  f.handles[3].finish(historyResult([commit('new')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.data.commits[0].oid, 'new')
  f.model.dispose()
})

test('Git append pages merge by oid and stop at the 500 commit cap', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult(
    [commit('a'), commit('b'), commit('c')],
    { cursor: 'page-2', hasMore: true },
  ))
  await tick()
  f.model.loadMore()
  await tick()
  assert.equal(f.calls[2].args.cursor, 'page-2')
  f.handles[2].finish(historyResult(
    [commit('c'), commit('d')],
    { cursor: 'page-3', hasMore: true },
  ))
  await tick()
  let snap = f.model.getSnapshot()
  assert.deepEqual(snap.history.data.commits.map((entry) => entry.oid), ['a', 'b', 'c', 'd'])

  const rest = Array.from({ length: HISTORY_MAX_COMMITS - 4 }, (_, index) => commit(`x${index}`))
  const all = snap.history.data.commits.concat(rest)
  f.model.refreshHistory()
  await tick()
  f.handles[f.handles.length - 1].finish(historyResult(all, { cursor: 'full', hasMore: true }))
  await tick()
  const count = f.calls.length
  f.model.loadMore()
  await tick()
  assert.equal(f.calls.length, count)
  assert.equal(f.model.getSnapshot().history.data.commits.length, HISTORY_MAX_COMMITS)
  f.model.dispose()
})

test('Git stale history cursors keep the loaded page and require a refresh', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult([commit('a')], { cursor: 'old-cursor', hasMore: true }))
  await tick()
  f.model.loadMore()
  await tick()
  f.handles[2].finish({ ok: false, code: 'stale_cursor', error: 'history moved' })
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'stale')
  assert.match(snap.history.error, /refresh|刷新/i)
  assert.equal(snap.history.data.commits.length, 1)
  const count = f.calls.length
  f.model.loadMore()
  await tick()
  assert.equal(f.calls.length, count)

  f.model.refreshHistory()
  await tick()
  const call = f.calls[f.calls.length - 1]
  assert.equal(call.action, 'history')
  assert.equal(call.args.cursor, undefined)
  f.handles[f.handles.length - 1].finish(historyResult([commit('a'), commit('b')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 2)
  f.model.dispose()
})

test('Git commit success invalidates read resources, clears selection and refetches the active view', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult([commit('a', ['b']), commit('b')]))
  await tick()
  f.model.selectCommit('a')
  await tick()
  f.handles[2].finish(detailResult('a', ['b'], [{ path: 'Power.kicad_sym', status: 'M' }]))
  await tick()
  assert.equal(f.model.getSnapshot().detail.phase, 'ready')

  f.model.setMessage('commit this')
  f.model.runCommit()
  await tick()
  const commitCall = f.calls[f.calls.length - 1]
  assert.equal(commitCall.action, 'commit')
  f.handles[3].finish({ ok: true })
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.message, '')
  assert.equal(snap.selectedOid, null)
  assert.equal(snap.history.phase, 'stale')
  assert.equal(f.calls[f.calls.length - 1].action, 'status')

  f.handles[4].finish()
  await tick()
  const historyCall = f.calls[f.calls.length - 1]
  assert.equal(historyCall.action, 'history')
  assert.equal(historyCall.args.cursor, undefined)
  assert.equal(f.model.getSnapshot().history.phase, 'loading')
  f.handles[5].finish(historyResult([commit('c', ['a']), commit('a', ['b']), commit('b')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 3)
  assert.equal(f.calls.filter((call) => call.action === 'commit').length, 1)
  f.model.dispose()
})

test('Git commit detail and merge-parent file diffs follow the active selection', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult([commit('merge', ['p1', 'p2']), commit('p1'), commit('p2')]))
  await tick()
  f.model.selectCommit('merge')
  await tick()
  assert.equal(f.calls[2].action, 'commit_detail')
  assert.equal(f.calls[2].args.oid, 'merge')
  assert.equal(f.calls[2].args.parent_index, 0)
  f.handles[2].finish(detailResult('merge', ['p1', 'p2'], [
    { path: 'Power.kicad_sym', status: 'M', additions: 2, deletions: 1 },
    { path: 'Packages.pretty/TSSOP-20.kicad_mod', binary: true, additions: null, deletions: null },
  ]))
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.detail.phase, 'ready')
  assert.equal(snap.detail.data.files.length, 2)
  assert.equal(snap.detail.data.target, 'C:/One')
  assert.equal(snap.detail.data.oid, 'merge')
  assert.equal(snap.detail.data.parentIndex, 0)

  // The legacy string call is accepted only because the selected detail agrees.
  f.model.openFile('Power.kicad_sym')
  await tick()
  assert.equal(f.calls[3].action, 'commit_file')
  assert.equal(f.calls[3].args.parent_index, 0)
  assert.equal(f.calls[3].args.parent, 'p1')
  f.handles[3].finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'merge', path: 'Power.kicad_sym',
    patch: '- old\n+ new\n', truncated: false, binary: false,
  })
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'ready')
  assert.equal(snap.fileDiff.data.target, 'C:/One')
  assert.equal(snap.fileDiff.data.parentIndex, 0)
  assert.match(snap.fileDiff.data.patch, /\+ new/)

  // Switching the comparison parent closes the old expansion; it must not
  // re-dispatch the same path under the new parent.
  const fileCalls = f.calls.filter((call) => call.action === 'commit_file').length
  f.model.setParentIndex(1)
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.parentIndex, 1)
  assert.equal(snap.file, null)
  assert.equal(snap.fileDiff.phase, 'idle')
  assert.equal(snap.fileDiff.data, null)
  assert.equal(snap.detail.phase, 'loading')
  assert.equal(snap.detail.data, null)
  assert.equal(f.calls.filter((call) => call.action === 'commit_file').length, fileCalls)
  const parentDetail = f.calls[f.calls.length - 1]
  assert.equal(parentDetail.action, 'commit_detail')
  assert.equal(parentDetail.args.parent_index, 1)
  assert.equal(parentDetail.args.parent, 'p2')
  f.handles[f.handles.length - 1].finish(detailResult('merge', ['p1', 'p2'], [
    { path: 'Power.kicad_sym', status: 'M' },
  ]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.detail.phase, 'ready')
  assert.equal(snap.detail.data.parentIndex, 1)

  f.model.openFile({ oid: 'merge', path: 'Power.kicad_sym', parentIndex: 1 })
  await tick()
  const parentFile = f.calls[f.calls.length - 1]
  assert.equal(parentFile.action, 'commit_file')
  assert.equal(parentFile.args.parent_index, 1)
  assert.equal(parentFile.args.parent, 'p2')
  f.handles[f.handles.length - 1].finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'merge', path: 'Power.kicad_sym',
    patch: '- p1\n+ p2\n', truncated: false, binary: false,
  })
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'ready')
  assert.equal(snap.fileDiff.data.parentIndex, 1)
  assert.match(snap.fileDiff.data.patch, /\+ p2/)

  // A file that is not in the current detail must not become clickable state.
  const beforeMissing = f.calls.length
  f.model.openFile({ oid: 'merge', path: 'Missing.kicad_sym', parentIndex: 1 })
  assert.equal(f.calls.length, beforeMissing)

  f.model.closeFile()
  assert.equal(f.model.getSnapshot().file, null)
  assert.equal(f.model.getSnapshot().fileDiff.phase, 'idle')

  const calls = f.calls.length
  f.model.selectCommit('p1')
  await tick()
  const rootDetail = f.calls[f.calls.length - 1]
  assert.equal(rootDetail.action, 'commit_detail')
  assert.equal(rootDetail.args.parent_index, undefined)
  assert.equal(rootDetail.args.parent, undefined)
  f.handles[f.handles.length - 1].finish(detailResult('p1'))
  await tick()
  // A -> B -> A is served from the identity-keyed detail cache.
  const beforeCached = f.calls.length
  f.model.selectCommit('merge')
  await tick()
  assert.equal(f.calls.length, beforeCached)
  snap = f.model.getSnapshot()
  assert.equal(snap.detail.phase, 'ready')
  assert.equal(snap.detail.data.oid, 'merge')
  assert.equal(snap.detail.data.parentIndex, 0)
  f.model.dispose()
})

test('Git target changes release repository scoped state and subscriptions (S2)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles[1].finish(historyResult([commit('a', ['b']), commit('b')]))
  await tick()
  f.model.selectCommit('a')
  await tick()
  f.model.setFind('abc')
  f.model.setScrollTop(180)
  const detailHandle = f.handles[2]
  assert.equal(detailHandle.subscriptions, 1)

  f.model.setTarget('C:/Two')
  await tick()
  assert.equal(detailHandle.subscriptions, 0)
  assert.equal(f.handles[1].subscriptions, 0)
  const snap = f.model.getSnapshot()
  assert.equal(snap.tab, 'history')
  assert.equal(snap.history.data, null)
  assert.equal(snap.history.phase, 'idle')
  assert.equal(snap.selectedOid, null)
  assert.equal(snap.find, '')
  assert.equal(snap.scrollTop, 0)
  assert.equal(snap.range, 'all')
  assert.equal(snap.branchRef, '')
  assert.equal(f.calls[f.calls.length - 1].args.target_library, 'C:/Two')
  f.model.dispose()
  for (const h of f.handles) assert.equal(h.subscriptions, 0)
})
test('Git read creation failures surface a reason and can be retried', async () => {
  const calls = [], handles = []
  let failHistory = true
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: {
      async componentLibraries() {
        return [{ id: 'one', name: 'One', symbolPath: 'C:/One/symbols', footprintPath: 'C:/One/footprints' }]
      },
    },
    tasks: {
      async run(action, args) {
        calls.push({ action, args })
        if (failHistory && action === 'history') throw new Error('HOST_DOWN')
        const h = handle(action)
        handles.push(h)
        return h
      },
    },
  })
  await model.start()
  await tick()
  handles[0].finish()
  await tick()
  model.selectTab('history')
  await tick()
  let snap = model.getSnapshot()
  assert.equal(snap.history.phase, 'error')
  assert.match(snap.history.error, /HOST_DOWN/)
  assert.equal(snap.running, false)
  assert.equal(snap.canCancel, false)

  failHistory = false
  model.refreshHistory()
  await tick()
  handles[handles.length - 1].finish(historyResult([commit('a')]))
  await tick()
  snap = model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits.length, 1)
  model.dispose()
})

test('Git cancels only in-flight reads and never leaves fake loading', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  assert.equal(f.model.getSnapshot().running, true)
  assert.equal(f.model.getSnapshot().canCancel, true)
  await f.model.cancel()
  const snap = f.model.getSnapshot()
  assert.equal(snap.running, false)
  assert.equal(snap.canCancel, false)
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(f.handles[1].cancellations, 1)
  f.handles[1].finish(historyResult([commit('late')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.data, null)
  f.model.dispose()
})

test('Git stops only the history read without cancelling unrelated status work', async () => {
  const f = fixture()
  await ready(f)
  f.model.runStatus()
  f.model.selectTab('history')
  await tick()
  const statusRead = f.handles[1]
  const historyRead = f.handles[2]
  assert.equal(f.model.getSnapshot().status.phase, 'loading')
  assert.equal(f.model.getSnapshot().history.phase, 'loading')
  await f.model.cancelChannel('history')
  let snap = f.model.getSnapshot()
  assert.equal(historyRead.cancellations, 1)
  assert.equal(statusRead.cancellations, 0)
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(snap.status.phase, 'loading')
  assert.equal(snap.running, true)
  statusRead.finish({ ok: true, initialized: true, branch: 'main', files: [] })
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.status.phase, 'ready')
  assert.equal(snap.status.data.branch, 'main')
  assert.equal(snap.history.phase, 'cancelled')
  f.model.dispose()
})

test('Git late history from an abandoned repository cannot overwrite the new one', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  const stale = f.handles[1]
  f.model.setTarget('C:/Two')
  await tick()
  assert.equal(stale.cancellations, 1)
  stale.finish(historyResult([commit('old')]))
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.history.data, null)
  assert.equal(snap.target, 'C:/Two')
  assert.equal(f.calls[f.calls.length - 1].args.target_library, 'C:/Two')
  f.model.dispose()
})

test('Git switching commits drops the old detail before old file buttons dispatch (N2)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectCommit('commit-A')
  await tick()
  f.handles.at(-1).finish(detailResult('commit-A', [], [{ path: 'only-A.kicad_sym', status: 'M' }]))
  await tick()
  f.model.selectCommit('commit-B')
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.selectedOid, 'commit-B')
  assert.equal(snap.detail.data, null)
  assert.equal(snap.detail.phase, 'loading')
  const before = f.calls.filter((call) => call.action === 'commit_file').length
  // A stale component could still invoke either the legacy string form or an
  // explicit A identity; both must be rejected by the model.
  f.model.openFile('only-A.kicad_sym')
  f.model.openFile({ oid: 'commit-A', path: 'only-A.kicad_sym', parentIndex: 0 })
  await tick()
  assert.equal(f.calls.filter((call) => call.action === 'commit_file').length, before)
  assert.equal(f.model.getSnapshot().file, null)
  f.handles.at(-1).finish(detailResult('commit-B', [], []))
  await tick()
  assert.equal(f.model.getSnapshot().detail.data.oid, 'commit-B')
  f.model.dispose()
})

test('Git file patches are keyed by commit identity and late old patches are ignored (N2)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectCommit('commit-A')
  await tick()
  f.handles.at(-1).finish(detailResult('commit-A', [], [{ path: 'same.kicad_sym', status: 'M' }]))
  await tick()
  f.model.openFile({ oid: 'commit-A', path: 'same.kicad_sym', parentIndex: 0 })
  await tick()
  const oldFile = f.handles.at(-1)
  assert.equal(f.calls.at(-1).action, 'commit_file')
  assert.equal(f.calls.at(-1).args.oid, 'commit-A')

  f.model.selectCommit('commit-B')
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.detail.data, null)
  assert.equal(snap.file, null)
  assert.equal(snap.fileDiff.phase, 'idle')
  oldFile.finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'same.kicad_sym',
    patch: '- base\n+A\n', truncated: false, binary: false,
  })
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.data, null)

  f.handles.at(-1).finish(detailResult('commit-B', [], [{ path: 'same.kicad_sym', status: 'M' }]))
  await tick()
  f.model.openFile({ oid: 'commit-B', path: 'same.kicad_sym', parentIndex: 0 })
  await tick()
  const call = f.calls.at(-1)
  assert.equal(call.action, 'commit_file')
  assert.equal(call.args.oid, 'commit-B')
  f.handles.at(-1).finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-B', path: 'same.kicad_sym',
    patch: '- base\n+B\n', truncated: false, binary: false,
  })
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'ready')
  assert.equal(snap.fileDiff.data.oid, 'commit-B')
  assert.equal(snap.fileDiff.data.target, 'C:/One')
  assert.match(snap.fileDiff.data.patch, /\+B/)
  f.model.dispose()
})

test('Git detail and file failures stay bound to their identity and can be retried (N2)', async () => {
  const calls = [], handles = []
  let failDetail = true, failFile = true
  const model = createGitModel({
    createStore: createSnapshotStore,
    librariesApi: {
      async componentLibraries() {
        return [{ id: 'one', name: 'One', symbolPath: 'C:/One/symbols', footprintPath: 'C:/One/footprints' }]
      },
    },
    tasks: {
      async run(action, args) {
        calls.push({ action, args })
        if (action === 'commit_detail' && failDetail) {
          failDetail = false
          throw new Error('DETAIL_DOWN')
        }
        if (action === 'commit_file' && failFile) {
          failFile = false
          throw new Error('FILE_DOWN')
        }
        const h = handle(action)
        handles.push(h)
        return h
      },
    },
  })
  await model.start()
  await tick()
  handles[0].finish()
  await tick()
  model.selectCommit('commit-A')
  await tick()
  let snap = model.getSnapshot()
  assert.equal(snap.detail.phase, 'error')
  assert.match(snap.detail.error, /DETAIL_DOWN/)
  assert.equal(snap.detail.data, null)

  model.retryDetail()
  await tick()
  handles.at(-1).finish(detailResult('commit-A', [], [{ path: 'A.kicad_sym', status: 'M' }]))
  await tick()
  snap = model.getSnapshot()
  assert.equal(snap.detail.phase, 'ready')

  model.openFile({ oid: 'commit-A', path: 'A.kicad_sym', parentIndex: 0 })
  await tick()
  snap = model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'error')
  assert.match(snap.fileDiff.error, /FILE_DOWN/)
  assert.equal(snap.fileDiff.data, null)

  model.retryFile()
  await tick()
  handles.at(-1).finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ retry\n', truncated: false, binary: false,
  })
  await tick()
  snap = model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'ready')
  assert.equal(snap.fileDiff.data.oid, 'commit-A')
  model.dispose()
})

test('Git retires implicit current-branch history when status reports a new HEAD (N3)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('main-tip')]))
  await tick()
  f.model.setHistoryRange('branch')
  await tick()
  const pending = f.handles.at(-1)
  const before = f.calls.filter((call) => call.action === 'history').length

  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'feature', files: [] })
  await tick()
  const historyCalls = f.calls.filter((call) => call.action === 'history')
  assert.equal(historyCalls.length, before + 1)
  assert.equal(historyCalls.at(-1).args.range, 'branch')
  assert.equal(historyCalls.at(-1).args.branch_ref, undefined)
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'loading')
  assert.equal(snap.history.data, null)
  assert.equal(snap.running, true)

  // The old read is no longer subscribed and must not settle the new channel.
  pending.finish(historyResult([commit('main-tip')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'loading')
  assert.equal(snap.history.data, null)

  f.handles.at(-1).finish(historyResult([commit('feature-tip')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'feature-tip')
  f.model.dispose()
})

test('Git explicit branch history is not retired by a current-HEAD change (N3)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('main-tip')]))
  await tick()
  f.model.setHistoryRange('branch', 'release')
  await tick()
  const pending = f.handles.at(-1)
  const calls = f.calls.filter((call) => call.action === 'history').length

  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'feature', last_commit: 'new-head', files: [] })
  await tick()
  assert.equal(f.calls.filter((call) => call.action === 'history').length, calls)
  assert.equal(pending.cancellations, 0)
  assert.equal(f.model.getSnapshot().history.phase, 'loading')

  pending.finish(historyResult([commit('release-tip')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.phase, 'ready')
  assert.equal(f.model.getSnapshot().history.data.commits[0].oid, 'release-tip')
  f.model.dispose()
})

test('Git current-branch history survives unborn, detached and A -> B -> A HEAD changes (N3)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('all-tip')]))
  await tick()
  f.model.setHistoryRange('branch')
  await tick()
  const first = f.handles.at(-1)

  // unborn (no last_commit) -> first commit, same branch name
  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'main', last_commit: 'c1', files: [] })
  await tick()
  const second = f.handles.at(-1)

  // feature -> detached -> main, each with a new HEAD identity
  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'feature', last_commit: 'c2', files: [] })
  await tick()
  const third = f.handles.at(-1)
  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: '', last_commit: 'c3', detached: true, files: [] })
  await tick()
  const fourth = f.handles.at(-1)
  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'main', last_commit: 'c4', files: [] })
  await tick()
  const fifth = f.handles.at(-1)

  assert.equal(f.calls.filter((call) => call.action === 'history').length, 6)
  for (const stale of [first, second, third, fourth]) {
    stale.finish(historyResult([commit('stale')]))
  }
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'loading')
  assert.equal(snap.history.data, null)

  fifth.finish(historyResult([commit('main-again')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'main-again')
  assert.equal(snap.status.data.branch, 'main')
  f.model.dispose()
})

test('Git cancelling detail and file reads cannot publish their late identities (N2)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectCommit('commit-A')
  await tick()
  const detail = f.handles.at(-1)
  await f.model.cancel()
  assert.equal(detail.cancellations, 1)
  assert.equal(f.model.getSnapshot().detail.phase, 'cancelled')
  assert.equal(f.model.getSnapshot().detail.data, null)
  assert.equal(f.model.getSnapshot().selectedOid, 'commit-A')
  detail.finish(detailResult('commit-A', [], [{ path: 'A.kicad_sym', status: 'M' }]))
  await tick()
  assert.equal(f.model.getSnapshot().detail.data, null)

  f.model.selectCommit('commit-A')
  await tick()
  f.handles.at(-1).finish(detailResult('commit-A', [], [{ path: 'A.kicad_sym', status: 'M' }]))
  await tick()
  f.model.openFile({ oid: 'commit-A', path: 'A.kicad_sym', parentIndex: 0 })
  await tick()
  const file = f.handles.at(-1)
  await f.model.cancel()
  assert.equal(file.cancellations, 1)
  assert.equal(f.model.getSnapshot().fileDiff.phase, 'cancelled')
  assert.equal(f.model.getSnapshot().fileDiff.data, null)
  assert.equal(f.model.getSnapshot().file.path, 'A.kicad_sym')
  assert.equal(f.model.getSnapshot().file.parentIndex, 0)
  file.finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ cancelled\n', truncated: false, binary: false,
  })
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.data, null)

  f.model.retryFile()
  await tick()
  f.handles.at(-1).finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ retried\n', truncated: false, binary: false,
  })
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.phase, 'ready')
  f.model.dispose()
})

test('Git cancellation arriving after a successful read leaves the result published', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  const read = f.handles.at(-1)
  read.finish(historyResult([commit('settled')]))
  await f.model.cancel()
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'settled')
  assert.equal(snap.running, false)
  assert.equal(read.cancellations, 0)
  f.model.dispose()
})

test('Git cancelled file refreshes keep the previous result and retry in place', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectCommit('commit-A')
  await tick()
  f.handles.at(-1).finish(detailResult('commit-A', [], [{ path: 'A.kicad_sym', status: 'M' }]))
  await tick()
  assert.equal(f.model.getSnapshot().detail.phase, 'ready')
  f.model.openFile({ oid: 'commit-A', path: 'A.kicad_sym', parentIndex: 0 })
  await tick()
  f.handles.at(-1).finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ old file\n', truncated: false, binary: false,
  })
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.data.patch, '+ old file\n')

  f.model.retryFile()
  await tick()
  const fileRefresh = f.handles.at(-1)
  await f.model.cancel()
  let snap = f.model.getSnapshot()
  assert.equal(fileRefresh.cancellations, 1)
  assert.equal(snap.fileDiff.phase, 'cancelled')
  assert.equal(snap.fileDiff.data.patch, '+ old file\n')
  assert.equal(snap.file.path, 'A.kicad_sym')

  fileRefresh.finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ late file\n', truncated: false, binary: false,
  })
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.fileDiff.phase, 'cancelled')
  assert.equal(snap.fileDiff.data.patch, '+ old file\n')

  f.model.retryFile()
  await tick()
  f.handles.at(-1).finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'commit-A', path: 'A.kicad_sym',
    patch: '+ retried file\n', truncated: false, binary: false,
  })
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.phase, 'ready')
  assert.equal(f.model.getSnapshot().fileDiff.data.patch, '+ retried file\n')
  f.model.dispose()
})

test('Git cancelled history refreshes keep the previous page and expose retry', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('first')]))
  await tick()
  f.model.refreshHistory()
  await tick()
  const refresh = f.handles.at(-1)
  await f.model.cancel()
  let snap = f.model.getSnapshot()
  assert.equal(refresh.cancellations, 1)
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(snap.history.data.commits[0].oid, 'first')

  refresh.finish(historyResult([commit('late')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(snap.history.data.commits[0].oid, 'first')

  f.model.refreshHistory()
  await tick()
  f.handles.at(-1).finish(historyResult([commit('retried')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'retried')
  f.model.dispose()
})

for (const method of ['cancelChannel', 'cancel']) {
  test(`Git ${method} sends cancellation before the first authoritative task snapshot`, async (t) => {
    const requests = []
    const pendingReads = []
    const tasks = createTasksApi('com.kicad_forge.git_version', async (url, options = {}) => {
      requests.push({ url, method: options.method })
      const cancelled = { task_id: 'history-1', action: 'history', state: 'cancelled', cancelable: false }
      let body
      if (url === '/api/tasks') {
        const { action } = JSON.parse(options.body)
        // Native submit only returns identity and state, without cancelable.
        body = action === 'history'
          ? { ok: true, task_id: 'history-1', state: 'queued' }
          : { task_id: 'status-1', action, state: 'succeeded', result: { ok: true, initialized: true, branch: 'main', files: [] } }
      } else if (url.endsWith('/cancel')) {
        body = { ...cancelled, status: 'cancel_requested' }
        for (const resolve of pendingReads) resolve(cancelled)
      } else {
        // Hold the first poll so cancellation always precedes capability discovery.
        body = await new Promise((resolve) => pendingReads.push(resolve))
      }
      return { ok: true, status: 200, json: async () => body }
    })
    const model = createGitModel({
      createStore: createSnapshotStore,
      librariesApi: { async componentLibraries() { return [{ id: 'one', name: 'One', symbolPath: 'C:/One/symbols' }] } },
      tasks,
    })
    t.after(() => { model.dispose(); tasks.dispose() })
    await model.start()
    await tick()
    model.selectTab('history')
    await tick()
    assert.equal(model.getSnapshot().history.phase, 'loading')
    if (method === 'cancelChannel') await model.cancelChannel('history')
    else await model.cancel()
    assert.equal(model.getSnapshot().history.phase, 'cancelled')
    assert.equal(requests.filter(({ url, method }) => url === '/api/tasks/history-1/cancel' && method === 'POST').length, 1)
  })
}

test('Git switching repositories during a pending cancel receipt keeps the new repository read', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('first')]))
  await tick()
  f.model.refreshHistory()
  await tick()
  const oldRead = f.handles.at(-1)
  let releaseCancel
  const receipt = new Promise((resolve) => { releaseCancel = resolve })
  oldRead.cancel = () => receipt
  const cancellation = f.model.cancel()
  assert.equal(f.model.getSnapshot().history.phase, 'cancelled')

  f.model.setTarget('C:/Two')
  await tick()
  assert.equal(f.model.getSnapshot().target, 'C:/Two')
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'two', files: [] })
  await tick()
  const newHistory = f.handles.at(-1)
  assert.equal(f.calls.at(-1).action, 'history')
  newHistory.finish(historyResult([commit('two')]))
  await tick()

  releaseCancel({ status: 'cancel_requested' })
  await cancellation
  oldRead.finish(historyResult([commit('old-late')]))
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.target, 'C:/Two')
  assert.equal(snap.status.data.branch, 'two')
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'two')
  f.model.dispose()
})

test('Git cancel receipts cannot overwrite reads started while cancellation is in flight', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('first')]))
  await tick()

  f.model.refreshHistory()
  await tick()
  const oldHistory = f.handles.at(-1)
  let releaseHistory
  const receipt = new Promise((resolve) => { releaseHistory = resolve })
  oldHistory.cancel = () => receipt
  const cancellation = f.model.cancel()
  assert.equal(f.model.getSnapshot().history.phase, 'cancelled')

  f.model.refreshHistory()
  await tick()
  const freshHistory = f.handles.at(-1)
  freshHistory.finish(historyResult([commit('fresh')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.phase, 'ready')
  assert.equal(f.model.getSnapshot().history.data.commits[0].oid, 'fresh')

  releaseHistory({ status: 'cancel_requested' })
  await cancellation
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'fresh')
  assert.equal(snap.selectedOid, null)
  f.model.dispose()
})

test('Git cancel receipts do not overwrite a commit selected during cancellation', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectCommit('commit-A')
  await tick()
  const oldDetail = f.handles.at(-1)
  let releaseDetail
  const receipt = new Promise((resolve) => { releaseDetail = resolve })
  oldDetail.cancel = () => receipt
  const cancellation = f.model.cancel()
  assert.equal(f.model.getSnapshot().detail.phase, 'cancelled')

  f.model.selectCommit('commit-B')
  await tick()
  assert.equal(f.model.getSnapshot().detail.phase, 'loading')
  f.handles.at(-1).finish(detailResult('commit-B', [], []))
  await tick()
  assert.equal(f.model.getSnapshot().detail.data.oid, 'commit-B')

  releaseDetail({ status: 'cancel_requested' })
  await cancellation
  await tick()
  const snap = f.model.getSnapshot()
  assert.equal(snap.selectedOid, 'commit-B')
  assert.equal(snap.detail.phase, 'ready')
  assert.equal(snap.detail.data.oid, 'commit-B')
  f.model.dispose()
})

test('Git cancelling a head-changed history read settles as cancelled without late data (N3)', async () => {
  const f = fixture()
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('all-tip')]))
  await tick()
  f.model.setHistoryRange('branch')
  await tick()
  const oldRead = f.handles.at(-1)

  f.model.runStatus()
  await tick()
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'feature', files: [] })
  await tick()
  const newRead = f.handles.at(-1)
  await f.model.cancel()
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(snap.history.data, null)
  assert.equal(snap.running, false)
  assert.equal(snap.canCancel, false)
  assert.equal(newRead.cancellations, 1)

  oldRead.finish(historyResult([commit('stale')]))
  newRead.finish(historyResult([commit('stale-new')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'cancelled')
  assert.equal(snap.history.data, null)
  f.model.dispose()
})

test('Git history creation failure after a HEAD change settles in error without loading (N3)', async () => {
  let failNext = false
  const f = fixture({ failHistory: () => failNext })
  await ready(f)
  f.model.selectTab('history')
  await tick()
  f.handles.at(-1).finish(historyResult([commit('all-tip')]))
  await tick()
  f.model.setHistoryRange('branch')
  await tick()
  const oldRead = f.handles.at(-1)

  f.model.runStatus()
  await tick()
  failNext = true
  f.handles.at(-1).finish({ ok: true, initialized: true, branch: 'feature', files: [] })
  await tick()
  let snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'error')
  assert.match(snap.history.error, /HISTORY_DOWN/)
  assert.equal(snap.history.data, null)
  assert.equal(snap.running, false)

  oldRead.finish(historyResult([commit('stale')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'error')
  assert.equal(snap.history.data, null)

  failNext = false
  f.model.refreshHistory()
  await tick()
  f.handles.at(-1).finish(historyResult([commit('recovered')]))
  await tick()
  snap = f.model.getSnapshot()
  assert.equal(snap.history.phase, 'ready')
  assert.equal(snap.history.data.commits[0].oid, 'recovered')
  f.model.dispose()
})
