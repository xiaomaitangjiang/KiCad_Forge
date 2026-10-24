import { useCallback, useMemo, useReducer, useState } from 'react'

// 中心区导航只记录页面身份/活动页/返回历史/分组结构，不持有业务数据或 ReactNode。
export const CENTER_HOME = 'kf:symbols'
export const CENTER_SETTINGS = 'kf:settings'
export const CENTER_TYPES = 'kf:types'
export const CENTER_MATCHING = 'kf:matching'
// 内建页不来自插件清单，prune 时必须保留；它们与插件页一样可关闭、可返回。
export const CENTER_BUILTIN_PAGES = [CENTER_HOME, CENTER_SETTINGS, CENTER_TYPES, CENTER_MATCHING]
export const MAX_CENTER_TABS = 24
export const MAX_CENTER_HISTORY = 64
export const MAX_CENTER_GROUPS = 4
export const MAX_CENTER_GROUP_DEPTH = 3

export function viewPageKey(viewId) {
  return viewId ? `view:${viewId}` : CENTER_HOME
}

export function pageViewId(pageId) {
  return pageId?.startsWith('view:') ? pageId.slice(5) : null
}

function createGroup(id, pages = [], active = null, size = 100) {
  return {
    id,
    pages,
    active,
    history: active ? [active] : [],
    index: active ? 0 : -1,
    size,
  }
}

export function createCenterNavigation() {
  return project({
    groups: [createGroup('g1', [CENTER_HOME], CENTER_HOME)],
    tree: { type: 'group', groupId: 'g1', size: 100 },
    activeGroupId: 'g1',
    nextId: 2,
    tabs: [CENTER_HOME],
    active: CENTER_HOME,
    history: [CENTER_HOME],
    index: 0,
  })
}

function project(state) {
  const group = state.groups.find(item => item.id === state.activeGroupId) || state.groups[0]
  return {
    ...state,
    tabs: group ? group.pages : [],
    active: group ? group.active : null,
    history: group ? (group.history || []) : [],
    index: group && Number.isInteger(group.index) ? group.index : -1,
  }
}

export function findGroupByPage(state, page) {
  return state.groups.find(group => group.pages.includes(page)) || null
}

function pushHistory(group, key) {
  if (!key || !group.pages.includes(key)) return group
  const history = group.history.slice(0, Math.max(0, group.index + 1))
  if (history[history.length - 1] !== key) history.push(key)
  let index = history.length - 1
  if (history.length > MAX_CENTER_HISTORY) {
    history.splice(0, history.length - MAX_CENTER_HISTORY)
    index = history.length - 1
  }
  return { ...group, active: key, history, index }
}

function cleanGroup(group, pages, active = group.active) {
  const nextActive = pages.includes(active) ? active : (pages[0] || null)
  const history = group.history.filter(item => pages.includes(item))
  if (nextActive && !history.includes(nextActive)) history.push(nextActive)
  return { ...group, pages, active: nextActive, history, index: history.lastIndexOf(nextActive) }
}

function trimPageLimit(groups, keepKey) {
  let total = groups.reduce((sum, group) => sum + group.pages.length, 0)
  if (total <= MAX_CENTER_TABS) return groups
  let next = groups.map(group => ({ ...group, pages: [...group.pages], history: [...group.history] }))
  while (total > MAX_CENTER_TABS) {
    let victimGroup = null
    let victim = null
    for (const group of next) {
      victim = group.pages.find(page => page !== keepKey && page !== group.active)
      if (victim) { victimGroup = group; break }
    }
    if (!victim || !victimGroup) break
    const pages = victimGroup.pages.filter(page => page !== victim)
    const cleaned = cleanGroup(victimGroup, pages)
    next = next.map(group => group.id === victimGroup.id ? cleaned : group)
    total -= 1
  }
  return next
}

function selectInGroup(state, groupId, key) {
  if (!key) return state
  const owner = findGroupByPage(state, key)
  if (owner && owner.id !== groupId) return selectInGroup(state, owner.id, key)
  const group = state.groups.find(item => item.id === groupId)
  if (!group) return state
  if (group.active === key && state.activeGroupId === groupId) return state
  const pages = group.pages.includes(key) ? group.pages : [...group.pages, key]
  let groups = state.groups.map(item => item.id === groupId ? { ...item, pages } : item)
  groups = trimPageLimit(groups, key)
  groups = groups.map(item => item.id === groupId ? pushHistory(item, key) : item)
  return project({ ...state, groups, activeGroupId: groupId })
}

export function collectGroupIds(tree, output = []) {
  if (!tree) return output
  if (tree.type === 'group') output.push(tree.groupId)
  else for (const child of tree.children || []) collectGroupIds(child, output)
  return output
}

function leafDepth(tree, groupId, depth = 1) {
  if (!tree) return -1
  if (tree.type === 'group') return tree.groupId === groupId ? depth : -1
  for (const child of tree.children || []) {
    const found = leafDepth(child, groupId, depth + 1)
    if (found >= 0) return found
  }
  return -1
}

function insertSplit(node, targetId, direction, newGroupId) {
  if (node.type === 'group') {
    if (node.groupId !== targetId) return null
    return {
      tree: {
        type: 'split', direction, size: node.size ?? 100,
        children: [{ ...node, size: 50 }, { type: 'group', groupId: newGroupId, size: 50 }],
      },
      sizes: { [targetId]: 50, [newGroupId]: 50 },
    }
  }
  const direct = node.children.findIndex(child => child.type === 'group' && child.groupId === targetId)
  if (direct >= 0) {
    const target = node.children[direct]
    if (node.direction !== direction) {
      const nested = {
        type: 'split', direction, size: target.size ?? 50,
        children: [{ ...target, size: 50 }, { type: 'group', groupId: newGroupId, size: 50 }],
      }
      const children = [...node.children]
      children[direct] = nested
      return { tree: { ...node, children }, sizes: { [targetId]: 50, [newGroupId]: 50 } }
    }
    const half = Math.max(10, (target.size ?? 50) / 2)
    return {
      tree: {
        ...node,
        children: [
          ...node.children.slice(0, direct),
          { ...target, size: half },
          { type: 'group', groupId: newGroupId, size: half },
          ...node.children.slice(direct + 1),
        ],
      },
      sizes: { [targetId]: half, [newGroupId]: half },
    }
  }
  for (let index = 0; index < node.children.length; index += 1) {
    const child = node.children[index]
    if (child.type !== 'split') continue
    const result = insertSplit(child, targetId, direction, newGroupId)
    if (!result) continue
    const children = [...node.children]
    children[index] = result.tree
    return { tree: { ...node, children }, sizes: result.sizes }
  }
  return null
}

function removeGroup(tree, groupId) {
  if (!tree) return null
  if (tree.type === 'group') return tree.groupId === groupId ? null : tree
  const children = []
  for (const child of tree.children || []) {
    if (child.type === 'group' && child.groupId === groupId) continue
    const next = removeGroup(child, groupId)
    if (next) children.push(next)
  }
  if (children.length === 0) return null
  if (children.length === 1) {
    const only = children[0]
    return { ...only, size: tree.size ?? only.size }
  }
  return { ...tree, children }
}

function splitGroup(state, direction) {
  if (state.groups.length >= MAX_CENTER_GROUPS) return state
  const targetId = state.activeGroupId
  const target = state.groups.find(group => group.id === targetId)
  if (!target) return state
  if (leafDepth(state.tree, targetId) >= MAX_CENTER_GROUP_DEPTH) return state
  const newGroupId = `g${state.nextId}`
  const result = insertSplit(state.tree, targetId, direction, newGroupId)
  if (!result) return state
  const groups = state.groups.map(group => (
    result.sizes[group.id] == null ? group : { ...group, size: result.sizes[group.id] }
  ))
  groups.push(createGroup(newGroupId, [], null, result.sizes[newGroupId] ?? 50))
  return project({
    ...state,
    groups,
    tree: result.tree,
    activeGroupId: newGroupId,
    nextId: state.nextId + 1,
  })
}

function movePage(state, key, targetGroupId) {
  if (!key) return state
  const source = findGroupByPage(state, key)
  const target = state.groups.find(group => group.id === targetGroupId)
  if (!source || !target) return state
  if (source.id === target.id) return selectInGroup(state, target.id, key)
  if (target.pages.includes(key)) return project({ ...state, activeGroupId: target.id })
  const pages = source.pages.filter(page => page !== key)
  let groups = state.groups.map(group => {
    if (group.id === source.id) return cleanGroup(group, pages)
    if (group.id === target.id) return pushHistory({ ...group, pages: [...group.pages, key] }, key)
    return group
  })
  let tree = state.tree
  let activeGroupId = target.id
  if (pages.length === 0 && groups.length > 1) {
    groups = groups.filter(group => group.id !== source.id)
    tree = removeGroup(tree, source.id)
    if (!groups.some(group => group.id === activeGroupId)) {
      const ids = collectGroupIds(tree)
      activeGroupId = ids[0] || groups[0].id
    }
  }
  return project({ ...state, groups, tree, activeGroupId })
}

function moveNext(state) {
  const ids = collectGroupIds(state.tree)
  if (ids.length < 2) return state
  const index = ids.indexOf(state.activeGroupId)
  const targetId = ids[(index + 1) % ids.length]
  const active = state.groups.find(group => group.id === state.activeGroupId)?.active
  if (!active) return project({ ...state, activeGroupId: targetId })
  return movePage(state, active, targetId)
}

function closePage(state, key, groupId) {
  if (!key) return state
  const group = groupId
    ? state.groups.find(item => item.id === groupId)
    : findGroupByPage(state, key)
  if (!group || !group.pages.includes(key)) return state
  if (group.pages.length === 1) {
    // 次级组只有一页时合并掉；最后一个组保留零页空态，由重开入口恢复。
    if (state.groups.length > 1) {
      const ids = collectGroupIds(state.tree)
      const groups = state.groups.filter(item => item.id !== group.id)
      const tree = removeGroup(state.tree, group.id)
      const remaining = ids.filter(id => id !== group.id)
      const oldIndex = Math.max(0, ids.indexOf(group.id))
      let activeGroupId = state.activeGroupId
      if (activeGroupId === group.id) {
        activeGroupId = remaining[Math.min(oldIndex, remaining.length - 1)] || groups[0].id
      }
      return project({ ...state, groups, tree, activeGroupId })
    }
    const cleaned = cleanGroup(group, [], null)
    return project({ ...state, groups: state.groups.map(item => item.id === group.id ? cleaned : item) })
  }
  const oldIndex = group.pages.indexOf(key)
  const pages = group.pages.filter(page => page !== key)
  const nextActive = group.active === key ? (pages[Math.min(oldIndex, pages.length - 1)] || pages[0]) : group.active
  const cleaned = cleanGroup(group, pages, nextActive)
  const groups = state.groups.map(item => item.id === group.id ? cleaned : item)
  return project({ ...state, groups })
}

function back(state, groupId) {
  const targetId = groupId || state.activeGroupId
  const group = state.groups.find(item => item.id === targetId)
  if (!group || group.index <= 0) return state
  const index = group.index - 1
  const active = group.history[index]
  const groups = state.groups.map(item => item.id === targetId ? { ...item, index, active } : item)
  return project({ ...state, groups, activeGroupId: targetId })
}

function prunePages(state, viewIds) {
  const allowed = new Set([...CENTER_BUILTIN_PAGES, ...(viewIds || []).map(viewPageKey)])
  let groups = state.groups.map(group => {
    const pages = group.pages.filter(page => allowed.has(page))
    if (pages.length === group.pages.length) return group
    const nextActive = pages.includes(group.active) ? group.active : (pages[pages.length - 1] || null)
    return cleanGroup(group, pages, nextActive)
  })
  let tree = state.tree
  let removed = true
  while (groups.length > 1 && removed) {
    removed = false
    for (const group of groups) {
      if (group.pages.length > 0) continue
      groups = groups.filter(item => item.id !== group.id)
      tree = removeGroup(tree, group.id)
      removed = true
      break
    }
  }
  let activeGroupId = state.activeGroupId
  if (!groups.some(group => group.id === activeGroupId)) activeGroupId = collectGroupIds(tree)[0] || groups[0].id
  return project({ ...state, groups, tree, activeGroupId })
}

function applyTreeSizes(tree, sizes) {
  if (!tree) return tree
  if (tree.type === 'group') {
    const size = sizes[tree.groupId]
    return Number.isFinite(size) && size !== tree.size ? { ...tree, size } : tree
  }
  let changed = false
  const children = (tree.children || []).map((child) => {
    const next = applyTreeSizes(child, sizes)
    if (next !== child) changed = true
    return next
  })
  return changed ? { ...tree, children } : tree
}

function resizeGroup(state, groupId, size) {
  if (!Number.isFinite(size)) return state
  return resizeGroups(state, { [groupId]: size })
}

function resizeGroups(state, sizes) {
  if (!sizes || typeof sizes !== 'object') return state
  const updates = {}
  const groups = state.groups.map(group => {
    const value = sizes[group.id]
    if (!Number.isFinite(value)) return group
    const clamped = Math.min(90, Math.max(10, value))
    if (Math.abs(clamped - group.size) < 0.05) return group
    updates[group.id] = clamped
    return { ...group, size: clamped }
  })
  if (!Object.keys(updates).length) return state
  return project({ ...state, groups, tree: applyTreeSizes(state.tree, updates) })
}

function activateGroup(state, groupId) {
  if (!state.groups.some(group => group.id === groupId)) return state
  return project({ ...state, activeGroupId: groupId })
}

export function centerNavigationReducer(state, action) {
  switch (action.type) {
    case 'select':
      return selectInGroup(state, action.groupId || state.activeGroupId, action.key)
    case 'activate-group':
      return activateGroup(state, action.groupId)
    case 'split':
      return splitGroup(state, action.direction === 'column' ? 'column' : 'row')
    case 'move-page':
      return movePage(state, action.key, action.groupId)
    case 'move-next':
      return moveNext(state)
    case 'resize-group':
      return resizeGroup(state, action.groupId, action.size)
    case 'resize-groups':
      return resizeGroups(state, action.sizes)
    case 'back':
      return back(state, action.groupId)
    case 'close':
      return closePage(state, action.key, action.groupId)
    case 'prune':
      return prunePages(state, action.viewIds)
    case 'hydrate':
      // 宿主持久化的是 serializeCenterState 产物（不含 history/index），
      // 必须重新规范化，否则第一次选页会读到 undefined 历史。
      return action.state?.groups?.length ? project(parseCenterState(action.state).state) : state
    default:
      return state
  }
}

function cloneTree(node) {
  if (!node) return null
  if (node.type === 'group') return { type: 'group', groupId: node.groupId, size: node.size }
  return {
    type: 'split',
    direction: node.direction,
    size: node.size,
    children: (node.children || []).map(cloneTree),
  }
}

export function serializeCenterState(state) {
  const normalized = project(state)
  return {
    activeGroupId: normalized.activeGroupId,
    nextId: normalized.nextId,
    tree: cloneTree(normalized.tree),
    groups: normalized.groups.map(group => ({
      id: group.id,
      pages: [...group.pages],
      active: group.active,
      size: group.size,
    })),
  }
}

function validPage(page) {
  if (typeof page !== 'string' || page.length === 0 || page.length > 200) return false
  for (const char of page) {
    const code = char.codePointAt(0)
    if (code < 0x20 || code === 0x7f) return false
  }
  return true
}

function validateTree(node, groupIds, depth = 1) {
  if (!node || typeof node !== 'object') return false
  if (node.type === 'group') return depth <= MAX_CENTER_GROUP_DEPTH && groupIds.has(node.groupId)
  if (node.type !== 'split') return false
  if (node.direction !== 'row' && node.direction !== 'column') return false
  if (!Array.isArray(node.children) || node.children.length < 2 || node.children.length > MAX_CENTER_GROUPS) return false
  if (node.size != null && (!Number.isFinite(node.size) || node.size <= 0 || node.size > 100)) return false
  return node.children.every(child => validateTree(child, groupIds, depth + 1))
}

export function parseCenterState(raw) {
  const fallback = () => ({ state: createCenterNavigation(), recovered: false })
  if (!raw) return fallback()
  if (typeof raw !== 'object' || Array.isArray(raw)) return { state: createCenterNavigation(), recovered: true }
  let recovered = false
  const ids = new Set()
  const groups = []
  const seenPages = new Set()
  const rawGroups = raw.groups
  if (!Array.isArray(rawGroups) || rawGroups.length === 0 || rawGroups.length > MAX_CENTER_GROUPS) {
    return { state: createCenterNavigation(), recovered: true }
  }
  for (const item of rawGroups) {
    if (!item || typeof item !== 'object') return { state: createCenterNavigation(), recovered: true }
    if (typeof item.id !== 'string' || !/^g[0-9]+$/.test(item.id) || ids.has(item.id)) {
      return { state: createCenterNavigation(), recovered: true }
    }
    ids.add(item.id)
    if (!Array.isArray(item.pages)) return { state: createCenterNavigation(), recovered: true }
    const pages = []
    for (const page of item.pages) {
      if (!validPage(page)) { recovered = true; continue }
      if (seenPages.has(page)) { recovered = true; continue }
      seenPages.add(page)
      pages.push(page)
    }
    const size = Number.isFinite(item.size) ? Math.min(100, Math.max(5, item.size)) : 100
    if (!Number.isFinite(item.size)) recovered = true
    const active = pages.includes(item.active) ? item.active : (pages[0] || null)
    if (item.active != null && active !== item.active) recovered = true
    groups.push(cleanGroup(createGroup(item.id, pages, active, size), pages, active))
  }
  if (!raw.tree || !validateTree(raw.tree, ids)) return { state: createCenterNavigation(), recovered: true }
  const leaves = collectGroupIds(raw.tree)
  if (leaves.length !== ids.size || new Set(leaves).size !== leaves.length || !leaves.every(id => ids.has(id))) {
    return { state: createCenterNavigation(), recovered: true }
  }
  let total = groups.reduce((sum, group) => sum + group.pages.length, 0)
  if (total > MAX_CENTER_TABS) {
    recovered = true
    let keep = total
    for (let index = groups.length - 1; index >= 0 && keep > MAX_CENTER_TABS; index -= 1) {
      const group = groups[index]
      while (keep > MAX_CENTER_TABS && group.pages.length > 1) {
        const victim = group.pages.find(page => page !== group.active)
        if (!victim) break
        group.pages = group.pages.filter(page => page !== victim)
        keep -= 1
      }
    }
  }
  let activeGroupId = ids.has(raw.activeGroupId) ? raw.activeGroupId : groups[0].id
  if (activeGroupId !== raw.activeGroupId) recovered = true
  const numeric = groups.map(group => Number(group.id.slice(1))).filter(Number.isFinite)
  const nextId = Math.max((Number.isFinite(raw.nextId) ? raw.nextId : 0), (numeric.length ? Math.max(...numeric) : 1) + 1, 2)
  const state = project({
    groups: groups.map(group => cleanGroup(group, group.pages, group.active)),
    tree: cloneTree(raw.tree),
    activeGroupId,
    nextId,
    tabs: [],
    active: null,
    history: [],
    index: -1,
  })
  return { state, recovered }
}

export function useCenterNavigation() {
  const [state, dispatch] = useReducer(centerNavigationReducer, undefined, createCenterNavigation)
  const [hydrated, setHydrated] = useState(false)
  const select = useCallback((key, groupId) => dispatch({ type: 'select', key, groupId }), [])
  const selectView = useCallback((viewId, groupId) => dispatch({ type: 'select', key: viewPageKey(viewId), groupId }), [])
  const ensureHomeOpen = useCallback(() => dispatch({ type: 'select', key: CENTER_HOME }), [])
  const close = useCallback((key, groupId) => dispatch({ type: 'close', key, groupId }), [])
  const back = useCallback((groupId) => dispatch({ type: 'back', groupId }), [])
  const prune = useCallback((viewIds) => dispatch({ type: 'prune', viewIds }), [])
  const selectInGroup = useCallback((groupId, key) => dispatch({ type: 'select', groupId, key }), [])
  const activateGroup = useCallback((groupId) => dispatch({ type: 'activate-group', groupId }), [])
  const splitGroup = useCallback((direction) => dispatch({ type: 'split', direction }), [])
  const movePage = useCallback((key, groupId) => dispatch({ type: 'move-page', key, groupId }), [])
  const moveNext = useCallback(() => dispatch({ type: 'move-next' }), [])
  const resizeGroup = useCallback((groupId, size) => dispatch({ type: 'resize-group', groupId, size }), [])
  const resizeGroups = useCallback((sizes) => dispatch({ type: 'resize-groups', sizes }), [])
  const hydrate = useCallback((centerState) => {
    dispatch({ type: 'hydrate', state: centerState })
    setHydrated(true)
  }, [])
  const activeGroup = state.groups.find(group => group.id === state.activeGroupId) || state.groups[0]
  const viewId = pageViewId(activeGroup?.active)
  return useMemo(() => ({
    tabs: state.tabs,
    active: state.active,
    viewId,
    canBack: state.index > 0,
    groups: state.groups,
    tree: state.tree,
    activeGroupId: state.activeGroupId,
    activeGroup,
    groupCount: state.groups.length,
    canSplit: state.groups.length < MAX_CENTER_GROUPS
      && leafDepth(state.tree, state.activeGroupId) < MAX_CENTER_GROUP_DEPTH,
    canMoveNext: state.groups.length > 1 && Boolean(activeGroup?.active),
    hydrated,
    select,
    selectView,
    ensureHomeOpen,
    close,
    back,
    prune,
    selectInGroup,
    activateGroup,
    splitGroup,
    movePage,
    moveNext,
    resizeGroup,
    resizeGroups,
    hydrate,
  }), [state, activeGroup, viewId, hydrated, select, selectView, ensureHomeOpen, close, back, prune, selectInGroup,
    activateGroup, splitGroup, movePage, moveNext, resizeGroup, resizeGroups, hydrate])
}
