import test from 'node:test'
import assert from 'node:assert/strict'
import { createSnapshotStore } from '@kiforge/plugin-sdk'
import { createGitModel, componentBase, HISTORY_MAX_COMMITS } from '../../plugins/git_version/frontend/src/model.ts'

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

function fixture() {
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
  assert.match(snap.fileDiff.data.patch, /\+ new/)

  f.model.setParentIndex(1)
  await tick()
  const parentCall = [...f.calls].reverse().find((call) => call.action === 'commit_file')
  assert.equal(parentCall.args.parent_index, 1)
  assert.equal(parentCall.args.parent, 'p2')
  const parentDetail = f.calls[f.calls.length - 1]
  assert.equal(parentDetail.action, 'commit_detail')
  assert.equal(parentDetail.args.parent_index, 1)
  f.handles[4].finish({
    ok: true, schema_version: 'commit_file_v1', oid: 'merge', path: 'Power.kicad_sym',
    patch: '', truncated: false, binary: true,
  })
  f.handles[5].finish(detailResult('merge', ['p1', 'p2'], []))
  await tick()
  assert.equal(f.model.getSnapshot().fileDiff.phase, 'empty')

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
  f.model.selectCommit('merge')
  await tick()
  assert.equal(f.calls.length, calls + 1)
  assert.equal(f.model.getSnapshot().detail.data.commit.oid, 'merge')
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
  assert.equal(snap.history.phase, 'idle')
  assert.equal(f.handles[1].cancellations, 1)
  f.handles[1].finish(historyResult([commit('late')]))
  await tick()
  assert.equal(f.model.getSnapshot().history.data, null)
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
