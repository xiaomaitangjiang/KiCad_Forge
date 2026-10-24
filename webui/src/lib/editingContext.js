/**
 * 编辑命令上下文：菜单打开时锁定一个真实可见的文本控件及其选区，
 * 之后所有编辑动作只作用于该快照，不再回落到"最后一个输入框"。
 *
 * 约束来自 docs/specs/EDITING_COMMANDS.md：
 * - 一次只作用一个上下文；隐藏页面（keep-alive）里的控件不算上下文；
 * - 顶层弹窗打开时，目标必须在顶层弹窗内；
 * - 异步剪贴板返回后必须用同一快照复核，选区或内容变了就中止。
 * - 控件是否支持文本选区按真实能力判断（typeof selectionStart === 'number'），
 *   不把 number/email 之类的控件伪装成"空选区"。
 */

const TEXT_INPUT_TYPES = new Set([
  '', 'text', 'search', 'url', 'tel', 'password', 'email', 'number',
])

const DIALOG_SELECTOR = '.modal[role="dialog"]'

function hasDom() {
  return typeof document !== 'undefined' && document !== null
}

function isVisible(element) {
  return Boolean(element && element.isConnected && element.getClientRects().length > 0)
}

function topVisibleDialog() {
  if (!hasDom() || typeof document.querySelectorAll !== 'function') return null
  const dialogs = [...document.querySelectorAll(DIALOG_SELECTOR)].filter(isVisible)
  return dialogs.length ? dialogs[dialogs.length - 1] : null
}

function ownerDialog(element) {
  if (!element || typeof element.closest !== 'function') return null
  return element.closest(DIALOG_SELECTOR) || null
}

/** 元素是否属于指定顶层编辑表面（弹窗）。无表面时返回 true。 */
export function isInEditSurface(element, surface) {
  if (!surface) return true
  if (!element || typeof element.closest !== 'function') return false
  return element.closest(DIALOG_SELECTOR) === surface
}

/** 从焦点节点向上找到真正的文本编辑宿主。 */
export function editorHost(element) {
  if (!element || element.nodeType !== 1) return null
  if (element.tagName === 'INPUT' || element.tagName === 'TEXTAREA') return element
  if (element.isContentEditable === true) {
    const host = element.closest('[contenteditable="true"]')
    return host && host.isContentEditable === true ? host : null
  }
  return null
}

export function isTextControl(element) {
  const host = editorHost(element)
  if (!host || host.disabled === true) return false
  if (host.tagName === 'INPUT') {
    return TEXT_INPUT_TYPES.has(String(host.type || 'text').toLowerCase())
  }
  return host.tagName === 'TEXTAREA' || host.isContentEditable === true
}

export function isReadOnly(element) {
  const host = editorHost(element)
  if (!host) return true
  if (host.tagName === 'INPUT' || host.tagName === 'TEXTAREA') return host.readOnly === true
  return host.isContentEditable !== true
}

/** 控件是否真的支持文本选区（number 等类型的 selectionStart 不是数字）。 */
export function supportsTextSelection(element) {
  const host = editorHost(element)
  if (!host) return false
  if (host.tagName === 'INPUT' || host.tagName === 'TEXTAREA') {
    return typeof host.selectionStart === 'number' && typeof host.selectionEnd === 'number'
  }
  return host.isContentEditable === true
}

function contentEditableRange(host) {
  if (typeof window === 'undefined' || typeof window.getSelection !== 'function') return null
  const selection = window.getSelection()
  if (!selection || !selection.rangeCount) return null
  const range = selection.getRangeAt(0)
  if (!host.contains(range.startContainer) || !host.contains(range.endContainer)) return null
  return range.cloneRange()
}

function readText(host) {
  if (host.tagName === 'INPUT' || host.tagName === 'TEXTAREA') return host.value ?? ''
  return host.textContent ?? ''
}

/** 当前可见的顶层弹窗；没有统一浮层时返回 null。 */
export function currentEditSurface() {
  return topVisibleDialog()
}

/**
 * 捕获上下文。返回 null 表示该目标此刻不可作为编辑命令目标。
 * surface 保存的是顶层弹窗元素本身，用来识别"目标在底层弹窗里"的情况。
 */
export function captureEditContext(target, scopeKey) {
  const host = editorHost(target)
  if (!host || !isTextControl(host) || !isVisible(host)) return null
  if (host.disabled === true || host.getAttribute?.('aria-disabled') === 'true') return null

  const surface = topVisibleDialog()
  const owner = ownerDialog(host)
  if (surface ? owner !== surface : owner !== null) return null

  if (host.tagName === 'INPUT' || host.tagName === 'TEXTAREA') {
    const selectable = typeof host.selectionStart === 'number' && typeof host.selectionEnd === 'number'
    const start = selectable ? host.selectionStart : 0
    const end = selectable ? host.selectionEnd : start
    return {
      target: host,
      scopeKey,
      surface,
      kind: host.tagName.toLowerCase(),
      readOnly: host.readOnly === true,
      selectable,
      value: host.value ?? '',
      selectionStart: start,
      selectionEnd: end,
      range: null,
    }
  }

  const range = contentEditableRange(host)
  return {
    target: host,
    scopeKey,
    surface,
    kind: 'contenteditable',
    readOnly: false,
    selectable: true,
    value: readText(host),
    selectionStart: range ? range.startOffset : 0,
    selectionEnd: range ? range.endOffset : 0,
    range,
  }
}

/** 同步命令的目标是否仍然有效（内容允许已经变化）。 */
export function isEditContextValid(context, scopeKey) {
  if (!context || context.scopeKey !== scopeKey) return false
  const host = context.target
  if (!host || editorHost(host) !== host || !isVisible(host)) return false
  if (host.disabled === true) return false

  const surface = topVisibleDialog()
  const owner = ownerDialog(host)
  if (surface ? owner !== surface : owner !== null) return false
  return true
}

/** 异步剪贴板返回后复核：目标、内容、选区必须与读剪贴板前一致。 */
export function isEditSnapshotUnchanged(before, after) {
  if (!before || !after) return false
  if (before.target !== after.target || before.kind !== after.kind) return false
  if (before.value !== after.value) return false
  if (before.kind === 'contenteditable') {
    if (!before.range || !after.range) return false
    return before.range.startContainer === after.range.startContainer
      && before.range.startOffset === after.range.startOffset
      && before.range.endContainer === after.range.endContainer
      && before.range.endOffset === after.range.endOffset
  }
  if (before.selectable !== after.selectable) return false
  return before.selectionStart === after.selectionStart && before.selectionEnd === after.selectionEnd
}

export function hasSelection(context) {
  if (!context || context.selectable === false) return false
  if (context.kind === 'contenteditable') return Boolean(context.range && !context.range.collapsed)
  return context.selectionEnd > context.selectionStart
}

export function selectedText(context) {
  if (!hasSelection(context)) return ''
  if (context.kind === 'contenteditable') return context.range.toString()
  return context.value.slice(context.selectionStart, context.selectionEnd)
}

/** 命令可用性只返回原因码，文案由组件翻译。 */
export function editCommandState(context, scopeKey) {
  const valid = isEditContextValid(context, scopeKey)
  const readOnly = valid && isReadOnly(context.target)
  const selectable = valid && supportsTextSelection(context.target)
  const selection = valid && selectable && hasSelection(context)
  const selectionReason = !selectable ? 'noSelectionSupport' : 'noSelection'
  return {
    copy: {
      disabled: !valid || !selection,
      reason: !valid ? 'noTarget' : (selection ? '' : selectionReason),
    },
    cut: {
      disabled: !valid || !selection || readOnly,
      reason: !valid ? 'noTarget' : (readOnly ? 'readOnly' : (selection ? '' : selectionReason)),
    },
    paste: { disabled: !valid || readOnly, reason: !valid ? 'noTarget' : (readOnly ? 'readOnly' : '') },
    selectAll: {
      disabled: !valid || !selectable,
      reason: !valid ? 'noTarget' : (selectable ? '' : 'noSelectionSupport'),
    },
    redo: { disabled: !valid || readOnly, reason: !valid ? 'noTarget' : (readOnly ? 'readOnly' : '') },
    undo: { disabled: !valid || readOnly, reason: !valid ? 'noTarget' : (readOnly ? 'readOnly' : '') },
  }
}

/** 把焦点和选区恢复到快照位置；返回是否成功。 */
export function focusEditContext(context) {
  if (!context || !isEditContextValid(context, context.scopeKey)) return false
  const host = context.target
  host.focus()
  if (context.kind === 'contenteditable') {
    if (!context.range) return true
    if (typeof window === 'undefined' || typeof window.getSelection !== 'function') return false
    const selection = window.getSelection()
    if (!selection) return false
    selection.removeAllRanges()
    selection.addRange(context.range)
    return true
  }
  // 真实能力判断：只有函数才能调用（旧实现用 Number.isInteger 判断函数对象，恒为 false）
  if (typeof host.setSelectionRange === 'function' && context.selectable !== false) {
    try {
      host.setSelectionRange(context.selectionStart, context.selectionEnd)
    } catch {
      return false
    }
  }
  return true
}
