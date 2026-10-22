import { useState, useEffect, useCallback, useMemo, useRef, useSyncExternalStore } from 'react'
import { useTranslation } from 'react-i18next'
import { setLoadingCallback, api } from './api'
import { useToast } from './hooks/useToast'
import { useDarkMode } from './hooks/useDarkMode'
import { useStatus, useSymbols, useLibraries, usePlugins, useIssues, useMatches, useSettings, useRules, useComponentLibraries } from './hooks/useData'
import { useComponentTypes } from './hooks/useComponentTypes'
import TopBar from './components/TopBar'
import Workbench from './workbench/Workbench'
import { useWorkbenchLayout } from './workbench/useWorkbenchLayout'
import {
  useCenterNavigation, serializeCenterState, CENTER_HOME, CENTER_SETTINGS, CENTER_TYPES, CENTER_MATCHING,
} from './workbench/useCenterNavigation'
import { OverlayProvider, useOverlayDepth } from './workbench/overlay'
import CenterGroups from './workbench/CenterGroups'
import Toolbar from './components/Toolbar'
import PasteTargetDialog from './components/PasteTargetDialog'
import SymbolTable from './components/SymbolTable'
import Inspector from './components/Inspector'
import { SettingsModal, RulesModal, PluginsModal, TypeModal, CreateLibraryModal, ImportingModal, ClassifyResultModal, MatchSuggestModal, DeleteLibraryModal, ConfirmDialog } from './components/Modals'
import TypeMatchingModal from './components/TypeMatchingModal'
import { committedRefreshOutcome, isMatchingReport } from './lib/typeMatching'
import { nextEditRefreshAttempt, planEditRefreshRetry, runEditRefreshFlow } from './lib/editRefresh'
import { createSymbolRefresher, switchWorkspaceQuery, selectAllQuery } from './lib/symbolRefresh'
import { startDesktopSession, statusCountText } from './lib/desktopSession'
import { appPluginRuntime } from './plugin-host/app-host'
import { editingRuntime } from './editing/runtime'
import { isUncertainWriteError } from './editing/command-service'
import { reasonLabel } from './editing/reasons'
import { ClipboardError, buildEnvelope, copyEnvelope, parseEnvelope, readClipboardText } from './editing/clipboard-model'
import { cutSignature, envelopeMatchesCut, moveDirectItems } from './editing/move-model'
import {
  clearSelection, emptySelection, extendSelection, normalizeSelection,
  removeFromSelection, selectAllIn, selectOnly, toggleSelection,
} from './editing/selection-model'
import { isProblemSymbol } from './lib/footprintStatus'
import { taskOverview } from './plugin-host/task-overview'
import { recoverTasks } from './plugin-host/task-recovery'
import { usePluginEntries } from './components/PluginSlot'
import { PluginEntry } from './plugin-host/slot-host'
import './App.css'
import './plugin-slots.css'

const API = '/api'

// 浮层（菜单/弹窗）存在时 Escape 归栈顶；没有浮层才清除待剪切标记（ED-005/044）。
function EscapeCutClearer({ active, onClear }) {
  const depth = useOverlayDepth()
  const activeRef = useRef(active)
  const depthRef = useRef(depth)
  const onClearRef = useRef(onClear)
  activeRef.current = active
  depthRef.current = depth
  onClearRef.current = onClear
  useEffect(() => {
    const handler = (event) => {
      if (event.key !== 'Escape' || event.defaultPrevented || event.isComposing || event.keyCode === 229) return
      if (depthRef.current > 0 || !activeRef.current) return
      onClearRef.current?.()
    }
    document.addEventListener('keydown', handler)
    return () => document.removeEventListener('keydown', handler)
  }, [])
  return null
}

export default function App() {
  const { t } = useTranslation()
  const { toast, toastMsg } = useToast()
  const { dark, toggleDark } = useDarkMode()

  // Data hooks
  const { status, loadStatus, loadStatusTracked, beginHeartbeatStatus, publishHeartbeatStatus } = useStatus()
  // 心跳回调放进 ref：即使 useStatus 因模型重建换了函数身份，也不重启心跳会话。
  const heartbeatCallbacksRef = useRef(null)
  heartbeatCallbacksRef.current = { beginHeartbeatStatus, publishHeartbeatStatus }
  // 全库统计与当前筛选结果分开：只有完整合法计数才显示数字，未知用 …，过期保留旧数字并说明。
  const libraryStatsKnown = status.known === true
  const libraryStatsText = t('toolbar.symbolsFootprints', {
    symbols: statusCountText(status, 'symbols'),
    footprints: statusCountText(status, 'footprints'),
  })
  const libraryStatsNotice = !libraryStatsKnown
    ? t('app.loadingSymbols')
    : (status.stale
      ? (status.reason === 'error' && status.error
        ? t('status.loadFailed', { error: status.error })
        : t('lcsc.refreshReason.status'))
      : '')
  const { symbols, symbolsError, symbolsLoading, loadSymbols, loadSymbolsTracked } = useSymbols()
  const { libraries, librariesError, loadLibraries, loadLibrariesTracked } = useLibraries()
  const { plugins, loadPlugins } = usePlugins()
  const { issues, loadIssues, loadIssuesTracked } = useIssues()
  const { matches, loadMatches } = useMatches()
  const { settings, setSettings, loadSettings, configReady } = useSettings()
  const { rules, loadRules } = useRules()
  const { compLibraries, loadCompLibraries } = useComponentLibraries()
  // 元件类型表：启动即加载，全应用共用一份快照（Sidebar / Inspector / SymbolTable / 类型管理）
  const { types: compTypes, typeMap, loading: compTypesLoading, error: compTypesError,
          stale: compTypesStale, canWrite: compTypesWritable,
          reload: loadCompTypes, mutate: mutateCompType } = useComponentTypes()
  // 布局持久化只存尺寸/显隐/活动视图，业务状态仍留在 App。
  const workbench = useWorkbenchLayout(settings, configReady)
  const layoutRecoveryNotified = useRef(false)

  // 损坏的布局快照已经按默认布局渲染；只提示一次，不把它当成业务数据写回。
  useEffect(() => {
    if (!workbench.recovered || layoutRecoveryNotified.current) return
    layoutRecoveryNotified.current = true
    toastMsg(t('workbench.layoutRecovered'))
  }, [workbench.recovered, toastMsg, t])

  // 提交后刷新的归属（N1）：用户查询/筛选/工作区/中心页每变一次就推进意图
  // 代次。刷新流程冻结发起时的代次，只有仍是最新意图时才允许发布列表、改
  // 查询/筛选、选中对象或清通知。noticeEpoch 单独记录“用户关闭通知”。
  const editRefreshIntentRef = useRef({
    intentEpoch: 0, workspaceId: '', route: '', noticeEpoch: 0, selectionEpoch: 0,
  })
  const suppressEditIntentRef = useRef(false)
  const markEditIntent = useCallback(() => {
    if (!suppressEditIntentRef.current) editRefreshIntentRef.current.intentEpoch += 1
  }, [])
  const markSelectionIntent = useCallback(() => {
    if (!suppressEditIntentRef.current) editRefreshIntentRef.current.selectionEpoch += 1
  }, [])

  // UI state
  const [filter, setFilterState] = useState('')
  const [search, setSearchState] = useState('')
  // 对象列表选择（ED-020..024）：只存 UUID；集合与详情对象分开。
  const [selection, setSelection] = useState(emptySelection)
  const [targetLib, setTargetLibState] = useState('')
  const [issueFilter, setIssueFilterState] = useState(false)
  // 对象粘贴/移动的目标与冲突确认（ED-031/032、ED-042）；粘贴只在前端持有
  // 待提交的剪贴板内容，移动只持有宿主 token 与直接项身份。
  const [pasteIntent, setPasteIntent] = useState(null)
  // 待移动意图（ED-040/044）：宿主 token + 直接项身份，源对象在移动提交前不动。
  const [pendingCut, setPendingCut] = useState(null)
  const [editHistory, setEditHistory] = useState({ canUndo: false, canRedo: false, undo: null, redo: null, entries: [] })
  // 提交后刷新结果（R6）：只有全部必要读取都发布并真正定位到对象才算成功；
  // 失败时保留旧数据与旧查询，只允许“重试读取”，绝不重发写请求。
  const [editRefresh, setEditRefresh] = useState(null)
  // 通知的归属以 ref 为准：所有写入都经过 editRefreshPresenter，渲染时不再
  // 用 state 回写 ref，避免异步流程刚设的 attempt 被一次旧渲染覆盖。
  const editRefreshRef = useRef(null)
  // 刷新编号只用于归属，与当前通知是否存在无关：清通知/失败/成功都不能让
  // 编号回退，否则在途的旧刷新会与后来者复用同一个编号。
  const editRefreshAttemptRef = useRef(0)

  // 查询状态（关键词 + 目标库）同时放在 state（渲染）和 ref（刷新读取）里。
  // 两个 setter 是唯一入口：改 UI 的同时同步 ref —— 只在 useEffect 里补 ref
  // 会漏掉“等待期间用户改了查询”的情况。
  const queryRef = useRef({ search: '', targetLib: '' })
  const setSearch = useCallback((value) => {
    markEditIntent()
    queryRef.current.search = value
    setSearchState(value)
  }, [markEditIntent])
  const setTargetLib = useCallback((value) => {
    markEditIntent()
    queryRef.current.targetLib = value
    setTargetLibState(value)
  }, [markEditIntent])
  const setFilter = useCallback((value) => {
    markEditIntent()
    setFilterState(value)
  }, [markEditIntent])
  const setIssueFilter = useCallback((value) => {
    markEditIntent()
    setIssueFilterState(value)
  }, [markEditIntent])

  // ---- Unified query state: keyword + library travel together ----
  // 稳定的刷新回调：调用时才从 ref 取最新查询，所有“操作完成后刷新”都用它。
  const refreshSymbols = useMemo(
    () => createSymbolRefresher(loadSymbols, () => queryRef.current),
    [loadSymbols])

  const refreshMatchingViews = useCallback(async () => {
    const query = queryRef.current
    const sources = ['symbols', 'status', 'libraries', 'issues', 'types']
      .map(source => `classifying.sources.${source}`)
    const results = await Promise.allSettled([
      loadSymbolsTracked(query.search, query.targetLib), loadStatusTracked(),
      loadLibrariesTracked(), loadIssuesTracked(), loadCompTypes(),
    ])
    return committedRefreshOutcome(results, sources)
  }, [loadSymbolsTracked, loadStatusTracked, loadLibrariesTracked, loadIssuesTracked, loadCompTypes])

  const [loading, setLoading] = useState(false)
  const [pluginsReady, setPluginsReady] = useState(false)
  const center = useCenterNavigation()
  const sidebarActivities = usePluginEntries('sidebar.panel')
  // 仅第三方插件仍使用 app.view 中心页（WP-009）；内置 LCSC/Git 已迁到侧栏。
  const viewEntries = usePluginEntries('app.view')
  const taskSnapshot = useSyncExternalStore(taskOverview.subscribe, taskOverview.getSnapshot, taskOverview.getSnapshot)
  const pruneCenter = center.prune

  // 内置插件（LCSC/Git）只出现在左侧活动栏与主侧栏，不再注册 app.view。
  // 水合与插件发现完成后按当前 app.view 贡献清理页签：历史遗留的
  // view:git-details / view:lcsc-details 被移除，第三方插件页保持可用，
  // 不重置用户其余分组布局。
  useEffect(() => {
    if (!center.hydrated || !pluginsReady) return
    pruneCenter(viewEntries.map((entry) => entry.options.id))
  }, [center.hydrated, pluginsReady, pruneCenter, viewEntries])

  // 第三方插件视图入口：从侧栏列表打开对应中心页。
  const openPluginView = useCallback((localId) => {
    const entry = viewEntries.find((item) => item.contribution?.localId === localId)
    if (entry) center.selectView(entry.options.id)
  }, [viewEntries, center])

  // 中心分组的水合/持久化：先等宿主布局就绪，再用同一份 v2 快照替换默认树。
  // 首次水合只记录基线，不把默认树写回，避免启动时覆盖用户已保存的分组。
  const centerSnapshot = useMemo(() => serializeCenterState(center), [center])
  const centerSnapshotRef = useRef(null)
  const hydrateCenter = center.hydrate
  const setCenterState = workbench.setCenterState
  useEffect(() => {
    if (!workbench.hydrated || center.hydrated) return
    hydrateCenter(workbench.layout.center)
  }, [workbench.hydrated, workbench.layout.center, center.hydrated, hydrateCenter])
  useEffect(() => {
    if (!center.hydrated) return
    if (centerSnapshotRef.current === null) {
      centerSnapshotRef.current = centerSnapshot
      return
    }
    if (centerSnapshotRef.current === centerSnapshot) return
    centerSnapshotRef.current = centerSnapshot
    setCenterState(center)
  }, [center.hydrated, centerSnapshot, setCenterState, center])

  // Modal visibility（设置/类型/匹配规则已迁到中心页，不再占独立弹窗）
  const [showRules, setShowRules] = useState(false)
  const [showPlugins, setShowPlugins] = useState(false)
  const [showPkgTypes, setShowPkgTypes] = useState(false)
  const [pkgTypes, setPkgTypes] = useState([])
  const [pendingDeleteLib, setPendingDeleteLib] = useState(null)  // DeleteLibraryModal pending

  // 确认弹窗（替代 window.confirm）：复用统一 Modal，进入浮层栈并做焦点恢复。
  const [confirmState, setConfirmState] = useState({
    open: false, message: '', title: '', confirmLabel: '', danger: true,
  })
  const confirmResolve = useRef(null)

  const showConfirm = useCallback((message, options = {}) => {
    return new Promise((resolve) => {
      confirmResolve.current = resolve
      setConfirmState({
        open: true,
        message,
        title: options.title || '',
        confirmLabel: options.confirmLabel || '',
        danger: options.danger !== false,
      })
    })
  }, [])

  // Two-step delete — the request carries the object kind so the right API is
  // used: 'symbol' → /api/libraries (unregister; file removal is opt-in),
  // 'component' → /api/component-libraries (registration only, never files).
  const confirmDeleteLib = useCallback(async (deleteFile) => {
    const req = pendingDeleteLib
    if (!req) return
    setPendingDeleteLib(null)
    const { kind, lib } = req
    // 只有被删的正好是当前选中的库才清目标库：关键词始终保留，
    // 删别的库不该把用户的查询一起清掉。
    const dropTarget = () => { if (queryRef.current.targetLib === lib.id) setTargetLib('') }
    if (kind === 'symbol') {
      const r = await api.deleteLibrary(lib.id, deleteFile)
      if (r?.ok) {
        toastMsg(deleteFile
          ? t('toast.deleted', { name: lib.name, extra: r.deleted_file ? ' (+file)' : '' })
          : t('toast.removed', { name: lib.name }))
        loadLibraries(); dropTarget(); refreshSymbols()
      } else toastMsg(r?.error || t('toast.failedUnknown'))
    } else {
      const r = await api.saveCompLibrary({ action: 'remove', id: lib.id })
      if (r?.ok) {
        toastMsg(t('toast.removed', { name: lib.name }))
        loadLibraries(); dropTarget(); refreshSymbols()
      } else toastMsg(r?.error || t('toast.failedUnknown'))
    }
  }, [pendingDeleteLib, t, toastMsg, loadLibraries, refreshSymbols, setTargetLib])

  const handleConfirmYes = useCallback(() => {
    confirmResolve.current?.(true)
    confirmResolve.current = null
    setConfirmState({ open: false, message: '', title: '', confirmLabel: '', danger: true })
  }, [])

  const handleConfirmNo = useCallback(() => {
    confirmResolve.current?.(false)
    confirmResolve.current = null
    setConfirmState({ open: false, message: '', title: '', confirmLabel: '', danger: true })
  }, [])

  // Loading callback
  useEffect(() => { setLoadingCallback(setLoading) }, [])

  // Workspace bootstrap: config (active workspace) + component library list
  // load once on mount — both hooks are lazy otherwise (SettingsModal-only)
  useEffect(() => { loadSettings(); loadCompLibraries() }, []) // eslint-disable-line react-hooks/exhaustive-deps

  // Active workspace — three distinct states:
  //   undefined → never chosen: default to the first workspace
  //   ''        → the user explicitly picked "All workspaces"
  //   <id>      → a concrete workspace (falls back if it was deleted)
  const activeLib = useMemo(() => {
    const configured = settings.active_component_library
    if (configured === undefined) return compLibraries[0]?.id || ''
    if (configured === '') return ''
    return compLibraries.some(c => c.id === configured) ? configured : (compLibraries[0]?.id || '')
  }, [settings.active_component_library, compLibraries])

  // 刷新归属里要比较的实时上下文。工作区/中心页变化由下面的 effect 推进代次；
  // 渲染时同步 ref，网络回调即使赶在 effect 前也能看到最新值。
  editRefreshIntentRef.current.workspaceId = activeLib
  editRefreshIntentRef.current.route = `${center.activeGroupId}:${center.active}`
  useEffect(() => {
    markEditIntent()
  }, [activeLib, center.activeGroupId, center.active, markEditIntent])

  // Libraries of the active workspace — symbols are filtered by their library ids.
  // A concrete workspace with no libraries shows an EMPTY list (never falls back
  // to symbols of other workspaces).
  const workspaceLibraryIds = useMemo(
    () => new Set(libraries.filter(l => l.component_library_id === activeLib).map(l => l.id)),
    [libraries, activeLib])
  const visibleSymbols = useMemo(
    () => (!activeLib ? symbols : symbols.filter(s => workspaceLibraryIds.has(s.library_id))),
    [symbols, activeLib, workspaceLibraryIds])

  // ---- 对象列表选择（ED-020..024）----
  // 结果顺序取当前搜索/工作空间/库/类型/问题筛选与排序下的已加载列表：
  // Shift 连续选择与全选都按这个顺序，与视口无关；筛选条件变化本身由
  // 各自入口清空集合，这里只负责把刷新后消失的 UUID 剔除。
  const resultSymbols = useMemo(() => {
    let list = visibleSymbols
    if (filter) list = list.filter(s => (s.type_id || s.type) === filter)
    if (issueFilter) list = list.filter(s => isProblemSymbol(s))
    return list
  }, [visibleSymbols, filter, issueFilter])
  const resultIds = useMemo(() => resultSymbols.map(s => s.id), [resultSymbols])
  const availableIds = useMemo(() => new Set(resultIds), [resultIds])

  useEffect(() => {
    setSelection(current => normalizeSelection(current, availableIds))
  }, [availableIds])

  const selected = useMemo(() => {
    if (!selection.activeId) return null
    return symbols.find(s => s.id === selection.activeId) || null
  }, [selection.activeId, symbols])

  // 粘贴/移动目标是已登记的符号库（/api/libraries），不是元件库包。
  // 只列出当前工作空间的库；无工作空间（"全部"）时退回全部已登记库。
  const pasteTargets = useMemo(() => {
    if (!activeLib) return libraries
    return libraries.filter(lib => lib.component_library_id === activeLib)
  }, [libraries, activeLib])

  const selectSymbol = useCallback((symbol, options = {}) => {
    if (!symbol?.id) return
    markSelectionIntent()
    const mode = options.mode || 'single'
    setSelection(current => {
      if (mode === 'toggle') return toggleSelection(current, symbol.id)
      if (mode === 'range') return extendSelection(current, symbol.id, resultIds)
      return selectOnly(symbol.id)
    })
  }, [markSelectionIntent, resultIds])

  const clearSymbolSelection = useCallback(() => {
    markSelectionIntent()
    setSelection(clearSelection())
  }, [markSelectionIntent])
  const selectAllLoaded = useCallback(() => {
    markSelectionIntent()
    setSelection(selectAllIn(resultIds))
  }, [markSelectionIntent, resultIds])

  // 工具栏「剪切」是否显示为可用：只按已加载对象与库锁判断；真正授权在宿主，
  // 这里不能把只读源库显示成可写（ED-013/030）。
  const selectionWritable = useMemo(() => {
    if (selection.ids.length === 0) return false
    const byId = new Map(symbols.map(symbol => [symbol.id, symbol]))
    return selection.ids.every((id) => {
      const symbol = byId.get(id)
      return Boolean(symbol) && symbol.locked !== true
    })
  }, [selection.ids, symbols])

  // 对象菜单/快捷键的统一结果提示（ED-006）：不可用给原因，失败按类型区分，
  // 成功与 noop 不打扰用户。
  const reportEditingResult = useCallback((result) => {
    if (!result || result.status === 'noop') return
    if (result.status === 'done') {
      if (result.operation === 'copy') toastMsg(t('topbar.copiedSymbols', { count: result.count ?? 0 }))
      return
    }
    if (result.status === 'stale') { toastMsg(t('topbar.editStaleTarget')); return }
    if (result.status === 'disabled') {
      const label = reasonLabel(result.reason, t)
      if (label) toastMsg(label)
      return
    }
    if (result.status !== 'failed') return
    const code = result.error?.code
    if (code === 'clipboardUnavailable') { toastMsg(t('topbar.clipboardUnavailable')); return }
    if (code === 'clipboardDenied') { toastMsg(t('topbar.clipboardDenied', { error: result.error?.message || '' })); return }
    if (code === 'aborted') { toastMsg(t(result.command === 'cut' ? 'topbar.cutAborted' : 'topbar.pasteAborted')); return }
    if (code === 'clipboardTooLarge') { toastMsg(t('topbar.clipboardTooLarge')); return }
    if (code === 'clipboardEmpty') { toastMsg(t('topbar.clipboardNoEnvelope')); return }
    if (code === 'clipboardFormat') { toastMsg(t('topbar.clipboardBadEnvelope')); return }
    if (code === 'target_not_found' || code === 'target_not_writable') { toastMsg(t('topbar.pasteNoTarget')); return }
    if (code === 'invalid_token') { toastMsg(t('topbar.cutExpired')); return }
    if (code === 'source_changed' || code === 'conflict') { toastMsg(t('topbar.sourceChanged')); return }
    if (code === 'operation_conflict') { toastMsg(t('topbar.operationConflict')); return }
    if (code === 'history_empty') { toastMsg(t('topbar.undoEmpty')); return }
    if (code === 'history_conflict') { toastMsg(t('topbar.editConflict')); return }
    toastMsg(t('topbar.editCommandFailed', { command: result.command || '' }))
  }, [t, toastMsg])

  // ---- 编辑命令运行时（ED-001/002/003）----
  // 全局只装一个 keydown 分发器；文本目标由 focusin/selectionchange 记录。
  // 对象列表上下文通过 provider 暴露给命令服务，默认不支持写入（宿主服务未接入前）。
  const editingObjectStateRef = useRef(null)
  const selectionCommandsRef = useRef({})
  const selectionRef = useRef(selection)
  selectionRef.current = selection
  const pendingCutRef = useRef(pendingCut)
  pendingCutRef.current = pendingCut
  const clearPendingCut = useCallback(() => {
    const cut = pendingCutRef.current
    pendingCutRef.current = null
    setPendingCut(null)
    if (cut?.token) api.clearCut(cut.token).catch(() => {})
    toastMsg(t('topbar.cutCleared'))
  }, [t, toastMsg])
  const refreshEditHistory = useCallback(async () => {
    try {
      const history = await api.history()
      if (!history?.ok) {
        // 读取失败保持旧快照：不能把「不知道」显示成可撤销/可重做。
        return { status: 'failed', error: history?.error || '' }
      }
      setEditHistory({
        canUndo: history.can_undo === true,
        canRedo: history.can_redo === true,
        undo: history.undo || null,
        redo: history.redo || null,
        entries: Array.isArray(history.entries) ? history.entries : [],
      })
      return { status: 'published' }
    } catch (error) {
      return { status: 'failed', error: error?.message || String(error) }
    }
  }, [])
  useEffect(() => { refreshEditHistory() }, [refreshEditHistory])
  const historyLabel = useCallback((entry) => {
    if (!entry) return ''
    const key = entry.kind === 'move' ? 'topbar.historyMove' : 'topbar.historyPaste'
    return t(key, { count: entry.count ?? 0 })
  }, [t])

  // 提交后的只读刷新（R6/N1）：runEditRefreshFlow 冻结发起时的查询、工作区、
  // 中心页与通知代次；用户随后改搜索/换库/换工作区/第二次编辑都会推进代次，
  // 旧流程只清自己的通知，不再发布旧列表或覆盖新查询。失败时保留旧数据与
  // 旧查询，重试只读；重试前重新核对当前意图，绝不重发粘贴/移动/撤销/重做。
  const readEditRefreshContext = useCallback(() => ({
    attempt: editRefreshRef.current?.attempt ?? 0,
    intentEpoch: editRefreshIntentRef.current.intentEpoch,
    workspaceId: editRefreshIntentRef.current.workspaceId,
    route: editRefreshIntentRef.current.route,
    noticeEpoch: editRefreshIntentRef.current.noticeEpoch,
    selectionEpoch: editRefreshIntentRef.current.selectionEpoch,
  }), [])
  const editRefreshPresenter = useMemo(() => ({
    pending(notice) {
      editRefreshRef.current = notice
      setEditRefresh(notice)
    },
    failed(notice) {
      editRefreshRef.current = notice
      setEditRefresh(notice)
    },
    clear(attempt) {
      // 只清自己这张卡片：另一个刷新的失败/进行中通知不能被旧流程抹掉。
      if (editRefreshRef.current?.attempt !== attempt) return
      editRefreshRef.current = null
      setEditRefresh(null)
    },
    applyQuery(targetLibraryId) {
      // 这是刷新自己的受控迁移，不是用户输入；不能推进意图代次。
      suppressEditIntentRef.current = true
      try {
        setSearch('')
        setTargetLib(targetLibraryId || '')
        setFilter('')
        setIssueFilter(false)
      } finally {
        suppressEditIntentRef.current = false
      }
    },
    select(ids) {
      setSelection({ ids, activeId: ids[0], anchorId: ids[0] })
    },
  }), [setFilter, setIssueFilter, setSearch, setTargetLib])
  const runEditRefresh = useCallback(async (request) => {
    const attempt = editRefreshAttemptRef.current = nextEditRefreshAttempt(editRefreshAttemptRef.current)
    return runEditRefreshFlow({
      request,
      attempt,
      readContext: readEditRefreshContext,
      getNoticeAttempt: () => editRefreshRef.current?.attempt ?? null,
      reads: {
        // 符号列表仍通过 request model 发布，保留最新 epoch 的 latest-wins；
        // 探测只用于判断读回是否可用，发布前后都会复核归属。
        probe: (search, targetLib) => api.symbols(search, targetLib),
        libraries: loadLibrariesTracked,
        status: loadStatusTracked,
        history: refreshEditHistory,
        publish: loadSymbolsTracked,
      },
      present: editRefreshPresenter,
      toast: toastMsg,
      translate: (key, params) => tRef.current(key, params),
    })
  }, [editRefreshPresenter, loadLibrariesTracked, loadStatusTracked, loadSymbolsTracked,
    readEditRefreshContext, refreshEditHistory, toastMsg])
  const retryEditRefresh = useCallback(() => {
    const current = editRefreshRef.current
    if (!current || current.status === 'refreshing') return
    const plan = planEditRefreshRetry(current, {
      query: { ...queryRef.current },
      context: readEditRefreshContext(),
    })
    if (!plan) return
    return runEditRefresh(plan.request)
  }, [readEditRefreshContext, runEditRefresh])
  const dismissEditRefresh = useCallback(() => {
    // 关闭即作废旧刷新：晚到的失败提示/成功 toast 不能再出现。
    editRefreshIntentRef.current.noticeEpoch += 1
    editRefreshRef.current = null
    setEditRefresh(null)
  }, [])
  const afterHistoryApply = useCallback(async (outcome) => {
    // 撤销/重做会改变文件与 DB：刷新视图是只读重取，失败不回滚成功结果。
    pendingCutRef.current = null
    setPendingCut(null)
    const ids = (Array.isArray(outcome?.restored) ? outcome.restored : [])
      .map(item => item?.uuid).filter(Boolean)
    await runEditRefresh({
      kind: outcome?.kind || 'history',
      ids,
      query: { ...queryRef.current },
      targetLibraryId: '',
      locate: false,
      requireAll: false,
      successKey: null,
    })
  }, [runEditRefresh])
  const afterHistoryApplyRef = useRef(afterHistoryApply)
  afterHistoryApplyRef.current = afterHistoryApply
  const tRef = useRef(t)
  tRef.current = t
  // undo/redo 的网络失败可能已经提交：保持写闸门，重新读取历史快照后才解除。
  const editReconcileRef = useRef(null)
  const reconcileEditWrite = useCallback(async () => {
    if (editReconcileRef.current) return editReconcileRef.current
    const task = (async () => {
      const query = { ...queryRef.current }
      const results = await Promise.allSettled([
        refreshEditHistory(),
        loadLibrariesTracked(),
        loadSymbolsTracked(query.search, query.targetLib),
      ])
      const history = results[0]
      if (history.status === 'fulfilled' && history.value?.status === 'published') {
        editingRuntime.clearWriteUncertain()
        toastMsg(tRef.current('topbar.editUnknownRecovered'))
        return true
      }
      toastMsg(tRef.current('topbar.editUnknown'))
      return false
    })().finally(() => { editReconcileRef.current = null })
    editReconcileRef.current = task
    return task
  }, [refreshEditHistory, loadLibrariesTracked, loadSymbolsTracked, toastMsg])
  const reconcileEditWriteRef = useRef(reconcileEditWrite)
  reconcileEditWriteRef.current = reconcileEditWrite
  // 删除/分类等宿主写入与编辑命令共用同一个冻结闸门（R7）：不在写进行中
  // 接受另一端发起的写，也不让未知结果悄悄放开下一次编辑。
  const appObjectWriteRef = useRef(false)
  const beginAppObjectWrite = useCallback(() => {
    if (appObjectWriteRef.current || editingRuntime.isWriteBusy()) {
      toastMsg(tRef.current('topbar.editBusy'))
      return false
    }
    if (editingRuntime.isWriteUncertain()) {
      toastMsg(tRef.current('topbar.editUnknown'))
      void reconcileEditWriteRef.current?.()
      return false
    }
    appObjectWriteRef.current = true
    editingRuntime.setExternalBusy(true)
    return true
  }, [toastMsg])
  const endAppObjectWrite = useCallback(() => {
    appObjectWriteRef.current = false
    editingRuntime.setExternalBusy(false)
  }, [])
  selectionCommandsRef.current = { selectAll: selectAllLoaded }
  editingObjectStateRef.current = {
    domain: 'symbols',
    count: selection.ids.length,
    total: resultSymbols.length,
    // 首期对象命令只作用于首页符号列表：切到设置/类型/插件页或打开粘贴对话框后，
    // 不能让快捷键在不可见的列表背后执行命令（ED-001/002）。
    supported: center.active === CENTER_HOME && !pasteIntent,
    // 宿主会话历史（S4）：可用状态与动作标签由后端快照提供，不做前端本地栈。
    unsupported: [],
    writable: selectionWritable,
    busy: false,
    pendingCutCount: pendingCut?.ids?.length || 0,
    // 不轮询系统剪贴板（ED 资源约束）：这里只表达「可以尝试粘贴」，
    // 真正读取与格式校验发生在用户触发 execute 的边界。
    clipboard: { kind: 'unknown' },
    history: {
      undoLabel: historyLabel(editHistory.undo),
      redoLabel: historyLabel(editHistory.redo),
      undoable: editHistory.canUndo,
      redoable: editHistory.canRedo,
    },
  }

  useEffect(() => {
    editingRuntime.setOnError((error) => {
      toastMsg(error?.message || String(error))
      // 结果未知的写入不能直接放开下一次写：先只读核对历史，成功才解冻。
      if (isUncertainWriteError(error)) void reconcileEditWriteRef.current?.()
    })
    return editingRuntime.install()
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    editingRuntime.setObjectProvider({
      getState: () => editingObjectStateRef.current,
      handlers: {
        selectAll: () => {
          selectionCommandsRef.current.selectAll?.()
          return { operation: 'selectAll' }
        },
        copy: async () => {
          // ED-030：只读源也允许复制。完整内容由宿主从源文件导出，前端只负责
          // 校验容量并写入带版本/种类的 envelope；不写 DB、不写源文件。
          const ids = selectionRef.current?.ids || []
          const exported = await api.exportSymbols(ids)
          const items = Array.isArray(exported?.items) ? exported.items : []
          if (items.length === 0) throw new Error('export returned no symbols')
          const summary = tRef.current('topbar.clipboardSummary', { count: items.length })
          const envelope = buildEnvelope(items, { summary })
          await copyEnvelope(envelope)
          return { operation: 'copy', count: items.length }
        },
        cut: async () => {
          // ED-040：剪切只建立宿主会话内的移动意图，不删源、不改文件/DB。
          const ids = selectionRef.current?.ids || []
          const previous = pendingCutRef.current
          if (previous?.token) api.clearCut(previous.token).catch(() => {})
          let cut
          try {
            cut = await api.cutSymbols(ids)
          } catch (cause) {
            // 命令服务只认 code；HTTP 层把服务端错误码放在 payload.code。
            cause.code = cause?.payload?.code || 'cut_failed'
            throw cause
          }
          if (!cut?.ok) {
            const failure = new Error(cut?.error || tRef.current('topbar.cutFailed', { error: '' }))
            failure.code = cut?.code
            throw failure
          }
          const items = Array.isArray(cut.items) ? cut.items : []
          const summary = cut.summary || tRef.current('topbar.clipboardSummary', { count: items.length })
          const envelope = buildEnvelope(items, { summary })
          try {
            await copyEnvelope(envelope)
          } catch (cause) {
            // 剪贴板写失败时释放 token：源对象仍完整，界面上不留假待移动标记。
            api.clearCut(cut.token).catch(() => {})
            throw cause
          }
          const intent = {
            token: cut.token,
            items,
            signature: cutSignature(items),
            ids: moveDirectItems(items).map(item => item.uuid),
          }
          pendingCutRef.current = intent
          setPendingCut(intent)
          return { operation: 'cut', count: intent.ids.length }
        },
        paste: async () => {
          // 在用户事件里才读剪贴板；无前缀的普通文本不是符号内容。
          const text = await readClipboardText()
          if (!text) throw new ClipboardError('clipboardEmpty', 'clipboard is empty')
          const envelope = parseEnvelope(text)
          if (!envelope) throw new ClipboardError('clipboardFormat', 'clipboard has no symbol payload')
          const cut = pendingCutRef.current
          if (cut && envelopeMatchesCut(envelope, cut)) {
            // 剪贴板仍是剪切时那份 → 走移动；真正删除源由宿主在提交时一次完成。
            setPasteIntent({
              mode: 'move', token: cut.token, items: cut.items,
              summary: tRef.current('topbar.moveSummary', { count: cut.ids.length }),
              sourceLibraryId: cut.items.find(item => !item.dependency)?.library_id || '',
            })
            return { operation: 'paste', deferred: true, count: cut.ids.length }
          }
          if (cut) {
            // 剪贴板被换掉：源对象保留，本地待移动标记作废（ED-044）。
            pendingCutRef.current = null
            setPendingCut(null)
            api.clearCut(cut.token).catch(() => {})
          }
          setPasteIntent({ items: envelope.items, summary: envelope.summary })
          // 目标与冲突由对话框确认，命令本身先返回 deferred，不重复提交。
          return { operation: 'paste', deferred: true, count: envelope.items.length }
        },
        undo: async () => {
          let outcome
          try {
            outcome = await api.undoEdit()
          } catch (cause) {
            cause.code = cause?.payload?.code || 'history_failed'
            throw cause
          }
          await afterHistoryApplyRef.current(outcome)
          return { operation: 'undo', count: outcome?.count ?? 0 }
        },
        redo: async () => {
          let outcome
          try {
            outcome = await api.redoEdit()
          } catch (cause) {
            cause.code = cause?.payload?.code || 'history_failed'
            throw cause
          }
          await afterHistoryApplyRef.current(outcome)
          return { operation: 'redo', count: outcome?.count ?? 0 }
        },
      },
    })
    return () => editingRuntime.setObjectProvider(null)
  }, [])

  // Frontend plugins and the legacy manifest adapter share the same outlets.
  useEffect(() => {
    const requirePublished = async (read, source) => {
      const outcome = committedRefreshOutcome(await Promise.allSettled([read]), [source])
      if (outcome.status !== 'published') {
        throw new Error(outcome.details[0]?.error || `Refresh ${source} ${outcome.status}`)
      }
    }
    appPluginRuntime.setRefresh({
      symbols: () => {
        const query = queryRef.current
        return requirePublished(loadSymbolsTracked(query.search, query.targetLib), 'symbols')
      },
      libraries: () => requirePublished(loadLibrariesTracked(), 'libraries'),
      status: () => requirePublished(loadStatusTracked(), 'status'),
    })
  }, [loadSymbolsTracked, loadLibrariesTracked, loadStatusTracked])

  useEffect(() => {
    appPluginRuntime.setScope({
      workspaceId: activeLib || null,
      selectedSymbolId: selected?.id ?? null,
      libraryFilterId: targetLib || null,
    })
  }, [activeLib, selected, targetLib])

  useEffect(() => {
    let alive = true
    appPluginRuntime.start().then(() => {
      if (alive) setPluginsReady(true)
    }).catch((error) => {
      appPluginRuntime.host.diagnostics.push({
        code: 'app/plugin-load-failed',
        severity: 'error',
        message: error?.message || String(error),
      })
    })
    return () => { alive = false }
  }, [])

  // 刷新后首次打开底部面板时恢复宿主仍在执行/已结束的任务。只读一次插件列表，
  // 不重建任务、不扫描全部插件；终态作为摘要，非终态交给同一任务的观察适配器。
  useEffect(() => {
    if (!pluginsReady || !workbench.layout.bottomPanel.visible) return
    const pluginIds = appPluginRuntime.loader.getSnapshot()
      .filter(plugin => plugin.enabled)
      .map(plugin => plugin.id)
    void recoverTasks(pluginIds)
  }, [pluginsReady, workbench.layout.bottomPanel.visible])

  async function switchWorkspace(id) {
    center.ensureHomeOpen()
    setSettings(prev => ({ ...prev, active_component_library: id }))
    // Context change → reset the whole query state (selection, keyword, lib)
    clearSymbolSelection(); setFilter(''); setIssueFilter(false); loadLibraries()
    // 起始：同步清空 UI+ref 并取一次空查询；保存完成后按“最新” ref 刷新，
    // 用户在等待期间输入的新查询不会被这次空查询覆盖。
    await switchWorkspaceQuery({
      resetQuery: (q, lib) => { setSearch(q); setTargetLib(lib) },
      loadFresh: (q, lib) => loadSymbols(q, lib),
      save: () => api.saveConfig({ active_component_library: id }),
      refresh: refreshSymbols,
    })
  }

  async function selectLibrary(libId) {
    center.ensureHomeOpen()
    setFilter(''); clearSymbolSelection(); setTargetLib(libId)
    loadSymbols(queryRef.current.search, libId)
  }

  // Heartbeat: keeps the local backend alive while the UI is open.
  // 递归 setTimeout（后台标签页的 setInterval 会被节流）；卸载时 clearTimeout +
  // abort 在途请求，StrictMode 的挂载-清理-重挂不会留下旧 timer 或旧请求。
  useEffect(() => {
    const session = startDesktopSession({
      fetchFn: fetch,
      setTimer: setTimeout,
      clearTimer: clearTimeout,
      statusUrl: `${API}/status`,
      issueStatusToken: () => heartbeatCallbacksRef.current?.beginHeartbeatStatus(),
      onStatus: (event) => heartbeatCallbacksRef.current?.publishHeartbeatStatus(event),
    })
    return () => session.stop()
  }, [])

  // Classify / check / match
  const [classifyResult, setClassifyResult] = useState(null)
  const [classifyRefresh, setClassifyRefresh] = useState(null)
  const classifyLock = useRef(false)
  const classifyReadLock = useRef(false)
  const classifyAwaitingRefresh = useRef(false)
  const classifyLastReport = useRef(null)
  const [matchList, setMatchList] = useState(null)

  const refreshClassifyViews = useCallback(async () => {
    if (classifyReadLock.current || !classifyAwaitingRefresh.current) return
    classifyReadLock.current = true
    setClassifyRefresh({ status: 'pending' })
    try {
      const result = await refreshMatchingViews()
      setClassifyRefresh(result)
      classifyAwaitingRefresh.current = result.status !== 'published'
    } catch (cause) {
      setClassifyRefresh({ status: 'failed', details: [{ source: 'classifying.sources.interface', error: cause.message }] })
    } finally { classifyReadLock.current = false }
  }, [refreshMatchingViews])

  async function classify() {
    // Toolbar 与 Inspector 共用同步锁；待刷新时重新打开已提交报告，不再发写请求。
    if (classifyLock.current || classifyReadLock.current) return
    if (classifyAwaitingRefresh.current && classifyLastReport.current) {
      setClassifyResult(classifyLastReport.current)
      return
    }
    if (!beginAppObjectWrite()) return
    classifyLock.current = true
    setClassifyResult(null)
    setClassifyRefresh(null)
    toastMsg(t('toast.classifying'))
    try {
      let r
      try { r = await api.classify() }
      catch (cause) {
        if (isMatchingReport(cause.payload)) r = cause.payload
        else throw cause
      }
      if (!isMatchingReport(r)) {
        if (!r?.summary && (r?.ok === false || typeof r?.error === 'string')) {
          toastMsg(t('toast.failed', { error: r.error || t('toast.failedUnknown') }))
          return
        }
        throw new Error(t('classifying.invalidResponse'))
      }
      // 有可信报告就保留真实提交计数，即使 ok:false 或 HTTP 非2xx。
      classifyLastReport.current = r
      classifyAwaitingRefresh.current = true
      setClassifyResult(r)
      await refreshClassifyViews()
    } catch (cause) {
      const rejected = cause.kind === 'http' && !cause.payload?.summary
        && cause.payload?.ok === false && typeof cause.payload.error === 'string'
      toastMsg(rejected ? t('toast.failed', { error: cause.payload.error })
        : t('classifying.resultUnknown', { error: cause.message }))
    } finally {
      classifyLock.current = false
      endAppObjectWrite()
    }
  }
  function toggleIssues() {
    setIssueFilter(prev => !prev)
    setFilter('')
    clearSymbolSelection()
  }

  async function checkCorrespondence() {
    // 用本次请求的返回值，不要读刷新前的 issues 状态
    const list = await loadIssues()
    if (!list) { toastMsg(t('toast.loadFailed')); return }
    toastMsg(list.length === 0 ? t('toast.noIssues') : t('toast.issuesFound', { count: list.length }))
  }
  async function autoMatch() {
    // useRequest.load returns the fresh data — the `matches` state is stale here
    const list = await loadMatches() || []
    setMatchList(list)
  }

  // Library operations
  const [showCreateLib, setShowCreateLib] = useState(false)
  async function deleteSymbol(sym) {
    if (!(await showConfirm(t('confirm.deleteSymbol', { name: sym.name })))) return
    if (!beginAppObjectWrite()) return
    try {
      const r = await api.deleteSymbol(sym.id)
      if (r?.ok) {
        toastMsg(t('toast.deleted', { name: '', extra: '' }))
        setSelection(current => removeFromSelection(current, sym.id))
        refreshSymbols(); loadStatus(); refreshEditHistory()
      }
      else toastMsg(r?.error || t('toast.failedUnknown'))
    } catch (cause) {
      toastMsg(cause?.message || t('toast.failedUnknown'))
    } finally { endAppObjectWrite() }
  }

  // Type management（封装类型仍是独立的一份，行为不变）
  async function loadPkgTypes() { const d = await api.pkgTypes(); if (d) setPkgTypes(d) }
  async function manageType(url, action, name, extra) {
    const r = await api.manageType(url, action, name, extra)
    if (r?.ok) { toastMsg(t(action === 'add' ? 'toast.added' : 'toast.removed', { name })); return true }
    // 被引用的类型不能删：返回引用数量
    toastMsg(r?.references ? t('toast.typeInUse', { count: r.references }) : (r?.error || 'Failed'))
    return false
  }

  // ---- Sidebar 六组意图：状态、加载与筛选都留在 App，区块只发意图 ----
  // “All” 保留关键词（那是用户输入）：清筛选与选中，只同步清 targetLib 并按最新
  // ref 刷新一次（原先区块里的重复 clear 合并到这里，不重复发 GET）
  function selectAllSymbols() {
    center.ensureHomeOpen()
    setFilter(''); setIssueFilter(false); clearSymbolSelection()
    selectAllQuery({ resetTargetLib: setTargetLib, refresh: refreshSymbols })
  }
  function selectIssues() { center.ensureHomeOpen(); toggleIssues(); clearSymbolSelection() }
  function runAutoMatch() { autoMatch(); clearSymbolSelection() }
  function selectType(id) { center.ensureHomeOpen(); setFilter(id); clearSymbolSelection() }
  function openSettings() { loadSettings(); center.select(CENTER_SETTINGS) }
  function requestDeleteLibrary(request) { setPendingDeleteLib(request) }
  async function togglePluginEnabled(plugin) {
    const next = plugin.enabled === false
    try {
      await appPluginRuntime.loader.setEnabled(plugin.id, next)
      await loadPlugins()
      toastMsg(t(next ? 'plugin.enabled' : 'plugin.disabled'))
    } catch (error) {
      toastMsg(error?.message || String(error))
    }
  }

  // 元件类型页的写入入口：弹窗和中心页共用同一套成功/失败回报。
  const saveComponentType = useCallback(async (payload) => {
    const r = await mutateCompType(payload)
    if (r.ok && payload.action === 'add') toastMsg(t('toast.added', { name: payload.name }))
    return r
  }, [mutateCompType, toastMsg, t])
  const removeComponentType = useCallback(async (item) => {
    const r = await mutateCompType({ action: 'remove', id: item.id })
    if (r.ok) toastMsg(t('toast.removed', { name: item.name }))
    // 待刷新/无有效快照时写入被冻结：如实说明没有提交，而不是报一个假失败
    else if (r.blocked) toastMsg(r.error || t('types.writeBlocked'))
    // 被引用的类型不能删：引用数量如实回报
    else toastMsg(r.references ? t('toast.typeInUse', { count: r.references }) : (r.error || t('toast.failedUnknown')))
    return r
  }, [mutateCompType, toastMsg, t])

  // Filtered data — memoized: rebuilt only when symbols change, not per render
  // 按类型 ID 归组（改名不影响筛选）；[id, name, count]
  const typeList = useMemo(() => {
    const byId = new Map()
    symbols.forEach(s => {
      const id = s.type_id || s.type
      const entry = byId.get(id) || { name: s.type || id, count: 0 }
      entry.count += 1
      byId.set(id, entry)
    })
    return [...byId.entries()].map(([id, e]) => [id, e.name, e.count])
      .sort((a, b) => b[2] - a[2])
  }, [symbols])

  // 列表用 UUID 区分同名符号；来源名从当前库快照取，不把展示名写进对象。
  const libraryNames = useMemo(() => {
    const map = {}
    libraries.forEach(l => { map[l.id] = l.name })
    return map
  }, [libraries])

  const bottomProblems = useMemo(() => {
    const list = []
    const add = (source, error) => {
      if (error) list.push({ source, severity: 'error', message: error.message || String(error) })
    }
    add('symbols', symbolsError)
    add('libraries', librariesError)
    add('types', compTypesError)
    for (const issue of issues.slice(0, 200)) {
      list.push({
        source: issue.symbol || 'check',
        severity: issue.severity || 'warning',
        message: issue.issue || String(issue.message || ''),
      })
    }
    for (const task of taskSnapshot.tasks) {
      if (task.state === 'failed' && task.error) {
        list.push({ source: task.pluginId || task.action, severity: 'error', message: task.error })
      }
    }
    return list
  }, [symbolsError, librariesError, compTypesError, issues, taskSnapshot.tasks])

  const centerPageLabels = useMemo(() => ({
    [CENTER_SETTINGS]: t('settings.title'),
    [CENTER_TYPES]: t('types.componentTypes'),
    [CENTER_MATCHING]: t('matching.title'),
  }), [t])
  const tabFor = useCallback((key) => {
    if (key === CENTER_HOME) return { id: key, label: t('sidebar.allSymbols'), closable: true }
    if (key.startsWith('view:')) {
      const viewId = key.slice(5)
      const entry = viewEntries.find((item) => item.options.id === viewId)
      return { id: key, label: entry?.options?.label || viewId, closable: true }
    }
    return { id: key, label: centerPageLabels[key] || key, closable: true }
  }, [centerPageLabels, t, viewEntries])
  // 内建页有固定父链：设置 → 元件类型 → 匹配规则。
  const breadcrumbFor = useCallback((active) => {
    const workspace = { label: t('sidebar.workspaceTitle'), page: CENTER_HOME }
    const settings = { label: t('settings.title'), page: CENTER_SETTINGS }
    const types = { label: t('types.componentTypes'), page: CENTER_TYPES }
    const matching = { label: t('matching.title'), page: CENTER_MATCHING }
    if (!active) return [workspace]
    if (active === CENTER_SETTINGS) return [workspace, settings]
    if (active === CENTER_TYPES) return [workspace, settings, types]
    if (active === CENTER_MATCHING) return [workspace, settings, types, matching]
    const tab = tabFor(active)
    return tab?.label ? [workspace, { label: tab.label, page: active }] : [workspace]
  }, [t, tabFor])

  // 页面只按身份渲染；分组容器负责把每组的全部页面 keep-alive，切组/切页不丢内部状态。
  const renderCenterPage = (key) => {
    if (key.startsWith('view:')) {
      const viewId = key.slice(5)
      const entry = viewEntries.find((item) => item.options.id === viewId)
      if (!entry) return null
      return (
        <div className="plugin-view">
          <PluginEntry host={appPluginRuntime.host} slot="app.view" entry={entry} />
        </div>
      )
    }
    if (key === CENTER_SETTINGS) {
      return (
        <SettingsModal inline {...{settings, setSettings, compLibraries, loadCompLibraries,
          onClose: () => center.close(CENTER_SETTINGS), refreshSymbols, loadStatus, toastMsg, showConfirm,
          onRequestDelete: setPendingDeleteLib, workspaceId: activeLib, dark, onToggleTheme: toggleDark,
          onOpenTool: (tool) => ({
            rules:     () => { loadRules(); setShowRules(true) },
            plugins:   () => { loadPlugins(); setShowPlugins(true) },
            compTypes: () => { loadCompTypes(); center.select(CENTER_TYPES) },
            pkgTypes:  () => { loadPkgTypes(); setShowPkgTypes(true) },
          }[tool]?.())}} />
      )
    }
    if (key === CENTER_TYPES) {
      return (
        <TypeModal inline title={t('types.componentTypes')} items={compTypes}
          onClose={() => center.close(CENTER_TYPES)}
          showIcon loading={compTypesLoading} loadError={compTypesError} stale={compTypesStale}
          onRetryLoad={loadCompTypes} canWrite={compTypesWritable}
          onOpenMatching={() => center.select(CENTER_MATCHING)}
          onSave={saveComponentType} onRemove={removeComponentType} />
      )
    }
    if (key === CENTER_MATCHING) {
      return (
        <TypeMatchingModal inline open types={compTypes} typeMap={typeMap}
          onClose={() => center.close(CENTER_MATCHING)}
          onRefreshCommitted={refreshMatchingViews} />
      )
    }
    return (
      <>
        <div className="center-heading">
          <h1>{t('app.centerTitle')}</h1>
          <span className="center-heading-meta" data-testid="center-library-counts"
            title={libraryStatsNotice || undefined}
            aria-label={`${t('sidebar.allSymbols')}: ${libraryStatsText}`}>
            {libraryStatsText}
          </span>
        </div>
        <Toolbar {...{classify, checkCorrespondence, autoMatch, workspaceId: activeLib, status, search, setSearch, loadSymbols,
          onSearch: (q) => loadSymbols(q, queryRef.current.targetLib)}} />
        <SymbolTable {...{
          symbols: resultSymbols, selection, onSelect: selectSymbol, onClearSelection: clearSymbolSelection,
          filter, issueFilter, typeMap, workspaceId: activeLib, libraryNames,
          pendingCutIds: pendingCut ? new Set(pendingCut.ids) : null,
          loading: symbolsLoading, error: symbolsError, onRetry: refreshSymbols,
          onCommandResult: reportEditingResult,
        }} />
      </>
    )
  }

  return (
    <OverlayProvider>
      <div className={dark ? 'app dark' : 'app'}>
      <div id="loading" style={{ display: loading ? 'block' : 'none' }} />

      <EscapeCutClearer active={Boolean(pendingCut)} onClear={clearPendingCut} />
      <TopBar toastMsg={toastMsg} controller={workbench}
        editScopeKey={`${activeLib}:${center.activeGroupId}:${center.active}`} />

      <div className="app-body">
        <Workbench
          controller={workbench}
          activities={sidebarActivities}
          host={appPluginRuntime.host}
          onOpenSettings={openSettings}
          sidebarProps={{
            workspace: { items: compLibraries, selectedId: activeLib, onSelect: switchWorkspace },
            navigation: {
              activeAll: center.active === CENTER_HOME && !filter && !issueFilter,
              activeIssues: center.active === CENTER_HOME && issueFilter,
              allCount: statusCountText(status, 'symbols'),
              allCountTitle: libraryStatsNotice || undefined,
              allCountLabel: `${t('sidebar.allSymbols')}: ${statusCountText(status, 'symbols')}`,
              issuesCount: issues.length, matchesCount: matches.length,
              onSelectAll: selectAllSymbols, onSelectIssues: selectIssues, onAutoMatch: runAutoMatch,
            },
            types: { items: typeList, metadata: typeMap, selectedId: filter, onSelect: selectType },
            libraries: { items: libraries, workspaceId: activeLib, selectedId: targetLib,
                         onSelect: selectLibrary, onDeleteRequest: requestDeleteLibrary },
            views: { items: viewEntries, selectedId: center.active, onSelect: center.selectView,
                     onOpenPlugin: openPluginView },
          }}
          main={(
            <CenterGroups
              center={center}
              tabFor={tabFor}
              breadcrumbFor={breadcrumbFor}
              renderPage={renderCenterPage}
            />
          )}
          inspector={<Inspector {...{
            selected, selectionCount: selection.ids.length, onClearSelection: clearSymbolSelection,
            classify, onDeleteSymbol: deleteSymbol, onRefresh: refreshSymbols, toastMsg, typeMap, workspaceId: activeLib,
          }} />}
          bottomTasks={taskSnapshot.tasks}
          bottomProblems={bottomProblems}
          statusbar={(
            <div className="statusbar">
              <span>{t('status.filtered', { count: resultSymbols.length, filter: filter ? ` · filtered by "${(typeList.find(([id]) => id === filter) || [])[1] || filter}"` : '' })}</span>
              {selection.ids.length > 0 && (
                <span className="statusbar-selection" data-testid="status-selection" style={{ marginLeft: 12 }}>
                  {t('status.selected', { count: selection.ids.length, total: resultSymbols.length })}
                  <button type="button" className="statusbar-button" style={{ marginLeft: 6 }}
                    onClick={clearSymbolSelection}>{t('app.clearSelection')}</button>
                </span>
              )}
              {/* 加载失败要显示出来：空列表和「请求挂了」不是一回事 */}
              {(symbolsError || librariesError) && (
                <span style={{ marginLeft: 12, color: 'var(--red)' }}>
                  {t('status.loadFailed', { error: (symbolsError || librariesError)?.message || '' })}
                </span>
              )}
              <span className="statusbar-item-group">
                <button type="button" className="statusbar-button" data-testid="status-tasks"
                  onClick={() => workbench.openBottomPanel('tasks')}
                  title={t('workbench.tasks')}>
                  {t('status.tasks', { count: taskSnapshot.tasks.length })}
                </button>
                <button type="button" className="statusbar-button" data-testid="status-problems"
                  onClick={() => workbench.openBottomPanel('problems')}
                  title={t('workbench.problems')}>
                  {t('status.problems', { count: bottomProblems.length })}
                </button>
              </span>
              {/* 全库统计与上方「当前结果/已选择」分开：未知或过期时保留占位/旧值，并给出可访问说明 */}
              <span className="statusbar-library-counts" data-testid="status-library-counts" role="status"
                style={{ marginLeft: 14 }}
                aria-label={`${t('sidebar.allSymbols')}: ${libraryStatsText}`}
                title={libraryStatsNotice || libraryStatsText}>
                {t('sidebar.allSymbols')}: {libraryStatsText}
              </span>
              <span style={{ marginLeft: 'auto' }}>{t('status.total', { count: visibleSymbols.length })}</span>
            </div>
          )}
        />
      </div>

      <div id="toast" className={toast ? 'show' : ''}>{toast}</div>

      {editRefresh && (
        <div
          className={`edit-refresh-notice ${editRefresh.status}`}
          data-testid="edit-refresh-notice"
          role={editRefresh.status === 'failed' ? 'alert' : 'status'}
          aria-live={editRefresh.status === 'failed' ? 'assertive' : 'polite'}>
          <span className="edit-refresh-message">
            {editRefresh.status === 'refreshing' && t('topbar.commitRefreshing')}
            {editRefresh.status === 'superseded' && t('topbar.commitRefreshSuperseded')}
            {editRefresh.status === 'failed' && (editRefresh.missingCount > 0
              ? editRefresh.error
              : t('topbar.commitRefreshFailed', { error: editRefresh.error || t('toast.failedUnknown') }))}
          </span>
          {editRefresh.status !== 'refreshing' && (
            <span className="edit-refresh-actions">
              <button type="button" className="statusbar-button"
                onClick={retryEditRefresh}>{t('topbar.commitRefreshRetry')}</button>
              <button type="button" className="statusbar-button"
                onClick={dismissEditRefresh}>{t('topbar.dismiss')}</button>
            </span>
          )}
        </div>
      )}

      <ImportingModal onDone={() => { loadLibraries(); refreshSymbols(); loadStatus(); }} />

      {classifyResult && <ClassifyResultModal result={classifyResult} refresh={classifyRefresh}
        onRetryRefresh={refreshClassifyViews} onClose={() => {
          if (!classifyLock.current && !classifyReadLock.current) setClassifyResult(null)
        }} />}
      {matchList && <MatchSuggestModal matches={matchList} onClose={() => setMatchList(null)} />}

      {pasteIntent && (
        <PasteTargetDialog
          intent={pasteIntent}
          libraries={pasteTargets}
          initialTargetId={targetLib || selected?.library_id || pasteTargets[0]?.id || ''}
          onClose={() => setPasteIntent(null)}
          onCommitted={(result, plan) => {
            const isMove = pasteIntent?.mode === 'move'
            setPasteIntent(null)
            if (isMove) {
              pendingCutRef.current = null
              setPendingCut(null)
            }
            // 提交成功不等于界面已生效：先按 UUID 刷新并核对目标库中的对象，
            // 全部发布后才选中并提示成功；失败保留旧列表/查询并可只读重试。
            const rows = isMove
              ? (Array.isArray(result?.moved) ? result.moved : [])
              : (Array.isArray(result?.created) ? result.created : [])
            const ids = rows.map(item => (typeof item === 'string' ? item : item?.uuid)).filter(Boolean)
            const targetLibraryId = plan?.target?.library_id || queryRef.current.targetLib || ''
            void runEditRefresh({
              kind: isMove ? 'move' : 'paste',
              ids,
              query: { search: '', targetLib: targetLibraryId },
              targetLibraryId,
              locate: true,
              requireAll: true,
              successKey: isMove ? 'topbar.moveSuccess' : 'topbar.pasteSuccess',
              successParams: { count: rows.length || ids.length },
            })
          }}
        />
      )}
      {showCreateLib && <CreateLibraryModal onClose={() => setShowCreateLib(false)} loadLibraries={loadLibraries} toastMsg={toastMsg} />}
      {showRules     && <RulesModal     {...{rules, onClose: () => setShowRules(false)}} />}
      {showPlugins   && <PluginsModal   {...{plugins, onClose: () => setShowPlugins(false), onTogglePlugin: togglePluginEnabled}} />}
      {showPkgTypes  && <TypeModal title={t('types.packageTypes')} items={pkgTypes} onClose={()=>setShowPkgTypes(false)}
        onAdd={([name,cat])=>manageType('/package-types','add',name,{category:cat}).then(ok=>ok&&loadPkgTypes())}
        onRemove={item=>manageType('/package-types','remove',item.name,{id:item.id}).then(ok=>ok&&loadPkgTypes())} showCategory />}

      {/* Confirm Dialog */}
      {confirmState.open && (
        <ConfirmDialog
          title={confirmState.title || t('confirm.title')}
          message={confirmState.message}
          confirmLabel={confirmState.confirmLabel || t('confirm.ok')}
          cancelLabel={t('confirm.cancel')}
          danger={confirmState.danger}
          onConfirm={handleConfirmYes}
          onCancel={handleConfirmNo}
        />
      )}
      {pendingDeleteLib && (
        <DeleteLibraryModal
          request={pendingDeleteLib}
          onClose={() => setPendingDeleteLib(null)}
          onConfirm={confirmDeleteLib} />
      )}
      </div>
    </OverlayProvider>
  )
}