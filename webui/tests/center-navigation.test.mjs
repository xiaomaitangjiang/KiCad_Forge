import test from 'node:test'
import assert from 'node:assert/strict'
import {
  CENTER_HOME, CENTER_SETTINGS, MAX_CENTER_GROUPS, MAX_CENTER_HISTORY, MAX_CENTER_TABS, centerNavigationReducer,
  createCenterNavigation, pageViewId, parseCenterState, serializeCenterState, viewPageKey,
} from '../src/workbench/useCenterNavigation.js'

const select = (state, key) => centerNavigationReducer(state, { type: 'select', key })
const close = (state, key) => centerNavigationReducer(state, { type: 'close', key })
const back = (state) => centerNavigationReducer(state, { type: 'back' })
const prune = (state, viewIds) => centerNavigationReducer(state, { type: 'prune', viewIds })
const split = (state, direction = 'row') => centerNavigationReducer(state, { type: 'split', direction })
const move = (state, key, groupId) => centerNavigationReducer(state, { type: 'move-page', key, groupId })
const activate = (state, groupId) => centerNavigationReducer(state, { type: 'activate-group', groupId })

test('view page keys round-trip without colliding with home', () => {
  assert.equal(viewPageKey('demo.supply'), 'view:demo.supply')
  assert.equal(pageViewId('view:demo.supply'), 'demo.supply')
  assert.equal(viewPageKey(null), CENTER_HOME)
  assert.equal(pageViewId(CENTER_HOME), null)
})

test('select opens one tab per page and records history', () => {
  let state = createCenterNavigation()
  state = select(state, viewPageKey('a'))
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:a'])
  assert.equal(state.active, 'view:a')
  assert.equal(state.index, 1)
  state = select(state, viewPageKey('b'))
  state = select(state, viewPageKey('a'))
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:a', 'view:b'])
  assert.equal(state.index, 3)
  assert.deepEqual(state.history, [CENTER_HOME, 'view:a', 'view:b', 'view:a'])
})

test('selecting the active page does not add history', () => {
  let state = select(createCenterNavigation(), viewPageKey('a'))
  const before = state
  state = select(state, viewPageKey('a'))
  assert.equal(state, before)
})

test('back walks history and stops at the first page', () => {
  let state = select(createCenterNavigation(), viewPageKey('a'))
  state = select(state, viewPageKey('b'))
  state = back(state)
  assert.equal(state.active, 'view:a')
  state = back(state)
  assert.equal(state.active, CENTER_HOME)
  state = back(state)
  assert.equal(state.active, CENTER_HOME)
})

test('closing the active tab returns to its nearest open neighbour', () => {
  let state = select(createCenterNavigation(), viewPageKey('a'))
  state = select(state, viewPageKey('b'))
  state = close(state, viewPageKey('b'))
  assert.equal(state.active, 'view:a')
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:a'])
  assert.equal(state.history.includes(viewPageKey('b')), false)
})

test('closing a background tab keeps the active page', () => {
  let state = select(createCenterNavigation(), viewPageKey('a'))
  state = select(state, viewPageKey('b'))
  state = close(state, viewPageKey('a'))
  assert.equal(state.active, 'view:b')
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:b'])
  assert.equal(state.history.includes(viewPageKey('a')), false)
})

test('home tab cannot be closed', () => {
  const state = createCenterNavigation()
  assert.equal(close(state, CENTER_HOME), state)
})

test('prune removes withdrawn views and focuses the last open tab when active disappears', () => {
  let state = select(createCenterNavigation(), viewPageKey('a'))
  state = select(state, viewPageKey('b'))
  state = prune(state, ['a'])
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:a'])
  assert.equal(state.active, 'view:a')
  state = prune(state, [])
  assert.deepEqual(state.tabs, [CENTER_HOME])
  assert.equal(state.active, CENTER_HOME)
})

test('open center tabs are bounded while home and the active page stay', () => {
  let state = createCenterNavigation()
  for (let i = 0; i < MAX_CENTER_TABS + 8; i += 1) state = select(state, viewPageKey(`v${i}`))
  assert.equal(state.tabs.length, MAX_CENTER_TABS)
  assert.equal(state.tabs[0], CENTER_HOME)
  assert.equal(state.active, viewPageKey(`v${MAX_CENTER_TABS + 7}`))
  assert.ok(state.tabs.includes(state.active))
  assert.equal(state.tabs.includes(viewPageKey('v0')), false)
  assert.equal(state.history.every(key => state.tabs.includes(key)), true)
})

test('history is bounded and close keeps index usable', () => {
  let state = createCenterNavigation()
  for (let i = 0; i < MAX_CENTER_TABS - 1; i += 1) state = select(state, viewPageKey(`v${i}`))
  // Cycle open pages so the bounded tab set stays stable while history grows.
  for (let i = 0; i < MAX_CENTER_HISTORY + 12; i += 1) {
    state = select(state, viewPageKey(`v${i % (MAX_CENTER_TABS - 1)}`))
  }
  assert.equal(state.history.length, MAX_CENTER_HISTORY)
  assert.equal(state.index, MAX_CENTER_HISTORY - 1)
  const active = state.active
  state = close(state, active)
  assert.ok(state.index >= 0 && state.index < state.history.length)
  assert.equal(state.active, state.history[state.index])
})
test('split creates an empty active group and pages stay single-instance across groups', () => {
  let state = split(createCenterNavigation(), 'row')
  assert.equal(state.groups.length, 2)
  assert.equal(state.activeGroupId, 'g2')
  assert.deepEqual(state.tabs, [])
  assert.equal(state.tree.type, 'split')
  state = select(state, viewPageKey('a'))
  assert.deepEqual(state.tabs, ['view:a'])
  assert.deepEqual(state.groups[0].pages, [CENTER_HOME])
  state = activate(state, 'g1')
  state = select(state, viewPageKey('a'))
  assert.equal(state.activeGroupId, 'g2')
  assert.deepEqual(state.groups[1].pages, ['view:a'])
  assert.equal(state.groups.flatMap(group => group.pages).filter(page => page === 'view:a').length, 1)
})

test('same-direction splits stay siblings and the group limit is enforced', () => {
  let state = createCenterNavigation()
  for (let i = 0; i < MAX_CENTER_GROUPS + 2; i += 1) state = split(state, 'row')
  assert.equal(state.groups.length, MAX_CENTER_GROUPS)
  assert.equal(state.tree.type, 'split')
  assert.equal(state.tree.children.length, MAX_CENTER_GROUPS)
  assert.equal(state.tree.direction, 'row')
  const total = state.tree.children.reduce((sum, child) => sum + child.size, 0)
  assert.ok(Math.abs(total - 100) < 0.01)
})

test('perpendicular splits nest and the depth limit stops at three splits', () => {
  let state = split(createCenterNavigation(), 'row')
  state = select(state, viewPageKey('a'))
  state = split(state, 'column')
  assert.equal(state.groups.length, 3)
  assert.equal(state.tree.children[1].type, 'split')
  assert.equal(state.tree.children[1].direction, 'column')
  const before = state
  state = split(state, 'row')
  assert.equal(state, before)
})

test('moving the only page out of a group collapses the empty group', () => {
  let state = split(createCenterNavigation(), 'row')
  state = select(state, viewPageKey('a'))
  state = move(state, 'view:a', 'g1')
  assert.equal(state.groups.length, 1)
  assert.equal(state.activeGroupId, 'g1')
  assert.deepEqual(state.tabs, [CENTER_HOME, 'view:a'])
  assert.equal(state.tree.type, 'group')
})

test('serialized center split parses back without losing sizes or active group', () => {
  let state = split(createCenterNavigation(), 'row')
  state = select(state, viewPageKey('a'))
  const snapshot = serializeCenterState(state)
  const parsed = parseCenterState(snapshot)
  assert.equal(parsed.recovered, false)
  assert.deepEqual(serializeCenterState(parsed.state), snapshot)
})

test('pruning after hydrate removes a plugin page that no longer exists', () => {
  const snapshot = serializeCenterState(select(select(createCenterNavigation(), viewPageKey('gone')), viewPageKey('kept')))
  let state = centerNavigationReducer(createCenterNavigation(), { type: 'hydrate', state: snapshot })
  state = prune(state, ['kept'])
  assert.deepEqual(state.groups[0].pages, [CENTER_HOME, viewPageKey('kept')])
  assert.equal(state.active, viewPageKey('kept'))
  assert.equal(state.history.includes(viewPageKey('gone')), false)
})

test('hydrating a serialized snapshot then selecting keeps history usable', () => {
  const snapshot = serializeCenterState(split(createCenterNavigation(), 'row'))
  const hydrated = centerNavigationReducer(createCenterNavigation(), { type: 'hydrate', state: snapshot })
  assert.equal(hydrated.groups.length, 2)
  assert.equal(hydrated.activeGroupId, 'g2')
  assert.deepEqual(hydrated.history, [])
  const next = select(hydrated, CENTER_SETTINGS)
  assert.equal(next.active, CENTER_SETTINGS)
  assert.ok(next.history.includes(CENTER_SETTINGS))
})

test('parseCenterState rejects duplicate pages and missing tree groups', () => {
  const snapshot = serializeCenterState(createCenterNavigation())
  const duplicate = structuredClone(snapshot)
  duplicate.groups[0].pages.push(CENTER_HOME)
  const parsedDuplicate = parseCenterState(duplicate)
  assert.equal(parsedDuplicate.recovered, true)
  assert.deepEqual(parsedDuplicate.state.tabs, [CENTER_HOME])

  const missing = structuredClone(snapshot)
  missing.tree = { type: 'group', groupId: 'g9', size: 100 }
  const parsedMissing = parseCenterState(missing)
  assert.equal(parsedMissing.recovered, true)
  assert.equal(parsedMissing.state.groups.length, 1)
})
test('resizing groups updates the tree so reload keeps the split sizes', () => {
  let state = split(createCenterNavigation(), 'row')
  state = centerNavigationReducer(state, { type: 'resize-groups', sizes: { g1: 30, g2: 70 } })
  assert.equal(state.groups[0].size, 30)
  assert.equal(state.groups[1].size, 70)
  assert.equal(state.tree.children[0].size, 30)
  assert.equal(state.tree.children[1].size, 70)
  const parsed = parseCenterState(serializeCenterState(state))
  assert.equal(parsed.recovered, false)
  assert.equal(parsed.state.groups[1].size, 70)
  assert.equal(parsed.state.tree.children[1].size, 70)
})
