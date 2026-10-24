// 对象列表选择模型（ED-020..024）：纯数据，只存 UUID。
// 集合（ids）与详情对象（activeId）分开；anchorId 是 Shift 连续选择的起点。
// 顺序来自当前排序下的结果列表，由调用方传入 orderedIds，模型不持对象快照。

export function emptySelection() {
  return { ids: [], activeId: null, anchorId: null }
}

export function selectionCount(selection) {
  return selection?.ids.length || 0
}

export function isSelected(selection, id) {
  return Boolean(id) && (selection?.ids.includes(id) ?? false)
}

// 单击：用新对象替换整个集合，锚点跟随。
export function selectOnly(id) {
  return { ids: [id], activeId: id, anchorId: id }
}

// Ctrl/Cmd 单击：切换单个 UUID；活动对象指向最后点击的项，锚点也移到这里。
export function toggleSelection(selection, id) {
  const ids = selection?.ids || []
  if (ids.includes(id)) {
    const next = ids.filter(item => item !== id)
    const activeId = selection.activeId === id ? (next[next.length - 1] || null) : selection.activeId
    return { ids: next, activeId, anchorId: id }
  }
  return { ids: [...ids, id], activeId: id, anchorId: id }
}

// Shift 单击：按当前排序从锚点到目标连续选择（含两端）。锚点或目标不在
// 当前结果里时退化为单选，绝不跨过滤范围拼接。
export function extendSelection(selection, id, orderedIds) {
  const order = Array.isArray(orderedIds) ? orderedIds : []
  const anchorId = selection?.anchorId && order.includes(selection.anchorId)
    ? selection.anchorId
    : (selection?.activeId && order.includes(selection.activeId) ? selection.activeId : null)
  if (!anchorId) return selectOnly(id)
  const from = order.indexOf(anchorId)
  const to = order.indexOf(id)
  if (to < 0) return selectOnly(id)
  const [start, end] = from <= to ? [from, to] : [to, from]
  return { ids: order.slice(start, end + 1), activeId: id, anchorId }
}

// 全选：当前搜索/筛选/工作空间/排序下的已加载结果（含视口外）。
// 集合态不设活动对象，详情区显示集合摘要。
export function selectAllIn(orderedIds) {
  const ids = Array.isArray(orderedIds) ? [...orderedIds] : []
  return { ids, activeId: null, anchorId: null }
}

// 结果刷新：剔除已消失的 UUID，保住仍然存在的集合与锚点。
// 没有任何变化时返回原引用，避免无意义重渲染。
export function normalizeSelection(selection, availableIds) {
  if (!selection) return emptySelection()
  const available = availableIds instanceof Set ? availableIds : new Set(availableIds || [])
  const ids = selection.ids.filter(id => available.has(id))
  if (ids.length === selection.ids.length) return selection
  const activeId = selection.activeId && ids.includes(selection.activeId) ? selection.activeId : null
  const anchorId = selection.anchorId && ids.includes(selection.anchorId) ? selection.anchorId : activeId
  return { ids, activeId, anchorId }
}

// 单个对象消失（删除）时只移除它，其余集合保留。
export function removeFromSelection(selection, id) {
  if (!isSelected(selection, id)) return selection
  const ids = selection.ids.filter(item => item !== id)
  const activeId = selection.activeId === id ? (ids[ids.length - 1] || null) : selection.activeId
  const anchorId = selection.anchorId === id ? activeId : selection.anchorId
  return { ids, activeId, anchorId }
}

export function clearSelection() {
  return emptySelection()
}
