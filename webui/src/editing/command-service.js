/**
 * 前端编辑命令分发（纯数据，无 DOM，可在 Node 里单测）。
 *
 * 规则来自 docs/specs/EDITING_COMMANDS.md：
 * - ED-001 一次只作用一个上下文（弹窗文本 → 视图文本 → 对象列表）；
 * - ED-002 菜单打开前冻结目标与选区，命令只作用于冻结的那一份；
 * - ED-003 菜单/快捷键/右键走同一个入口、同一份可用状态；
 * - ED-004 平台快捷键与真实分发（Win/Linux Ctrl+，macOS Cmd+）；
 * - ED-005 IME 组合期间不截获；
 * - ED-006 不可用要给原因，执行失败要可见。
 */

export const COMMANDS = Object.freeze(['copy', 'cut', 'paste', 'selectAll', 'undo', 'redo'])

export const REASONS = Object.freeze({
  noTarget: 'noTarget',
  noSelection: 'noSelection',
  noSelectionSupport: 'noSelectionSupport',
  readOnly: 'readOnly',
  noObjectSelection: 'noObjectSelection',
  noResults: 'noResults',
  objectUnsupported: 'objectUnsupported',
  clipboardEmpty: 'clipboardEmpty',
  historyEmpty: 'historyEmpty',
  busy: 'busy',
  uncertain: 'uncertain',
  staleTarget: 'staleTarget',
})

// 会改变宿主状态的对象命令：跨命令互斥，避免 undo 与 cut/redo 交错。
const OBJECT_WRITE_COMMANDS = new Set(['cut', 'paste', 'undo', 'redo'])

function isObjectWrite(commandId) {
  return OBJECT_WRITE_COMMANDS.has(commandId)
}

/** 网络/解析失败或 5xx 时，服务端可能已经提交，只是结果没有回到前端。 */
export function isUncertainWriteError(error) {
  if (!error) return false
  if (error.uncertain === true) return true
  if (error.kind === 'network' || error.kind === 'parse') return true
  return Number.isFinite(error.status) && error.status >= 500
}

const ENABLED = Object.freeze({ disabled: false, reason: '' })

function disabled(reason) {
  return { disabled: true, reason }
}

export function detectPlatform(source = typeof navigator !== 'undefined' ? navigator : null) {
  const text = String(
    (source && (source.userAgentData?.platform || source.platform || source.userAgent)) || '',
  )
  return /mac|iphone|ipad|ipod/i.test(text) ? 'mac' : 'other'
}

function normalizeText(raw) {
  if (!raw || raw.valid === false) return null
  return {
    key: raw.key ?? null,
    // DOM 目标与冻结选区只在真实运行时快照里存在；命令服务保持纯数据，
    // 但绝不能把它们丢掉，否则异步命令只能重新读取“当前”选区。
    target: raw.target ?? null,
    surface: raw.surface ?? null,
    kind: typeof raw.kind === 'string' ? raw.kind : '',
    range: raw.range ?? null,
    version: Number.isFinite(raw.version) ? raw.version : 0,
    valid: true,
    readOnly: raw.readOnly === true,
    selectable: raw.selectable !== false,
    hasSelection: raw.hasSelection === true,
    value: typeof raw.value === 'string' ? raw.value : '',
    selectionStart: Number.isFinite(raw.selectionStart) ? raw.selectionStart : 0,
    selectionEnd: Number.isFinite(raw.selectionEnd) ? raw.selectionEnd : 0,
  }
}

function normalizeHistory(raw) {
  if (!raw) return { undoLabel: '', redoLabel: '', undoable: false, redoable: false }
  return {
    undoLabel: typeof raw.undoLabel === 'string' ? raw.undoLabel : '',
    redoLabel: typeof raw.redoLabel === 'string' ? raw.redoLabel : '',
    undoable: raw.undoable === true,
    redoable: raw.redoable === true,
  }
}

function normalizeClipboard(raw) {
  if (!raw) return null
  return {
    kind: raw.kind || '',
    version: Number.isFinite(raw.version) ? raw.version : 0,
    bytes: Number.isFinite(raw.bytes) ? raw.bytes : 0,
    items: Number.isFinite(raw.items) ? raw.items : 0,
  }
}

function normalizeObjects(raw) {
  if (!raw) return null
  return {
    domain: raw.domain || 'symbols',
    count: Number.isFinite(raw.count) ? raw.count : 0,
    total: Number.isFinite(raw.total) ? raw.total : 0,
    supported: raw.supported !== false,
    unsupported: Array.isArray(raw.unsupported) ? raw.unsupported.filter((id) => COMMANDS.includes(id)) : [],
    writable: raw.writable !== false,
    busy: raw.busy === true,
    pendingCutCount: Number.isFinite(raw.pendingCutCount) ? raw.pendingCutCount : 0,
    clipboard: normalizeClipboard(raw.clipboard),
    history: normalizeHistory(raw.history),
  }
}

/** 把任意快照源收敛成命令服务认得的形状。 */
export function normalizeSnapshot(raw) {
  const source = raw && typeof raw === 'object' ? raw : {}
  const owner = source.owner === 'text' || source.owner === 'objects' ? source.owner : null
  const text = normalizeText(source.text)
  const objects = normalizeObjects(source.objects)
  const resolved = owner === 'text' && !text ? null : (owner === 'objects' && !objects ? null : owner)
  return {
    owner: resolved,
    scopeKey: typeof source.scopeKey === 'string' ? source.scopeKey : '',
    epoch: Number.isFinite(source.epoch) ? source.epoch : 0,
    text,
    objects,
  }
}

function textState(text, commandId) {
  const readOnly = text.readOnly === true
  switch (commandId) {
    case 'copy':
      if (!text.selectable) return disabled(REASONS.noSelectionSupport)
      if (!text.hasSelection) return disabled(REASONS.noSelection)
      return ENABLED
    case 'cut':
      if (readOnly) return disabled(REASONS.readOnly)
      if (!text.selectable) return disabled(REASONS.noSelectionSupport)
      if (!text.hasSelection) return disabled(REASONS.noSelection)
      return ENABLED
    case 'paste':
    case 'undo':
    case 'redo':
      return readOnly ? disabled(REASONS.readOnly) : ENABLED
    case 'selectAll':
      return text.selectable ? ENABLED : disabled(REASONS.noSelectionSupport)
    default:
      return disabled(REASONS.noTarget)
  }
}

function objectState(objects, commandId, gate = null) {
  if (!objects || !objects.supported) return disabled(REASONS.objectUnsupported)
  if (objects.unsupported.includes(commandId)) return disabled(REASONS.objectUnsupported)
  if (isObjectWrite(commandId)) {
    if (gate && gate.uncertain === true) return disabled(REASONS.uncertain)
    if (objects.busy || (gate && gate.busy === true)) return disabled(REASONS.busy)
  }
  switch (commandId) {
    case 'copy':
      return objects.count > 0 ? ENABLED : disabled(REASONS.noObjectSelection)
    case 'cut':
      if (!objects.writable) return disabled(REASONS.readOnly)
      return objects.count > 0 ? ENABLED : disabled(REASONS.noObjectSelection)
    case 'paste':
      return objects.clipboard ? ENABLED : disabled(REASONS.clipboardEmpty)
    case 'selectAll':
      return objects.total > 0 ? ENABLED : disabled(REASONS.noResults)
    case 'undo':
      return objects.history.undoable ? ENABLED : disabled(REASONS.historyEmpty)
    case 'redo':
      return objects.history.redoable ? ENABLED : disabled(REASONS.historyEmpty)
    default:
      return disabled(REASONS.noTarget)
  }
}

/** 单个命令的可用状态与原因码（文案由组件翻译）。 */
export function commandState(snapshot, commandId, gate = null) {
  const state = normalizeSnapshot(snapshot)
  if (state.owner === 'text') return textState(state.text, commandId)
  if (state.owner === 'objects') return objectState(state.objects, commandId, gate)
  return disabled(REASONS.noTarget)
}

export function commandStates(snapshot, gate = null) {
  const state = normalizeSnapshot(snapshot)
  const result = {}
  for (const commandId of COMMANDS) result[commandId] = commandState(state, commandId, gate)
  return result
}

/**
 * 冻结快照是否仍然指向同一个目标。
 * 内容允许变化（同步命令本就要作用于当前值），但目标/作用域/版本必须一致。
 */
export function isSnapshotCurrent(before, live) {
  const a = normalizeSnapshot(before)
  const b = normalizeSnapshot(live)
  if (a.scopeKey !== b.scopeKey) return false
  if (a.epoch !== b.epoch) return false

  if (a.owner === 'text') {
    if (!a.text || !b.text) return false
    if (a.text.key !== b.text.key) return false
    if (a.text.target && b.text.target && a.text.target !== b.text.target) return false
    return a.text.version === b.text.version
  }
  if (a.owner === 'objects') {
    if (!a.objects || !b.objects) return false
    return a.objects.domain === b.objects.domain
  }
  return a.owner === b.owner
}

const MAC_SHORTCUTS = Object.freeze({
  copy: 'Cmd+C',
  cut: 'Cmd+X',
  paste: 'Cmd+V',
  selectAll: 'Cmd+A',
  undo: 'Cmd+Z',
  redo: 'Cmd+Shift+Z',
})

const PC_SHORTCUTS = Object.freeze({
  copy: 'Ctrl+C',
  cut: 'Ctrl+X',
  paste: 'Ctrl+V',
  selectAll: 'Ctrl+A',
  undo: 'Ctrl+Z',
  redo: 'Ctrl+Y',
})

export function shortcutLabel(commandId, platform = detectPlatform()) {
  return (platform === 'mac' ? MAC_SHORTCUTS : PC_SHORTCUTS)[commandId] || ''
}

/**
 * 键盘事件 → 命令 id；不匹配返回 null。
 * IME 组合、已处理事件、Alt 组合和非平台主修饰键都不参与。
 */
export function matchShortcut(event, platform = detectPlatform()) {
  if (!event || event.defaultPrevented) return null
  if (event.isComposing === true || event.keyCode === 229 || event.key === 'Process') return null
  if (event.altKey === true) return null
  const mac = platform === 'mac'
  if (mac) {
    if (event.metaKey !== true || event.ctrlKey === true) return null
  } else if (event.ctrlKey !== true || event.metaKey === true) {
    return null
  }
  const key = typeof event.key === 'string' ? event.key.toLowerCase() : ''
  if (key.length !== 1) return null
  const shift = event.shiftKey === true
  switch (key) {
    case 'c':
      return shift ? null : 'copy'
    case 'x':
      return shift ? null : 'cut'
    case 'v':
      return shift ? null : 'paste'
    case 'a':
      return shift ? null : 'selectAll'
    case 'z':
      return shift ? 'redo' : 'undo'
    case 'y':
      return mac || shift ? null : 'redo'
    default:
      return null
  }
}

/**
 * 命令服务：唯一的命令入口。菜单、快捷键、右键都经由它。
 * getSnapshot() 每次读取实时快照；execute 用冻结快照派发。
 */
export function createCommandService(options = {}) {
  const getSnapshot = typeof options.getSnapshot === 'function' ? options.getSnapshot : () => null
  const handlers = options.handlers || {}
  const platform = options.platform || detectPlatform()
  const onEvent = typeof options.onEvent === 'function' ? options.onEvent : () => {}
  const onError = typeof options.onError === 'function' ? options.onError : () => {}
  const handledEvents = new WeakSet()
  const inFlight = new Set()
  // 对象写入跨命令互斥：undo/redo/cut/paste 任意一个在飞时，其它写命令不可用。
  // uncertain 在写入结果未知时保持，直到调用方完成只读核对。
  let objectWriteInFlight = false
  let externalWriteBusy = false
  let writeUncertain = false

  function gate() {
    return { busy: objectWriteInFlight || externalWriteBusy, uncertain: writeUncertain }
  }

  function read() {
    return normalizeSnapshot(getSnapshot())
  }

  function getState(commandId, frozen) {
    return commandState(frozen ? normalizeSnapshot(frozen) : read(), commandId, gate())
  }

  function getStates(frozen) {
    return commandStates(frozen ? normalizeSnapshot(frozen) : read(), gate())
  }

  function handlerFor(commandId, snapshot) {
    if (snapshot.owner === 'objects' && options.objects && typeof options.objects[commandId] === 'function') {
      return options.objects[commandId]
    }
    return typeof handlers[commandId] === 'function' ? handlers[commandId] : null
  }

  async function execute(commandId, frozen) {
    if (!COMMANDS.includes(commandId)) {
      throw new Error('unknown editing command: ' + String(commandId))
    }
    const snapshot = normalizeSnapshot(frozen || getSnapshot())
    const state = commandState(snapshot, commandId, gate())
    if (state.disabled) return { status: 'disabled', commandId, reason: state.reason }

    if (!isSnapshotCurrent(snapshot, read())) {
      return { status: 'stale', commandId, reason: REASONS.staleTarget }
    }
    const handler = handlerFor(commandId, snapshot)
    if (!handler) return { status: 'disabled', commandId, reason: REASONS.objectUnsupported }
    if (inFlight.has(commandId)) return { status: 'disabled', commandId, reason: REASONS.busy }

    const objectWrite = snapshot.owner === 'objects' && isObjectWrite(commandId)
    if (objectWrite) objectWriteInFlight = true
    inFlight.add(commandId)
    onEvent({ type: 'execute', commandId, owner: snapshot.owner })
    try {
      const result = await handler(snapshot)
      if (result && result.status === 'uncertain' && objectWrite) writeUncertain = true
      return result === undefined ? { status: 'done', commandId } : { ...result, commandId }
    } catch (error) {
      // undo/redo 已经落到宿主写路径：网络中断或 5xx 时不能当作确定失败，
      // 也不能让下一次写命令直接重试，先冻结到调用方核对历史快照。
      if (objectWrite && (commandId === 'undo' || commandId === 'redo') && isUncertainWriteError(error)) {
        writeUncertain = true
      }
      onError(error, commandId)
      return { status: 'failed', commandId, error }
    } finally {
      inFlight.delete(commandId)
      if (objectWrite) objectWriteInFlight = false
    }
  }

  /**
   * 全局快捷键入口。命令不可用时不 preventDefault，让浏览器原生行为接管
   * （包装器把 Ctrl+C 留给系统文本复制，对象命令不可用时也不会吞键）。
   */
  function handleKeydown(event) {
    if (!event || handledEvents.has(event)) return null
    const commandId = matchShortcut(event, platform)
    if (!commandId) return null
    const snapshot = read()
    const state = commandState(snapshot, commandId)
    if (state.disabled) {
      onEvent({ type: 'shortcut-disabled', commandId, reason: state.reason })
      return null
    }
    handledEvents.add(event)
    if (typeof event.preventDefault === 'function') event.preventDefault()
    return execute(commandId, snapshot)
  }

  return {
    platform,
    read,
    freeze: read,
    getState,
    getStates,
    execute,
    handleKeydown,
    setExternalBusy: (busy) => { externalWriteBusy = busy === true },
    isWriteBusy: () => objectWriteInFlight || externalWriteBusy,
    isWriteUncertain: () => writeUncertain,
    clearWriteUncertain: () => { writeUncertain = false },
  }
}
