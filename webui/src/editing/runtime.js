/**
 * 编辑命令运行时：把 DOM 文本上下文、对象列表上下文和命令服务粘在一起。
 *
 * 约束（docs/specs/EDITING_COMMANDS.md）：
 * - ED-001 弹窗文本 > 视图文本 > 对象列表；
 * - ED-002 菜单打开前冻结目标/选区，关闭弹窗、切工作空间后不回落到其它输入框；
 * - ED-005 IME 与 Escape 不交给命令服务处理；
 * - 快捷键只在有可执行命令时 preventDefault。
 */

import {
  captureEditContext,
  currentEditSurface,
  editorHost,
  hasSelection,
  isEditContextValid,
  isInEditSurface,
  isTextControl,
} from '../lib/editingContext.js'
import { createCommandService, detectPlatform, normalizeSnapshot } from './command-service.js'
import * as textAdapter from './text-adapter.js'

function emptyObjects() {
  return {
    domain: 'symbols',
    count: 0,
    total: 0,
    supported: false,
    writable: false,
    busy: false,
    pendingCutCount: 0,
    clipboard: null,
    history: { undoLabel: '', redoLabel: '', undoable: false, redoable: false },
  }
}

export function createEditingRuntime(options = {}) {
  let scopeKey = ''
  let epoch = 0
  let textTarget = null
  let textContext = null
  let objectProvider = null
  let objectDomain = null
  // 宿主其它写入（删除、分类、导入）在飞时同样冻结对象编辑命令（R7）。
  let externalObjectBusy = false
  let installed = false
  let onError = typeof options.onError === 'function' ? options.onError : null
  const versions = new WeakMap()
  const scopeListeners = new Set()

  function platform() {
    return options.platform || detectPlatform()
  }

  function bumpVersion(target) {
    if (!target) return
    versions.set(target, (versions.get(target) || 0) + 1)
  }

  function versionOf(target) {
    return target ? (versions.get(target) || 0) : 0
  }

  function readObjects() {
    if (!objectProvider || typeof objectProvider.getState !== 'function') return null
    try {
      const raw = objectProvider.getState()
      if (!raw) return null
      return { ...emptyObjects(), ...raw, busy: raw.busy === true || externalObjectBusy }
    } catch (error) {
      reportError(error)
      return null
    }
  }

  function currentText() {
    if (!textTarget) return null
    const live = captureEditContext(textTarget, scopeKey)
    if (!live) return null
    return {
      key: live.target,
      // 冻结快照必须能回到同一个 DOM 目标与同一段选区；异步命令只用这份数据执行。
      target: live.target,
      surface: live.surface,
      kind: live.kind,
      range: live.range,
      version: versionOf(live.target),
      valid: true,
      readOnly: live.readOnly,
      selectable: live.selectable,
      hasSelection: hasSelection(live),
      value: live.value,
      selectionStart: live.selectionStart,
      selectionEnd: live.selectionEnd,
    }
  }

  function activeElement() {
    if (typeof document === 'undefined' || !document) return null
    return document.activeElement || null
  }

  function inObjectList(element) {
    if (!element || typeof element.closest !== 'function') return false
    return Boolean(element.closest('[data-editing-domain]'))
  }

  /**
   * 实时快照：owner 判定（ED-001/002）。
   * - 顶层弹窗存在时只允许弹窗内的文本/列表；不能回落到背景视图（ED-001）。
   * - 焦点在文本控件里 → 文本；
   * - 焦点在对象列表里 → 对象列表（焦点进入列表时就已清掉旧文本目标）；
   * - 焦点在顶栏菜单/其它外壳上且还有有效的文本目标 → 文本（菜单命令作用于打开菜单前的输入框）；
   * - 其它情况有对象列表 → 对象列表。
   */
  function getSnapshot() {
    const text = currentText()
    const objects = readObjects()
    const active = activeElement()
    const surface = currentEditSurface()
    let owner = null
    if (surface) {
      // 弹窗存在时，只有同一弹窗内的目标可以接管命令；焦点落在弹窗按钮等
      // 非编辑元素上时保持无目标，绝不把命令透传给背景列表。
      if (text) owner = 'text'
      else if (objects && inObjectList(active) && isInEditSurface(active, surface)) owner = 'objects'
    } else if (inObjectList(active) && objects) {
      owner = 'objects'
    } else if (text) {
      owner = 'text'
    } else if (objects) {
      owner = 'objects'
    }
    return { owner, scopeKey, epoch, text, objects }
  }

  function reportError(error) {
    if (onError) {
      try {
        onError(error)
      } catch {
        // 报错回调自身失败不再抛出
      }
      return
    }
    if (typeof console !== 'undefined' && typeof console.error === 'function') console.error(error)
  }

  async function runTextCommand(commandId, snapshot) {
    const text = snapshot && snapshot.text
    const target = text && text.target
    settingsValid(target)
    const handler = textAdapter[commandId === 'selectAll' ? 'selectAllText' : commandId + 'Text']
    if (typeof handler !== 'function') {
      throw new textAdapter.EditingError('execFailed')
    }
    // handler 拿到冻结快照与复核回调：异步读取剪贴板返回后必须再次确认
    // scope/epoch/目标/编辑版本仍未变化，且执行时使用冻结的选区（ED-002/012/015）。
    const result = await handler({
      target,
      frozen: text,
      validate: () => frozenTextCurrent(snapshot),
    }, scopeKey)
    // 全选由命令自身改选区，浏览器随后才投递 selectionchange。先同步刷新
    // remembered 上下文，避免这条由命令自己造成的选区事件把紧接着的
    // 剪切/复制快照误判为“用户移动了光标”（R2 回归）。
    if (commandId === 'selectAll') rememberTextTarget(target)
    return result
  }

  function settingsValid(target) {
    if (!target) throw new textAdapter.EditingError('staleTarget')
    return target
  }

  function frozenTextCurrent(snapshot) {
    if (!snapshot || !snapshot.text || !snapshot.text.target) return false
    if ((snapshot.scopeKey || '') !== scopeKey) return false
    if ((snapshot.epoch || 0) !== epoch) return false
    const live = currentText()
    if (!live || live.target !== snapshot.text.target) return false
    if (live.version !== snapshot.text.version) return false
    if (live.readOnly !== snapshot.text.readOnly) return false
    return true
  }

  function objectHandler(commandId) {
    return async (snapshot) => {
      const handler = objectProvider && objectProvider.handlers && objectProvider.handlers[commandId]
      if (typeof handler !== 'function') {
        throw new textAdapter.EditingError('objectUnsupported')
      }
      return handler(snapshot)
    }
  }

  const handlers = {}
  for (const commandId of ['copy', 'cut', 'paste', 'selectAll', 'undo', 'redo']) {
    handlers[commandId] = (snapshot) => (
      snapshot.owner === 'objects' ? objectHandler(commandId)(snapshot) : runTextCommand(commandId, snapshot)
    )
  }

  const service = createCommandService({
    getSnapshot,
    handlers,
    platform: options.platform,
    onEvent: options.onEvent,
    // 转发到当前 onError，而不是创建时捕获的回调：App 在挂载后才设置提示。
    onError: (error, commandId) => {
      if (onError) onError(error, commandId)
    },
  })

  function rememberTextTarget(element) {
    if (!isTextControl(element)) return false
    const context = captureEditContext(element, scopeKey)
    if (!context) return false
    if (textTarget !== context.target) {
      textTarget = context.target
      epoch += 1
      notifyScope()
    }
    textContext = context
    if (!versions.has(textTarget)) versions.set(textTarget, 0)
    return true
  }

  function refreshRemembered() {
    if (!textTarget) return
    const live = captureEditContext(textTarget, scopeKey)
    if (live) {
      textContext = live
      return
    }
    if (!isEditContextValid(textContext, scopeKey)) {
      textTarget = null
      textContext = null
      epoch += 1
      notifyScope()
    }
  }

  function clearTextTarget() {
    if (!textTarget && !textContext) return
    textTarget = null
    textContext = null
    epoch += 1
    notifyScope()
  }

  function setScopeKey(next) {
    const value = typeof next === 'string' ? next : ''
    if (value === scopeKey) return
    scopeKey = value
    textTarget = null
    textContext = null
    epoch += 1
    notifyScope()
  }

  function subscribe(listener) {
    if (typeof listener !== 'function') return () => {}
    scopeListeners.add(listener)
    return () => scopeListeners.delete(listener)
  }

  function notifyScope() {
    for (const listener of [...scopeListeners]) {
      try {
        listener()
      } catch (error) {
        reportError(error)
      }
    }
  }

  function setObjectProvider(provider) {
    objectProvider = provider || null
    const nextDomain = provider && typeof provider.getState === 'function'
      ? (provider.getState() || {}).domain || null
      : null
    if (nextDomain !== objectDomain) {
      objectDomain = nextDomain
      epoch += 1
      notifyScope()
    }
  }

  function onFocusIn(event) {
    const target = event && event.target
    if (isTextControl(target)) {
      rememberTextTarget(target)
      return
    }
    // 焦点进了对象列表：文本上下文结束，命令改为作用于列表（ED-001/023）。
    if (inObjectList(target)) clearTextTarget()
  }

  function onSelectionChange() {
    const active = activeElement()
    if (!active) return
    if (textTarget && (active === textTarget || (textTarget.contains && textTarget.contains(active)))) {
      const previous = textContext
      const live = captureEditContext(textTarget, scopeKey)
      // 选区变化也是编辑状态变化：冻结快照里的版本必须随之推进，
      // 否则异步命令会把内容插到用户已经移动过的光标处（ED-002/012）。
      if (live && previous && (
        live.value !== previous.value
        || live.selectionStart !== previous.selectionStart
        || live.selectionEnd !== previous.selectionEnd
      )) {
        bumpVersion(live.target)
      }
      refreshRemembered()
    }
  }

  function onInput(event) {
    const host = editorHost(event && event.target)
    if (host) bumpVersion(host)
  }

  function onKeyDown(event) {
    if (!event || event.isComposing === true) return null
    if (typeof event.defaultPrevented === 'boolean' && event.defaultPrevented) return null
    return service.handleKeydown(event)
  }

  function install(target) {
    if (installed) return () => {}
    const doc = target && target.addEventListener ? target : (typeof document !== 'undefined' ? document : null)
    if (!doc) return () => {}
    doc.addEventListener('focusin', onFocusIn, true)
    doc.addEventListener('selectionchange', onSelectionChange, true)
    doc.addEventListener('input', onInput, true)
    doc.addEventListener('keydown', onKeyDown, true)
    installed = true
    return () => {
      doc.removeEventListener('focusin', onFocusIn, true)
      doc.removeEventListener('selectionchange', onSelectionChange, true)
      doc.removeEventListener('input', onInput, true)
      doc.removeEventListener('keydown', onKeyDown, true)
      installed = false
    }
  }

  return {
    install,
    subscribe,
    setScopeKey,
    getScopeKey: () => scopeKey,
    rememberTextTarget,
    clearTextTarget,
    setObjectProvider,
    setExternalBusy: (busy) => {
      externalObjectBusy = busy === true
      service.setExternalBusy(externalObjectBusy)
    },
    isWriteBusy: () => service.isWriteBusy(),
    isWriteUncertain: () => service.isWriteUncertain(),
    clearWriteUncertain: () => service.clearWriteUncertain(),
    setOnError: (fn) => {
      onError = typeof fn === 'function' ? fn : null
    },
    getSnapshot,
    // 菜单/右键只在冻结时取一次快照：owner 可显式指定（列表右键必须按对象上下文
    // 取状态），其余情况沿用焦点判定。
    freeze: (owner) => {
      const snapshot = getSnapshot()
      return normalizeSnapshot(owner && snapshot.owner !== owner ? { ...snapshot, owner } : snapshot)
    },
    getStates: (frozen) => service.getStates(frozen),
    getState: (commandId, frozen) => service.getState(commandId, frozen),
    execute: (commandId, frozen) => service.execute(commandId, frozen),
    handleKeydown: onKeyDown,
    bumpVersion,
    platform,
  }
}

/** 应用内单例：TopBar 注册文本目标，App 注册对象列表 provider。 */
export const editingRuntime = createEditingRuntime()
