import { useState, useEffect, useCallback, useMemo, useRef } from 'react'
import { useTranslation } from 'react-i18next'
import { setLoadingCallback, api } from './api'
import { useToast } from './hooks/useToast'
import { useDarkMode } from './hooks/useDarkMode'
import { useStatus, useSymbols, useLibraries, usePlugins, useIssues, useMatches, useSettings, useRules, useComponentLibraries } from './hooks/useData'
import Sidebar from './components/Sidebar'
import Toolbar from './components/Toolbar'
import SymbolTable from './components/SymbolTable'
import Inspector from './components/Inspector'
import { SettingsModal, RulesModal, PluginsModal, TypeModal, PluginActionModal, CreateLibraryModal, ImportingModal, ClassifyResultModal, MatchSuggestModal, DeleteLibraryModal } from './components/Modals'
import './App.css'

const API = '/api'

export default function App() {
  const { t } = useTranslation()
  const { toast, toastMsg } = useToast()
  const { dark, toggleDark } = useDarkMode()

  // Data hooks
  const { status, loadStatus } = useStatus()
  const { symbols, symbolsError, loadSymbols, setSymbols } = useSymbols()
  const { libraries, librariesError, loadLibraries, setLibraries } = useLibraries()
  const { plugins, loadPlugins } = usePlugins()
  const { issues, loadIssues } = useIssues()
  const { matches, loadMatches } = useMatches()
  const { settings, setSettings, loadSettings } = useSettings()
  const { rules, loadRules } = useRules()
  const { compLibraries, loadCompLibraries, setCompLibraries } = useComponentLibraries()

  // UI state
  const [filter, setFilter] = useState('')
  const [search, setSearch] = useState('')
  const [selected, setSelected] = useState(null)
  const [targetLib, setTargetLib] = useState('')
  const [issueFilter, setIssueFilter] = useState(false)
  const [loading, setLoading] = useState(false)
  const [pluginAction, setPluginAction] = useState(null)
  const [pluginFormData, setPluginFormData] = useState({})

  // Modal visibility
  const [showSettings, setShowSettings] = useState(false)
  const [showRules, setShowRules] = useState(false)
  const [showPlugins, setShowPlugins] = useState(false)
  const [showCompTypes, setShowCompTypes] = useState(false)
  const [showPkgTypes, setShowPkgTypes] = useState(false)
  const [compTypes, setCompTypes] = useState([])
  const [pkgTypes, setPkgTypes] = useState([])
  const [pendingDeleteLib, setPendingDeleteLib] = useState(null)  // DeleteLibraryModal pending

  // 确认弹窗（替代 window.confirm）
  const [confirmState, setConfirmState] = useState({ open: false, message: '' })
  const confirmResolve = useRef(null)

  const showConfirm = useCallback((message) => {
    return new Promise((resolve) => {
      confirmResolve.current = resolve
      setConfirmState({ open: true, message })
    })
  }, [])

  // Two-step delete — the request carries the object kind so the right API is
  // used: 'symbol' → /api/libraries (unregister; file removal is opt-in),
  // 'component' → /api/component-libraries (registration only, never files).
  const confirmDeleteLib = (async (deleteFile) => {
    const req = pendingDeleteLib
    if (!req) return
    setPendingDeleteLib(null)
    const { kind, lib } = req
    if (kind === 'symbol') {
      const r = await api.deleteLibrary(lib.id, deleteFile)
      if (r?.ok) {
        toastMsg(deleteFile
          ? t('toast.deleted', { name: lib.name, extra: r.deleted_file ? ' (+file)' : '' })
          : t('toast.removed', { name: lib.name }))
        loadLibraries(); loadSymbols(); setTargetLib('')
      } else toastMsg(r?.error || t('toast.failedUnknown'))
    } else {
      const r = await api.saveCompLibrary({ action: 'remove', id: lib.id })
      if (r?.ok) {
        toastMsg(t('toast.removed', { name: lib.name }))
        loadLibraries(); loadSymbols(); setTargetLib('')
      } else toastMsg(r?.error || t('toast.failedUnknown'))
    }
  }, [pendingDeleteLib, t, toastMsg, loadLibraries, loadSymbols])

  const handleConfirmYes = useCallback(() => {
    confirmResolve.current?.(true)
    setConfirmState({ open: false, message: '' })
  }, [])

  const handleConfirmNo = useCallback(() => {
    confirmResolve.current?.(false)
    setConfirmState({ open: false, message: '' })
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

  // Libraries of the active workspace — symbols are filtered by their library ids.
  // A concrete workspace with no libraries shows an EMPTY list (never falls back
  // to symbols of other workspaces).
  const workspaceLibraryIds = useMemo(
    () => new Set(libraries.filter(l => l.component_library_id === activeLib).map(l => l.id)),
    [libraries, activeLib])
  const visibleSymbols = useMemo(
    () => (!activeLib ? symbols : symbols.filter(s => workspaceLibraryIds.has(s.library_id))),
    [symbols, activeLib, workspaceLibraryIds])

  async function switchWorkspace(id) {
    setSettings(prev => ({ ...prev, active_component_library: id }))
    // Context change → reset the whole query state (selection, keyword, lib)
    setSelected(null); setTargetLib(''); setFilter(''); setSearch('')
    await api.saveConfig({ active_component_library: id })
    loadLibraries(); loadSymbols()
  }

  // ---- Unified query state: keyword + library travel together ----
  // Refreshes keep the current search/library context; a context change
  // (library / workspace switch) resets the selection.
  const refreshSymbols = useCallback(
    () => loadSymbols(search, targetLib),
    [search, targetLib, loadSymbols])

  async function selectLibrary(libId) {
    setFilter(''); setSelected(null); setTargetLib(libId)
    loadSymbols(search, libId)
  }

  // Heartbeat + shutdown signal
  // Use recursive setTimeout — browsers throttle setInterval in background tabs
  useEffect(() => {
    let active = true
    function beat() {
      if (!active) return
      fetch(`${API}/status`).catch(()=>{})
      setTimeout(beat, 1000)
    }
    beat()
    const bye = () => { navigator.sendBeacon(`${API}/bye`, '{}') }
    window.addEventListener('beforeunload', bye)
    return () => { active = false; window.removeEventListener('beforeunload', bye) }
  }, [])

  // Classify / check / match
  const [classifyResult, setClassifyResult] = useState(null)
  const [matchList, setMatchList] = useState(null)

  async function classify() {
    toastMsg(t('toast.classifying'))
    try {
      const r = await api.classify()
      if (r?.error) { toastMsg(t('toast.failed', { error: r.error })); return }
      setClassifyResult(r); loadSymbols(); loadStatus()
    } catch (cause) {
      toastMsg(t('toast.failed', { error: cause.message }))
    }
  }
  function toggleIssues() {
    setIssueFilter(prev => !prev)
    setFilter('')
    setSelected(null)
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
    const r = await api.deleteSymbol(sym.id)
    if (r?.ok) { toastMsg(t('toast.deleted', { name: '', extra: '' })); setSelected(null); loadSymbols(); loadStatus() }
    else toastMsg(r?.error || t('toast.failedUnknown'))
  }

  // Type management
  async function loadCompTypes() { const d = await api.compTypes(); if (d) setCompTypes(d) }
  async function loadPkgTypes() { const d = await api.pkgTypes(); if (d) setPkgTypes(d) }
  async function manageType(url, action, name, extra) {
    const r = await api.manageType(url, action, name, extra)
    if (r?.ok) { toastMsg(t(action === 'add' ? 'toast.added' : 'toast.removed', { name })); return true }
    // 被引用的类型不能删：返回引用数量
    toastMsg(r?.references ? t('toast.typeInUse', { count: r.references }) : (r?.error || 'Failed'))
    return false
  }

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

  return (
    <div className={dark ? 'app dark' : 'app'}>
      <div id="loading" style={{ display: loading ? 'block' : 'none' }} />

      <Sidebar {...{filter, setFilter, clearFilters: () => { setFilter(''); setIssueFilter(false) }, setSelected, setTargetLib, loadSymbols: (q, lib) => loadSymbols(q, lib),
        onSelectLibrary: selectLibrary,
        status, issues, checkCorrespondence: toggleIssues, matches, autoMatch, typeList, issueFilter,
        targetLib, setTargetLibRaw: setTargetLib, libraries,
        onRequestDelete: setPendingDeleteLib,
        plugins, setPluginAction, setPluginFormData,
        compLibraries, activeLib, onSwitchWorkspace: switchWorkspace,
        loadRules, setShowRules, loadPlugins, setShowPlugins,
        loadCompTypes: () => { loadCompTypes(); setShowCompTypes(true) },
        setShowCompTypes, loadPkgTypes: () => { loadPkgTypes(); setShowPkgTypes(true) },
        setShowPkgTypes, loadSettings, setShowSettings,
        dark, toggleDark, api, loadLibraries, loadSymbols, toastMsg, showConfirm }} />

      <div className="main">
        <Toolbar {...{plugins, loadLibraries, setPluginAction, setPluginFormData, classify, checkCorrespondence, autoMatch, targetLib, status, search, setSearch, loadSymbols,
          onSearch: (q) => loadSymbols(q, targetLib)}} />
        <SymbolTable {...{symbols: visibleSymbols, filter, issueFilter, selected, setSelected}} />
        <div className="statusbar">
          <span>{t('status.filtered', { count: (filter ? visibleSymbols.filter(s => (s.type_id || s.type) === filter) : visibleSymbols).length, filter: filter ? ` · filtered by "${(typeList.find(([id]) => id === filter) || [])[1] || filter}"` : '' })}</span>
          {/* 加载失败要显示出来：空列表和「请求挂了」不是一回事 */}
          {(symbolsError || librariesError) && (
            <span style={{ marginLeft: 12, color: 'var(--red)' }}>
              {t('status.loadFailed', { error: (symbolsError || librariesError)?.message || '' })}
            </span>
          )}
          <span style={{ marginLeft: 'auto' }}>{t('status.total', { count: visibleSymbols.length })}</span>
        </div>
      </div>

      <Inspector {...{selected, classify, onDeleteSymbol: deleteSymbol, onRefresh: refreshSymbols, toastMsg}} />

      <div id="toast" className={toast ? 'show' : ''}>{toast}</div>

      <ImportingModal onDone={() => { loadLibraries(); loadSymbols(); loadStatus(); }} />

      <PluginActionModal {...{pluginAction, setPluginAction, pluginFormData, setPluginFormData, libraries, toastMsg, loadSymbols, loadLibraries, loadStatus,
        activeWorkspace: compLibraries.find(c => c.id === activeLib)}} />

      {classifyResult && <ClassifyResultModal result={classifyResult} onClose={() => setClassifyResult(null)} />}
      {matchList && <MatchSuggestModal matches={matchList} onClose={() => setMatchList(null)} />}

      {showCreateLib && <CreateLibraryModal onClose={() => setShowCreateLib(false)} loadLibraries={loadLibraries} toastMsg={toastMsg} />}
      {showSettings  && <SettingsModal  {...{settings, setSettings, compLibraries, setCompLibraries, loadCompLibraries, onClose: () => setShowSettings(false), loadSymbols, loadStatus, toastMsg, showConfirm, onRequestDelete: setPendingDeleteLib,
        onOpenTool: (key) => ({
          rules:     () => { loadRules(); setShowRules(true) },
          plugins:   () => { loadPlugins(); setShowPlugins(true) },
          compTypes: () => { loadCompTypes(); setShowCompTypes(true) },
          pkgTypes:  () => { loadPkgTypes(); setShowPkgTypes(true) },
        }[key]?.())}} />}
      {showRules     && <RulesModal     {...{rules, onClose: () => setShowRules(false)}} />}
      {showPlugins   && <PluginsModal   {...{plugins, onClose: () => setShowPlugins(false), setPluginAction, setPluginFormData, loadLibraries, targetLib}} />}
      {showCompTypes && <TypeModal title={t('types.componentTypes')} items={compTypes} onClose={()=>setShowCompTypes(false)}
        onAdd={([name])=>manageType('/component-types','add',name).then(ok=>ok&&loadCompTypes())}
        onRemove={item=>manageType('/component-types','remove',item.name,{id:item.id}).then(ok=>ok&&loadCompTypes())} showIcon />}
      {showPkgTypes  && <TypeModal title={t('types.packageTypes')} items={pkgTypes} onClose={()=>setShowPkgTypes(false)}
        onAdd={([name,cat])=>manageType('/package-types','add',name,{category:cat}).then(ok=>ok&&loadPkgTypes())}
        onRemove={item=>manageType('/package-types','remove',item.name,{id:item.id}).then(ok=>ok&&loadPkgTypes())} showCategory />}

      {/* Confirm Dialog */}
      {confirmState.open && (
        <div className="modal-overlay show" onClick={e => { if (e.target === e.currentTarget) handleConfirmNo() }}>
          <div className="confirm-modal">
            <p style={{ whiteSpace: 'pre-line', lineHeight: 1.6 }}>{confirmState.message}</p>
            <div className="btn-row" style={{ marginTop: 20 }}>
              <button className="btn-ghost" onClick={handleConfirmNo}>Cancel</button>
              <button className="btn-primary" style={{ background: 'var(--red)' }} onClick={handleConfirmYes}>Delete</button>
            </div>
          </div>
        </div>
      )}
      {pendingDeleteLib && (
        <DeleteLibraryModal
          request={pendingDeleteLib}
          onClose={() => setPendingDeleteLib(null)}
          onConfirm={confirmDeleteLib} />
      )}
    </div>
  )
}
