import { useState, useEffect } from 'react'
import { useTranslation } from 'react-i18next'
import { setLoadingCallback, api } from './api'
import { useToast } from './hooks/useToast'
import { useDarkMode } from './hooks/useDarkMode'
import { useStatus, useSymbols, useLibraries, usePlugins, useIssues, useMatches, useSettings, useRules } from './hooks/useData'
import Sidebar from './components/Sidebar'
import Toolbar from './components/Toolbar'
import SymbolTable from './components/SymbolTable'
import Inspector from './components/Inspector'
import { SettingsModal, RulesModal, PluginsModal, TypeModal, PluginActionModal } from './components/Modals'
import './App.css'

const API = '/api'

export default function App() {
  const { t } = useTranslation()
  const { toast, toastMsg } = useToast()
  const { dark, toggleDark } = useDarkMode()

  // Data hooks
  const { status, loadStatus } = useStatus()
  const { symbols, loadSymbols, setSymbols } = useSymbols()
  const { libraries, loadLibraries, setLibraries } = useLibraries()
  const { plugins, loadPlugins } = usePlugins()
  const { issues, loadIssues } = useIssues()
  const { matches, loadMatches } = useMatches()
  const { settings, setSettings, loadSettings } = useSettings()
  const { rules, loadRules } = useRules()

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

  // Loading callback
  useEffect(() => { setLoadingCallback(setLoading) }, [])

  // Heartbeat
  useEffect(() => { const beat = setInterval(() => fetch(`${API}/status`).catch(()=>{}), 500); return () => clearInterval(beat) }, [])

  // Classify / check / match
  async function classify() {
    toastMsg(t('toast.classifying'))
    const r = await api.classify()
    if (r) { toastMsg(t('toast.classified', { matched: r.matched, total: r.total })); loadSymbols(); loadStatus() }
  }
  function toggleIssues() {
    setIssueFilter(prev => !prev)
    setFilter('')
    setSelected(null)
  }

  async function checkCorrespondence() {
    await loadIssues()
    toastMsg(issues.length === 0 ? t('toast.noIssues') : t('toast.issuesFound', { count: issues.length }))
  }
  async function autoMatch() {
    await loadMatches()
    toastMsg(t('toast.matchSuggestions', { count: matches.length }))
  }

  // Library operations
  function createLibrary() { const name = prompt('Library name:'); if (name) api.createLibrary(name).then(() => loadLibraries()) }
  async function deleteSymbol(sym) {
    if (!confirm(t('confirm.deleteSymbol', { name: sym.name }))) return
    const r = await api.deleteSymbol(sym.id)
    if (r?.ok) { toastMsg(t('toast.deleted', { name: '', extra: '' })); setSelected(null); loadSymbols(); loadStatus() }
    else toastMsg(r?.error || t('toast.failedUnknown'))
  }

  // Type management
  async function loadCompTypes() { const d = await api.compTypes(); if (d) setCompTypes(d) }
  async function loadPkgTypes() { const d = await api.pkgTypes(); if (d) setPkgTypes(d) }
  async function manageType(url, action, name) {
    const r = await api.manageType(url, action, name)
    if (r?.ok) { toastMsg(t(action === 'add' ? 'toast.added' : 'toast.removed', { name })); return true }
    toastMsg(r?.error || 'Failed'); return false
  }

  // Filtered data
  const typeCounts = {}; symbols.forEach(s => { typeCounts[s.type] = (typeCounts[s.type]||0) + 1 })
  const typeList = Object.entries(typeCounts).sort((a, b) => b[1] - a[1])

  return (
    <div className={dark ? 'app dark' : 'app'}>
      <div id="loading" style={{ display: loading ? 'block' : 'none' }} />

      <Sidebar {...{filter, setFilter, clearFilters: () => { setFilter(''); setIssueFilter(false) }, setSelected, setTargetLib, loadSymbols: (q, lib) => loadSymbols(q, lib),
        status, issues, checkCorrespondence: toggleIssues, matches, autoMatch, typeList, issueFilter,
        targetLib, setTargetLibRaw: setTargetLib, libraries, createLibrary,
        loadRules, setShowRules, loadPlugins, setShowPlugins,
        loadCompTypes: () => { loadCompTypes(); setShowCompTypes(true) },
        setShowCompTypes, loadPkgTypes: () => { loadPkgTypes(); setShowPkgTypes(true) },
        setShowPkgTypes, loadSettings, setShowSettings,
        dark, toggleDark, api, loadLibraries, loadSymbols, toastMsg }} />

      <div className="main">
        <Toolbar {...{plugins, loadLibraries, setPluginAction, setPluginFormData, classify, checkCorrespondence, autoMatch, status, search, setSearch, loadSymbols}} />
        <SymbolTable {...{symbols, filter, issueFilter, selected, setSelected}} />
        <div className="statusbar">
          <span>{t('status.filtered', { count: (filter ? symbols.filter(s => s.type === filter) : symbols).length, filter: filter ? ` · filtered by "${filter}"` : '' })}</span>
          <span style={{ marginLeft: 'auto' }}>{t('status.total', { count: symbols.length })}</span>
        </div>
      </div>

      <Inspector {...{selected, classify, autoMatch, onDeleteSymbol: deleteSymbol}} />

      <div id="toast" className={toast ? 'show' : ''}>{toast}</div>

      <PluginActionModal {...{pluginAction, setPluginAction, pluginFormData, setPluginFormData, libraries, toastMsg, loadSymbols, loadLibraries, loadStatus}} />

      {showSettings  && <SettingsModal  {...{settings, setSettings, onClose: () => setShowSettings(false), loadSymbols, loadStatus, toastMsg}} />}
      {showRules     && <RulesModal     {...{rules, onClose: () => setShowRules(false)}} />}
      {showPlugins   && <PluginsModal   {...{plugins, onClose: () => setShowPlugins(false)}} />}
      {showCompTypes && <TypeModal title={t('types.componentTypes')} items={compTypes} onClose={()=>setShowCompTypes(false)}
        onAdd={([name])=>manageType('/component-types','add',name).then(ok=>ok&&loadCompTypes())}
        onRemove={name=>manageType('/component-types','remove',name).then(ok=>ok&&loadCompTypes())} showIcon />}
      {showPkgTypes  && <TypeModal title={t('types.packageTypes')} items={pkgTypes} onClose={()=>setShowPkgTypes(false)}
        onAdd={([name,cat])=>manageType('/package-types','add',name,{category:cat}).then(ok=>ok&&loadPkgTypes())}
        onRemove={name=>manageType('/package-types','remove',name).then(ok=>ok&&loadPkgTypes())} showCategory />}
    </div>
  )
}
