import test from 'node:test'
import assert from 'node:assert/strict'
import {
  describeEditRefresh, selectableCommitIds, EDIT_REFRESH_SOURCES,
  nextEditRefreshAttempt, planEditRefreshRetry, runEditRefreshFlow,
} from '../../src/lib/editRefresh.js'

const published = (data) => ({ status: 'fulfilled', value: { status: 'published', data } })
const failed = (message) => ({ status: 'fulfilled', value: { status: 'failed', error: message } })
const superseded = () => ({ status: 'fulfilled', value: { status: 'superseded' } })

test('edit refresh reports success only when every source published', () => {
  const report = describeEditRefresh([
    published([]), published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'published')
  assert.deepEqual(report.failedSources, [])
  assert.equal(report.error, '')
  assert.equal(report.details.length, EDIT_REFRESH_SOURCES.length)
})

test('edit refresh keeps failed sources and messages for the retry notice', () => {
  const report = describeEditRefresh([
    published([{ id: 's1', library_id: 'lib-a' }]),
    failed('HTTP 503'),
    published({}),
    failed('history unavailable'),
  ])
  assert.equal(report.status, 'failed')
  assert.deepEqual(report.failedSources, ['libraries', 'history'])
  assert.match(report.error, /HTTP 503/)
  assert.match(report.error, /history unavailable/)
})

test('edit refresh treats superseded reads as not failed', () => {
  const report = describeEditRefresh([
    superseded(), published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'superseded')
  assert.deepEqual(report.failedSources, [])
  assert.deepEqual(report.supersededSources, ['symbols'])
})

test('edit refresh treats a rejected read as a failed source', () => {
  const report = describeEditRefresh([
    { status: 'rejected', reason: Object.assign(new Error('network down'), { kind: 'network' }) },
    published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'failed')
  assert.deepEqual(report.failedSources, ['symbols'])
  assert.match(report.error, /network down/)
})

test('selectableCommitIds keeps only UUIDs published in the target library', () => {
  const rows = [
    { id: 'a', library_id: 'lib-a' },
    { id: 'b', library_id: 'lib-b' },
    { id: 'c', library_id: 'lib-a' },
  ]
  assert.deepEqual(selectableCommitIds(rows, ['a', 'b', 'missing', 'a'], 'lib-a'), {
    selected: ['a'], missing: ['b', 'missing'],
  })
  assert.deepEqual(selectableCommitIds(rows, ['a', 'b'], ''), {
    selected: ['a', 'b'], missing: [],
  })
})

test('selectableCommitIds does not trust malformed rows or ids', () => {
  const rows = [null, { id: '' }, { id: 7 }, { id: 'ok', library_id: 'lib-a' }]
  assert.deepEqual(selectableCommitIds(rows, ['ok', '', null, 'ok'], 'lib-a'), {
    selected: ['ok'], missing: [],
  })
  assert.deepEqual(selectableCommitIds(null, ['a']), { selected: [], missing: ['a'] })
})

// ---- N1: refresh ownership vs. later user intent -------------------------
//
// These tests run the same runEditRefreshFlow() the App uses. They model the
// delayed read-only probe from the delivery review: the write (paste/undo) has
// already committed, the readback probe is still pending, the user searches for
// Gamma, and then the old probe is released. The flow must never publish the
// old list/query, clear the new search or select an old object.

function deferred() {
  let resolve, reject
  const promise = new Promise((res, rej) => { resolve = res; reject = rej })
  return { promise, resolve, reject }
}

async function until(check, message) {
  for (let i = 0; i < 40; i += 1) {
    if (check()) return
    await Promise.resolve()
  }
  assert.fail(message)
}

function createHarness({ symbols = [], query = { search: '', targetLib: 'lib-a' },
  workspaceId = 'ws-a', route = 'kf:symbols' } = {}) {
  const state = {
    symbols: symbols.slice(),
    query: { ...query },
    workspaceId,
    route,
    intentEpoch: 0,
    noticeEpoch: 0,
    selectionEpoch: 0,
    notice: null,
    selection: null,
    toasts: [],
    appliedQuery: [],
    probes: [],
    publishes: [],
  }
  const gates = []
  const context = () => ({
    attempt: state.notice?.attempt ?? 0,
    intentEpoch: state.intentEpoch,
    workspaceId: state.workspaceId,
    route: state.route,
    noticeEpoch: state.noticeEpoch,
    selectionEpoch: state.selectionEpoch,
  })
  const reads = {
    probe: (search, targetLib) => {
      const gate = deferred()
      gates.push(gate)
      state.probes.push({ search, targetLib })
      return gate.promise
    },
    libraries: async () => ({ status: 'published', data: [] }),
    status: async () => ({ status: 'published', data: {} }),
    history: async () => ({ status: 'published' }),
    publish: async (search, targetLib) => {
      state.publishes.push({ search, targetLib })
      return { status: 'published', data: state.symbols.slice() }
    },
  }
  const present = {
    pending: notice => { state.notice = notice },
    failed: notice => { state.notice = notice },
    clear: attempt => { if (state.notice?.attempt === attempt) state.notice = null },
    applyQuery: targetLibraryId => {
      state.appliedQuery.push(targetLibraryId)
      state.query = { search: '', targetLib: targetLibraryId }
    },
    select: ids => { state.selection = ids.slice() },
  }
  const start = (request, attempt) => runEditRefreshFlow({
    request, attempt,
    readContext: context,
    getNoticeAttempt: () => state.notice?.attempt ?? null,
    reads, present,
    toast: message => state.toasts.push(message),
    translate: (key, params) => (params ? key + ':' + JSON.stringify(params) : key),
  })
  const userSearch = (value, nextSymbols) => {
    state.intentEpoch += 1
    state.query.search = value
    if (nextSymbols) state.symbols = nextSymbols.slice()
  }
  const userWorkspace = id => { state.intentEpoch += 1; state.workspaceId = id }
  const userLibrary = (id, nextSymbols) => {
    state.intentEpoch += 1
    state.query.targetLib = id
    if (nextSymbols) state.symbols = nextSymbols.slice()
  }
  const userRoute = route => { state.intentEpoch += 1; state.route = route }
  const userDismiss = () => { state.noticeEpoch += 1; state.notice = null }
  const userSelection = ids => { state.selectionEpoch += 1; state.selection = ids.slice() }
  return {
    state, gates, context, reads, present, start,
    userSearch, userWorkspace, userLibrary, userRoute, userDismiss, userSelection,
  }
}

const historyRequest = () => ({
  kind: 'history', ids: [], query: { search: '', targetLib: 'lib-a' },
  targetLibraryId: '', locate: false, requireAll: false, successKey: null,
})

test('N1: a delayed post-commit probe cannot replace a later search', async () => {
  const h = createHarness({
    symbols: [{ id: 'alpha' }, { id: 'beta' }, { id: 'delta' }],
  })
  const flow = h.start(historyRequest(), 1)
  await until(() => h.gates.length === 1, 'probe should start')

  h.userSearch('Gamma', [{ id: 'gamma' }])
  h.gates[0].resolve([{ id: 'alpha' }, { id: 'beta' }, { id: 'delta' }, { id: 'gamma' }])
  const report = await flow

  assert.equal(report.status, 'superseded')
  assert.deepEqual(h.state.symbols.map(row => row.id), ['gamma'])
  assert.equal(h.state.query.search, 'Gamma')
  assert.equal(h.state.publishes.length, 0, 'old query must not be published')
  assert.equal(h.state.selection, null)
  assert.deepEqual(h.state.toasts, [])
  assert.equal(h.state.notice, null)
})

test('N1: switching workspace supersedes an in-flight refresh', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  const flow = h.start(historyRequest(), 1)
  await until(() => h.gates.length === 1, 'probe should start')

  h.userWorkspace('ws-b')
  h.gates[0].resolve([{ id: 'alpha' }])
  const report = await flow

  assert.equal(report.status, 'superseded')
  assert.equal(h.state.publishes.length, 0)
  assert.equal(h.state.notice, null)
  assert.deepEqual(h.state.toasts, [])
})

test('N1: switching target library supersedes an in-flight refresh', async () => {
  const h = createHarness({
    symbols: [{ id: 'alpha', library_id: 'lib-a' }],
    query: { search: 'Alpha', targetLib: 'lib-a' },
  })
  const flow = h.start({
    kind: 'paste', ids: ['alpha'], query: { search: 'Alpha', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  }, 1)
  await until(() => h.gates.length === 1, 'probe should start')

  h.userLibrary('lib-b', [{ id: 'beta', library_id: 'lib-b' }])
  h.gates[0].resolve([{ id: 'alpha', library_id: 'lib-a' }])
  const report = await flow

  assert.equal(report.status, 'superseded')
  assert.equal(h.state.publishes.length, 0)
  assert.equal(h.state.query.search, 'Alpha')
  assert.equal(h.state.query.targetLib, 'lib-b')
  assert.deepEqual(h.state.symbols.map(row => row.id), ['beta'])
  assert.deepEqual(h.state.appliedQuery, [])
  assert.equal(h.state.notice, null)
  assert.deepEqual(h.state.toasts, [])
})

test('N1: switching center page supersedes an in-flight refresh', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  const flow = h.start(historyRequest(), 1)
  await until(() => h.gates.length === 1, 'probe should start')

  h.userRoute('kf:settings')
  h.gates[0].resolve([{ id: 'alpha' }])
  const report = await flow

  assert.equal(report.status, 'superseded')
  assert.equal(h.state.publishes.length, 0)
  assert.equal(h.state.notice, null)
})

test('N1: dismissing the notice cancels the in-flight refresh', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  const flow = h.start({
    kind: 'paste', ids: ['alpha'], query: { search: '', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  }, 1)
  await until(() => h.gates.length === 1, 'probe should start')

  h.userDismiss()
  h.gates[0].resolve([{ id: 'alpha' }])
  const report = await flow

  assert.equal(report.status, 'superseded')
  assert.equal(h.state.publishes.length, 0)
  assert.equal(h.state.notice, null)
  assert.deepEqual(h.state.toasts, [])
  assert.deepEqual(h.state.appliedQuery, [])
  assert.equal(h.state.selection, null)
})

test('N1: a newer refresh owns the notice; the older attempt cannot clear or publish', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  const first = h.start(historyRequest(), 1)
  await until(() => h.gates.length === 1, 'first probe should start')

  const second = h.start({
    kind: 'paste', ids: ['gamma'], query: { search: 'Gamma', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  }, 2)
  await until(() => h.gates.length === 2, 'second probe should start')
  h.state.symbols = [{ id: 'gamma', library_id: 'lib-a' }]
  h.gates[1].resolve([{ id: 'gamma', library_id: 'lib-a' }])
  await second
  assert.equal(h.state.notice, null)
  assert.deepEqual(h.state.selection, ['gamma'])
  assert.deepEqual(h.state.appliedQuery, ['lib-a'])

  h.gates[0].resolve([{ id: 'alpha' }])
  const firstReport = await first
  assert.equal(firstReport.status, 'superseded')
  assert.equal(h.state.publishes.length, 1)
  assert.equal(h.state.publishes[0].search, 'Gamma')
  assert.deepEqual(h.state.selection, ['gamma'])
})

test('N1: an older attempt cannot clear a newer failure notice', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  const first = h.start(historyRequest(), 1)
  await until(() => h.gates.length === 1, 'first probe should start')

  const second = h.start(historyRequest(), 2)
  await until(() => h.gates.length === 2, 'second probe should start')
  h.gates[1].reject(new Error('HTTP 503'))
  await second
  assert.equal(h.state.notice.attempt, 2)
  assert.equal(h.state.notice.status, 'failed')

  h.gates[0].resolve([{ id: 'alpha' }])
  await first
  assert.equal(h.state.notice.attempt, 2)
  assert.equal(h.state.notice.status, 'failed')
})

test('N1: retry after a failed refresh re-reads the current query instead of hijacking it', async () => {
  const h = createHarness({ symbols: [] })
  const failed = h.start({
    kind: 'paste', ids: ['new'], query: { search: '', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  }, 1)
  await until(() => h.gates.length === 1, 'probe should start')
  h.gates[0].reject(new Error('HTTP 503'))
  await failed
  assert.equal(h.state.notice.status, 'failed')

  h.userSearch('Gamma', [{ id: 'gamma' }])
  const plan = planEditRefreshRetry(h.state.notice, {
    query: { ...h.state.query }, context: h.context(),
  })
  assert.equal(plan.reused, false)
  const retry = h.start(plan.request, h.state.notice.attempt + 1)
  await until(() => h.gates.length === 2, 'retry probe should start')
  h.gates[1].resolve([{ id: 'gamma' }])
  const report = await retry

  assert.equal(report.status, 'published')
  assert.deepEqual(h.state.probes[1], { search: 'Gamma', targetLib: 'lib-a' })
  assert.deepEqual(h.state.publishes.map(item => item.search), ['Gamma'])
  assert.deepEqual(h.state.appliedQuery, [], 'retry must not switch back to the old target library')
  assert.equal(h.state.selection, null)
  assert.ok(!h.state.toasts.some(message => String(message).includes('pasteSuccess')))
  assert.equal(h.state.notice, null)
})

test('N1: retry with unchanged intent still locates and selects the committed object', async () => {
  const h = createHarness({ symbols: [] })
  const failed = h.start({
    kind: 'paste', ids: ['new'], query: { search: '', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  }, 1)
  await until(() => h.gates.length === 1, 'probe should start')
  h.gates[0].reject(new Error('HTTP 503'))
  await failed

  h.state.symbols = [{ id: 'new', library_id: 'lib-a' }]
  const plan = planEditRefreshRetry(h.state.notice, {
    query: { ...h.state.query }, context: h.context(),
  })
  assert.equal(plan.reused, true)
  const retry = h.start(plan.request, 2)
  await until(() => h.gates.length === 2, 'retry probe should start')
  h.gates[1].resolve(h.state.symbols)
  await retry

  assert.deepEqual(h.state.appliedQuery, ['lib-a'])
  assert.deepEqual(h.state.selection, ['new'])
  assert.ok(h.state.toasts.some(message => String(message).includes('topbar.pasteSuccess')))
  assert.equal(h.state.notice, null)
})

test('M1: a cleared notice does not let a later refresh reuse an older attempt id', async () => {
  const h = createHarness({ symbols: [{ id: 'alpha' }] })
  let sequence = 0
  const start = (request) => {
    sequence = nextEditRefreshAttempt(sequence)
    return h.start(request, sequence)
  }

  const old = start(historyRequest())
  await until(() => h.gates.length === 1, 'old probe should start')
  const middle = start(historyRequest())
  await until(() => h.gates.length === 2, 'middle probe should start')
  h.gates[1].resolve([{ id: 'alpha' }])
  await middle
  assert.equal(h.state.notice, null, 'the completed middle refresh clears its notice')

  const newest = start({
    kind: 'paste', ids: ['gamma'], query: { search: 'Gamma', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: true, requireAll: true, successKey: 'topbar.pasteSuccess',
  })
  await until(() => h.gates.length === 3, 'newest probe should start')
  assert.deepEqual([old, middle, newest].map(flow => flow instanceof Promise), [true, true, true])
  h.state.symbols = [{ id: 'gamma', library_id: 'lib-a' }]
  const publishesBeforeOld = h.state.publishes.length

  h.gates[0].resolve([{ id: 'alpha' }])
  const oldReport = await old
  assert.equal(oldReport.status, 'superseded', 'old A must stay superseded after B clears and C starts')
  assert.equal(h.state.notice?.attempt, 3, 'old A must not clear the newest notice')
  assert.equal(h.state.publishes.length, publishesBeforeOld, 'old A must not publish its list')

  h.gates[2].resolve([{ id: 'gamma', library_id: 'lib-a' }])
  await newest
  assert.deepEqual(h.state.selection, ['gamma'], 'the newest refresh still owns its selection')
  assert.deepEqual(h.state.appliedQuery, ['lib-a'])
  assert.ok(h.state.toasts.some(message => String(message).includes('topbar.pasteSuccess')))
  assert.equal(h.state.notice, null)
})

test('M1: the attempt allocator never reuses an id after a notice is cleared', () => {
  let sequence = 0
  sequence = nextEditRefreshAttempt(sequence)
  sequence = nextEditRefreshAttempt(sequence)
  sequence = nextEditRefreshAttempt(sequence)
  assert.equal(sequence, 3)
  assert.equal(nextEditRefreshAttempt(undefined), 1)
  assert.equal(nextEditRefreshAttempt(-4), 1)
  assert.equal(nextEditRefreshAttempt(Number.NaN), 1)
})

test('N1: a refresh does not replace a newer user selection', async () => {
  const h = createHarness({
    symbols: [{ id: 'a', library_id: 'lib-a' }],
  })
  const flow = h.start({
    kind: 'paste', ids: ['a'], query: { search: '', targetLib: 'lib-a' },
    targetLibraryId: 'lib-a', locate: false, requireAll: false, successKey: null,
  }, 1)
  await until(() => h.gates.length === 1, 'probe should start')
  h.userSelection(['user-picked'])
  h.gates[0].resolve([{ id: 'a', library_id: 'lib-a' }])
  await flow

  assert.deepEqual(h.state.selection, ['user-picked'])
  assert.equal(h.state.publishes.length, 1)
  assert.equal(h.state.notice, null)
})
