import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import { Modal } from './Modals'
import { useOverlayLayer } from '../workbench/overlay'
import { compareVersions, normalizeVersion } from '../lib/version'
import { editingRuntime } from '../editing/runtime'
import { shortcutLabel } from '../editing/command-service'
import { REASON_KEYS } from '../editing/reasons'

const HOMEPAGE = 'https://github.com/xiaomaitangjiang/KiCad_Forge'
const LATEST_RELEASE_API = 'https://api.github.com/repos/xiaomaitangjiang/KiCad_Forge/releases/latest'

const MINIMIZE_ICON = (
  <svg viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2"><line x1="1" y1="5.5" x2="9" y2="5.5" /></svg>
)
const MAXIMIZE_ICON = (
  <svg viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2"><rect x="1.5" y="1.5" width="7" height="7" /></svg>
)
const RESTORE_ICON = (
  <svg viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2">
    <rect x="1" y="2.6" width="6.4" height="6.4" />
    <path d="M3 2.6V1h6v6H7.4" />
  </svg>
)
const CLOSE_ICON = (
  <svg viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.2"><path d="M1.5 1.5l7 7M8.5 1.5l-7 7" /></svg>
)

const EMPTY_STATES = {
  copy: { disabled: true, reason: 'noTarget' },
  cut: { disabled: true, reason: 'noTarget' },
  paste: { disabled: true, reason: 'noTarget' },
  selectAll: { disabled: true, reason: 'noTarget' },
  undo: { disabled: true, reason: 'noTarget' },
  redo: { disabled: true, reason: 'noTarget' },
}

/// 顶栏：KIFORGE 字标 + 编辑/帮助/关于菜单。
/// 编辑命令统一交给 editingRuntime：菜单打开前冻结目标与选区，
/// 快捷键、菜单、对象列表右键共用同一份可用状态（ED-001/002/003）。
export default function TopBar({ toastMsg, controller = null, editScopeKey = '' }) {
  const { t } = useTranslation()
  const [openMenu, setOpenMenu] = useState(null)
  const [frozenEdit, setFrozenEdit] = useState(null)
  const [showAbout, setShowAbout] = useState(false)
  const [appInfo, setAppInfo] = useState(null)
  const topRef = useRef(null)
  const [nativeWindow, setNativeWindow] = useState(false)
  const [maximized, setMaximized] = useState(false)
  // 菜单也是浮层：Escape 只由栈顶处理，确认框/弹窗打开时不会连带关掉菜单。
  const menuInStack = useOverlayLayer(() => setOpenMenu(null), Boolean(openMenu))

  // The native binding is injected before page load; retry briefly in case the
  // first React render lands before WebView2 exposes it.
  useEffect(() => {
    let attempts = 0
    let timer = null
    const check = () => {
      if (typeof window.kforge_window === 'function') setNativeWindow(true)
      else if (attempts++ < 20) timer = setTimeout(check, 100)
    }
    check()
    return () => { if (timer) clearTimeout(timer) }
  }, [])

  useEffect(() => {
    if (!nativeWindow) return undefined
    let alive = true
    const sync = () => {
      try {
        window.kforge_window('state')
          .then((result) => {
            if (alive && typeof result?.maximized === 'boolean') setMaximized(result.maximized)
          })
          .catch(() => {})
      } catch { /* binding unavailable */ }
    }
    sync()
    window.addEventListener('resize', sync)
    return () => {
      alive = false
      window.removeEventListener('resize', sync)
    }
  }, [nativeWindow])

  async function windowAction(action) {
    if (!nativeWindow) return
    try {
      const result = await window.kforge_window(action)
      if (typeof result?.maximized === 'boolean') setMaximized(result.maximized)
    } catch { /* window is closing or the binding is unavailable */ }
  }

  function onTopbarPointerDown(event) {
    if (!nativeWindow || event.button !== 0) return
    if (event.target.closest('button, input, a, .topbar-menu, .topbar-controls')) return
    window.kforge_window('drag').catch(() => {})
  }

  // 工作空间或中心页变化时 runtime 会进入新 epoch，旧快照自然失效。
  useEffect(() => {
    editingRuntime.setScopeKey(editScopeKey)
    setFrozenEdit(null)
  }, [editScopeKey])

  useEffect(() => {
    if (!openMenu) return undefined
    const onPointerDown = (event) => {
      if (!topRef.current?.contains(event.target)) setOpenMenu(null)
    }
    document.addEventListener('pointerdown', onPointerDown)
    return () => document.removeEventListener('pointerdown', onPointerDown)
  }, [openMenu])

  // 没有 OverlayProvider 的单独挂载场景（测试/旧入口）保留 document 回退。
  useEffect(() => {
    if (!openMenu || menuInStack) return undefined
    const onKeyDown = (event) => { if (event.key === 'Escape') setOpenMenu(null) }
    document.addEventListener('keydown', onKeyDown)
    return () => document.removeEventListener('keydown', onKeyDown)
  }, [openMenu, menuInStack])

  async function loadAppInfo() {
    if (appInfo) return appInfo
    const info = await api.appInfo()
    setAppInfo(info)
    return info
  }

  async function openLogs() {
    try {
      const result = await api.openLogDir()
      if (!result?.ok) throw new Error(result?.error || t('topbar.failedUnknown'))
    } catch (error) {
      toastMsg(t('topbar.openLogsFailed', { error: error.message }))
    }
  }

  async function openHomepage() {
    try {
      const result = await api.openUrl(HOMEPAGE)
      if (!result?.ok) throw new Error(result?.error || t('topbar.failedUnknown'))
    } catch (error) {
      toastMsg(t('topbar.openHomepageFailed', { error: error.message }))
    }
  }

  async function checkUpdates() {
    try {
      const info = await loadAppInfo()
      if (!info?.version || !normalizeVersion(info.version))
        throw new Error(t('topbar.versionUnknown'))
      const response = await fetch(LATEST_RELEASE_API, {
        headers: { Accept: 'application/vnd.github+json' },
        signal: AbortSignal.timeout(8000),
      })
      if (!response.ok) throw new Error('HTTP ' + response.status)
      const release = await response.json()
      const latest = String(release?.tag_name || '')
      if (!normalizeVersion(latest)) throw new Error(t('topbar.invalidRelease'))
      if (compareVersions(latest, info.version) > 0)
        toastMsg(t('topbar.updateAvailable', { version: latest }))
      else
        toastMsg(t('topbar.upToDate'))
    } catch (error) {
      toastMsg(t('topbar.updateFailed', { error: error.message }))
    }
  }

  /**
   * 打开菜单前冻结当前上下文：点击按钮会把焦点移走，冻结后命令仍作用于
   * 原来的输入框/对象列表（ED-002）。对象列表没有文本目标时同样会冻结。
   */
  function freezeForMenu() {
    if (typeof document !== 'undefined' && document.activeElement) {
      editingRuntime.rememberTextTarget(document.activeElement)
    }
    return editingRuntime.freeze()
  }

  function toggleMenu(menuId) {
    if (openMenu === menuId) {
      setOpenMenu(null)
      setFrozenEdit(null)
      return
    }
    setFrozenEdit(freezeForMenu())
    setOpenMenu(menuId)
  }

  const editStates = openMenu === 'edit' && frozenEdit
    ? editingRuntime.getStates(frozenEdit)
    : EMPTY_STATES
  const editObjects = frozenEdit?.objects || null

  const reasonText = (state) => (state.disabled && state.reason
    ? t(REASON_KEYS[state.reason] || 'topbar.editNoTarget')
    : undefined)

  function editLabel(commandId) {
    if (commandId === 'undo' && editObjects?.history?.undoLabel) {
      return t('topbar.undoWith', { action: editObjects.history.undoLabel })
    }
    if (commandId === 'redo' && editObjects?.history?.redoLabel) {
      return t('topbar.redoWith', { action: editObjects.history.redoLabel })
    }
    return t('topbar.' + commandId)
  }

  /** 命令结果 → 可见反馈（ED-006 要求失败可发现，而不是静默 false）。 */
  function reportEditResult(result) {
    if (!result || !result.status) return
    const { status, commandId, reason, error } = result
    if (status === 'disabled') {
      toastMsg(t(REASON_KEYS[reason] || 'topbar.editNoTarget'))
      return
    }
    if (status === 'stale') {
      toastMsg(t('topbar.editStaleTarget'))
      return
    }
    if (status !== 'failed') return
    const code = error?.code
    if (code === 'aborted') {
      const key = commandId === 'cut' ? 'topbar.cutAborted' : 'topbar.pasteAborted'
      toastMsg(t(key))
      return
    }
    if (code === 'clipboardUnavailable') {
      toastMsg(t('topbar.clipboardUnavailable'))
      return
    }
    if (code === 'clipboardDenied') {
      toastMsg(t('topbar.clipboardDenied', { error: error?.message || t('topbar.failedUnknown') }))
      return
    }
    if (code === 'clipboardEmpty') {
      toastMsg(t('topbar.clipboardNoEnvelope'))
      return
    }
    if (code === 'clipboardFormat') {
      toastMsg(t('topbar.clipboardBadEnvelope'))
      return
    }
    const commandName = t('topbar.' + commandId)
    toastMsg(t('topbar.editCommandFailed', { command: commandName }))
  }

  function runCommand(commandId) {
    const frozen = frozenEdit
    setOpenMenu(null)
    setFrozenEdit(null)
    editingRuntime.execute(commandId, frozen).then(reportEditResult).catch((error) => {
      toastMsg(error?.message || t('topbar.failedUnknown'))
    })
  }

  const editItems = [
    { id: 'copy', label: editLabel('copy'), shortcut: shortcutLabel('copy', editingRuntime.platform()),
      disabled: editStates.copy.disabled, reason: reasonText(editStates.copy) },
    { id: 'cut', label: editLabel('cut'), shortcut: shortcutLabel('cut', editingRuntime.platform()),
      disabled: editStates.cut.disabled, reason: reasonText(editStates.cut) },
    { id: 'paste', label: editLabel('paste'), shortcut: shortcutLabel('paste', editingRuntime.platform()),
      disabled: editStates.paste.disabled, reason: reasonText(editStates.paste) },
    { id: 'selectAll', label: editLabel('selectAll'), shortcut: shortcutLabel('selectAll', editingRuntime.platform()),
      disabled: editStates.selectAll.disabled, reason: reasonText(editStates.selectAll) },
    { id: 'undo', label: editLabel('undo'), shortcut: shortcutLabel('undo', editingRuntime.platform()),
      disabled: editStates.undo.disabled, reason: reasonText(editStates.undo) },
    { id: 'redo', label: editLabel('redo'), shortcut: shortcutLabel('redo', editingRuntime.platform()),
      disabled: editStates.redo.disabled, reason: reasonText(editStates.redo) },
  ].map((item) => ({ ...item, run: () => runCommand(item.id) }))

  const menus = [
    { id: 'edit', label: t('topbar.edit'), items: editItems },
    ...(controller ? [{
      id: 'view',
      label: t('topbar.view'),
      items: [
        { id: 'primarySidebar', label: t('topbar.primarySidebar'), shortcut: 'Ctrl+B',
          checked: Boolean(controller.layout.primarySidebar.visible), run: controller.togglePrimarySidebar },
        { id: 'auxiliarySidebar', label: t('topbar.auxiliarySidebar'), shortcut: 'Ctrl+Alt+B',
          checked: Boolean(controller.layout.auxiliarySidebar.visible), run: controller.toggleAuxiliarySidebar },
        { id: 'bottomPanel', label: t('topbar.bottomPanel'), shortcut: 'Ctrl+J',
          checked: Boolean(controller.layout.bottomPanel.visible), run: controller.toggleBottomPanel },
        { id: 'view-separator', separator: true },
        { id: 'resetLayout', label: t('topbar.resetLayout'), run: controller.resetLayout },
        ...(controller.saveState === 'error' ? [{
          id: 'retrySave', label: t('topbar.retrySave'), run: controller.retrySave,
        }] : []),
      ],
    }] : []),
    {
      id: 'help',
      label: t('topbar.help'),
      items: [
        { id: 'openLogs', label: t('topbar.openLogs'), run: openLogs },
      ],
    },
    {
      id: 'about',
      label: t('topbar.about'),
      items: [
        { id: 'aboutApp', label: t('topbar.aboutApp'), run: () => {
          setShowAbout(true)
          loadAppInfo().catch(() => {})
        } },
        { id: 'homepage', label: t('topbar.homepage'), run: openHomepage },
        { id: 'checkUpdates', label: t('topbar.checkUpdates'), run: checkUpdates },
      ],
    },
  ]

  function activate(item) {
    setOpenMenu(null)
    setFrozenEdit(null)
    Promise.resolve(item.run?.()).catch((error) => toastMsg(error.message))
  }

  return (
    <>
      <header className="topbar" ref={topRef} onPointerDown={onTopbarPointerDown}>
        <img className="topbar-logo" src="/kiforge-wordmark.png" alt="KiCad Forge" />
        <nav className="topbar-nav">
          {menus.map((menu) => (
            <div className="topbar-menu" key={menu.id}>
              <button type="button" className="topbar-trigger"
                aria-haspopup="menu" aria-expanded={openMenu === menu.id}
                onFocus={menu.id === 'edit' ? () => editingRuntime.rememberTextTarget(document.activeElement) : undefined}
                onPointerDown={menu.id === 'edit' ? () => editingRuntime.rememberTextTarget(document.activeElement) : undefined}
                onClick={() => toggleMenu(menu.id)}>
                {menu.label}
              </button>
              {openMenu === menu.id ? (
                <div className="topbar-dropdown" role="menu">
                  {menu.items.map((item) => (
                    item.separator ? (
                      <div className="topbar-separator" role="separator" key={item.id} />
                    ) : (
                      <button type="button" role="menuitem" className="topbar-item" key={item.id}
                        disabled={Boolean(item.disabled)}
                        title={item.reason}
                        aria-disabled={item.disabled ? true : undefined}
                        aria-checked={item.checked === undefined ? undefined : Boolean(item.checked)}
                        onClick={() => activate(item)}>
                        {item.checked === undefined ? null : (
                          <span className="topbar-check" aria-hidden="true">{item.checked ? '✓' : ''}</span>
                        )}
                        <span>{item.label}</span>
                        {item.shortcut ? <span className="topbar-shortcut">{item.shortcut}</span> : null}
                      </button>
                    )
                  ))}
                </div>
              ) : null}
            </div>
          ))}
        </nav>
        {nativeWindow ? (
          <div className="topbar-controls">
            <button type="button" className="topbar-control"
              title={t('topbar.minimize')} aria-label={t('topbar.minimize')}
              onClick={() => windowAction('minimize')}>{MINIMIZE_ICON}</button>
            <button type="button" className="topbar-control"
              title={maximized ? t('topbar.restore') : t('topbar.maximize')}
              aria-label={maximized ? t('topbar.restore') : t('topbar.maximize')}
              onClick={() => windowAction('maximize')}>
              {maximized ? RESTORE_ICON : MAXIMIZE_ICON}
            </button>
            <button type="button" className="topbar-control topbar-close"
              title={t('topbar.closeWindow')} aria-label={t('topbar.closeWindow')}
              onClick={() => windowAction('close')}>{CLOSE_ICON}</button>
          </div>
        ) : null}
      </header>

      {showAbout ? (
        <Modal title={t('topbar.aboutApp')} className="about-modal" onClose={() => setShowAbout(false)}>
          <img className="about-logo" src="/kiforge-wordmark.png" alt="KiCad Forge" />
          <p className="about-version">{t('topbar.version', { version: appInfo?.version || '—' })}</p>
          <p className="about-text">{t('topbar.aboutText')}</p>
          <p className="about-text">{t('topbar.license')}</p>
          {appInfo?.log_dir ? <p className="about-path">{appInfo.log_dir}</p> : null}
          <div className="about-actions">
            <button type="button" className="btn-ghost" onClick={openHomepage}>{t('topbar.homepage')}</button>
            <button type="button" className="btn-primary" onClick={() => setShowAbout(false)}>{t('topbar.close')}</button>
          </div>
        </Modal>
      ) : null}
    </>
  )
}
