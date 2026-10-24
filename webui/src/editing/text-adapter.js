/**
 * 文本编辑命令适配器：把六条命令落到真实 DOM 文本控件上。
 *
 * 规则（docs/specs/EDITING_COMMANDS.md）：
 * - ED-010 复制取当前选区；剪切必须"剪贴板写入成功后"才删除，失败保留原文；
 * - ED-011 粘贴只接收纯文本，替换选区或插入光标，空文本不改变字段；
 * - ED-012 异步读剪贴板返回后复核目标/内容/选区，失败中止并提示；
 * - ED-013 readOnly 允许复制/全选，拒绝剪切/粘贴/撤销/重做；
 * - ED-015 撤销/重做只走编辑器本地历史，并同步回 React 受控值。
 */

import {
  captureEditContext,
  focusEditContext,
  hasSelection,
  isEditContextValid,
  isEditSnapshotUnchanged,
  selectedText,
} from '../lib/editingContext.js'

export class EditingError extends Error {
  constructor(code, message, cause) {
    super(message || code)
    this.name = 'EditingError'
    this.code = code
    if (cause) this.cause = cause
  }
}

function clipboardApi() {
  if (typeof navigator === 'undefined' || !navigator) return null
  return navigator.clipboard || null
}

function exec(command, ...args) {
  if (typeof document === 'undefined' || typeof document.execCommand !== 'function') return false
  try {
    return document.execCommand(command, ...args) === true
  } catch {
    return false
  }
}

function captureFor(context, scopeKey) {
  // 运行时传入的是冻结快照包装：绝不能用 DOM 节点重新捕获当前选区。
  // 直接调用 text-adapter 的测试仍可传 DOM 节点，此时按实时上下文处理。
  if (context && context.frozen) {
    const frozen = context.frozen
    if (!frozen.target) throw new EditingError('staleTarget')
    const frozenContext = { ...frozen, scopeKey }
    if (!isEditContextValid(frozenContext, scopeKey)) {
      throw new EditingError('staleTarget')
    }
    return {
      target: frozen.target,
      scopeKey,
      surface: frozen.surface,
      kind: frozen.kind || 'input',
      readOnly: frozen.readOnly === true,
      selectable: frozen.selectable !== false,
      value: typeof frozen.value === 'string' ? frozen.value : '',
      selectionStart: Number.isFinite(frozen.selectionStart) ? frozen.selectionStart : 0,
      selectionEnd: Number.isFinite(frozen.selectionEnd) ? frozen.selectionEnd : 0,
      range: frozen.range || null,
      frozen: true,
      validate: context.validate,
    }
  }
  const next = captureEditContext(context, scopeKey)
  if (!next) throw new EditingError('staleTarget')
  return next
}

function ensureUnchanged(before, scopeKey) {
  // 运行时冻结快照带 validate：比较 scope/epoch/目标/编辑版本，
  // 即使值被改回原样、选区回到原位置也要拒绝过期的异步结果。
  if (typeof before.validate === 'function' && !before.validate()) {
    throw new EditingError('aborted')
  }
  if (!isEditContextValid(before, scopeKey)) throw new EditingError('staleTarget')
  const live = captureEditContext(before.target, scopeKey)
  if (!live) throw new EditingError('staleTarget')
  if (before.frozen && !isEditSnapshotUnchanged(before, live)) {
    throw new EditingError('aborted')
  }
  return before.frozen ? before : live
}

function refocus(context) {
  if (!focusEditContext(context)) throw new EditingError('staleTarget')
}

function textFieldValue(host) {
  return host && typeof host.value === 'string' ? host.value : ''
}

/**
 * 用原生 setter 绕过 React 的 value tracker 再派发 input 事件，
 * 让 execCommand('undo'|'redo') 后的 DOM 值与受控 React 状态保持一致。
 * 返回是否成功触发同步。
 */
export function syncControlledValue(host) {
  if (!host || typeof host.dispatchEvent !== 'function') return false
  try {
    if (host.tagName === 'INPUT' || host.tagName === 'TEXTAREA') {
      const view = typeof window !== 'undefined' ? window : null
      const ctor = view ? (host.tagName === 'INPUT' ? view.HTMLInputElement : view.HTMLTextAreaElement) : null
      const descriptor = ctor ? Object.getOwnPropertyDescriptor(ctor.prototype, 'value') : null
      if (descriptor && typeof descriptor.set === 'function') descriptor.set.call(host, host.value)
    }
    const view = typeof window !== 'undefined' ? window : null
    const event = view && typeof view.InputEvent === 'function'
      ? new view.InputEvent('input', { bubbles: true, inputType: 'historyUndo' })
      : (() => {
        if (typeof Event !== 'function') return null
        return new Event('input', { bubbles: true })
      })()
    if (!event) return false
    host.dispatchEvent(event)
    return true
  } catch {
    return false
  }
}

async function writeClipboard(text) {
  const clipboard = clipboardApi()
  if (!clipboard || typeof clipboard.writeText !== 'function') {
    throw new EditingError('clipboardUnavailable')
  }
  try {
    await clipboard.writeText(text)
  } catch (error) {
    throw new EditingError('clipboardDenied', undefined, error)
  }
}

export async function copyText(context, scopeKey) {
  const current = captureFor(context, scopeKey)
  if (!hasSelection(current)) throw new EditingError('noSelection')
  const text = selectedText(current)
  await writeClipboard(text)
  return { status: 'done', text }
}

export async function cutText(context, scopeKey) {
  const before = captureFor(context, scopeKey)
  if (before.readOnly) throw new EditingError('readOnly')
  if (!hasSelection(before)) throw new EditingError('noSelection')
  const text = selectedText(before)

  // 先写剪贴板：写入失败时原文必须完整保留
  await writeClipboard(text)

  const after = ensureUnchanged(before, scopeKey)
  refocus(after)
  if (!exec('delete')) throw new EditingError('execFailed')
  return { status: 'done', text }
}

export async function pasteText(context, scopeKey) {
  const before = captureFor(context, scopeKey)
  if (before.readOnly) throw new EditingError('readOnly')

  const clipboard = clipboardApi()
  if (!clipboard || typeof clipboard.readText !== 'function') {
    throw new EditingError('clipboardUnavailable')
  }
  let text = ''
  try {
    text = await clipboard.readText()
  } catch (error) {
    throw new EditingError('clipboardDenied', undefined, error)
  }
  if (typeof text !== 'string') throw new EditingError('clipboardFormat')
  if (!text) return { status: 'noop', text: '' }

  const after = ensureUnchanged(before, scopeKey)
  refocus(after)
  if (!exec('insertText', false, text)) throw new EditingError('execFailed')
  return { status: 'done', text }
}

export async function selectAllText(context, scopeKey) {
  const current = captureFor(context, scopeKey)
  refocus(current)
  if (current.kind === 'contenteditable') {
    if (typeof document === 'undefined' || typeof window === 'undefined') {
      throw new EditingError('execFailed')
    }
    const range = document.createRange()
    range.selectNodeContents(current.target)
    const selection = window.getSelection()
    if (!selection) throw new EditingError('execFailed')
    selection.removeAllRanges()
    selection.addRange(range)
    return { status: 'done' }
  }
  const host = current.target
  // 直接设置选区，不依赖 execCommand('selectAll')：它在异步回调里可能返回 true
  // 却因缺少用户激活而不改变选区，导致快捷键看似执行但字段仍无选中内容。
  if (typeof host.setSelectionRange === 'function' && current.selectable) {
    host.setSelectionRange(0, textFieldValue(host).length)
    const after = captureEditContext(host, scopeKey)
    if (after && after.selectionEnd > after.selectionStart) return { status: 'done' }
  }
  if (exec('selectAll')) return { status: 'done' }
  throw new EditingError('execFailed')
}

export async function undoText(context, scopeKey) {
  return runHistoryCommand('undo', context, scopeKey)
}

export async function redoText(context, scopeKey) {
  return runHistoryCommand('redo', context, scopeKey)
}

async function runHistoryCommand(command, context, scopeKey) {
  const current = captureFor(context, scopeKey)
  if (current.readOnly) throw new EditingError('readOnly')
  refocus(current)
  if (!exec(command)) throw new EditingError('execFailed')
  syncControlledValue(current.target)
  return { status: 'done' }
}
