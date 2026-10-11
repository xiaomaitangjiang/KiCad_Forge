import { useState } from 'react'
import { useTranslation } from 'react-i18next'

const TYPE_ICONS = {
  Resistor: '⊟', Capacitor: '⊞', Inductor: '◠', Diode: '▷', LED: '☀',
  Microcontroller: '⬡', Connector: '▣', Crystal: '◇', VoltageRegulator: '▦',
  Transistor: '△', MOSFET: '△', FPGA: '◰', Memory: '▤', Oscillator: '◆',
  Fuse: '◎', Switch: '⏣', Relay: '⏻', Jumper: '⌸', TestPoint: '◎',
  OpAmp: '▷', Comparator: '⩽', LogicGate: '⊡',
}

export default function Sidebar({
  filter, setFilter, clearFilters, setSelected, setTargetLib, loadSymbols,
  status, issues, checkCorrespondence, matches, autoMatch, issueFilter,
  typeList, targetLib, setTargetLibRaw, libraries,
  loadRules, setShowRules, loadPlugins, setShowPlugins,
  loadCompTypes, setShowCompTypes, loadPkgTypes, setShowPkgTypes,
  loadSettings, setShowSettings, dark, toggleDark, api,
  loadLibraries, toastMsg, showConfirm,
  onRequestDelete,
  plugins, setPluginAction, setPluginFormData,
  compLibraries, activeLib, onSwitchWorkspace,
  onSelectLibrary,
}) {
  const { t, i18n } = useTranslation()
  const [expanded, setExpanded] = useState({})

  // Workspace filter: show only libraries of the active component library
  // (empty activeLib → show everything, backwards compatible)
  const visibleLibraries = activeLib
    ? libraries.filter(l => l.component_library_id === activeLib)
    : libraries

  return (
    <nav className="sidebar">
      <div className="logo">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"><rect x="2" y="3" width="8" height="8" rx="2"/><rect x="14" y="3" width="8" height="8" rx="2"/><rect x="2" y="13" width="8" height="8" rx="2"/><rect x="14" y="13" width="8" height="8" rx="2"/></svg>
        {t('app.title')}
      </div>

      {/* Workspace selector — full-width row under the logo */}
      {(compLibraries || []).length > 0 && (
        <div style={{ padding:'0 10px 8px' }}>
          <select value={activeLib || ''} onChange={e => onSwitchWorkspace(e.target.value)}
            title={t('sidebar.workspace')}
            style={{ width:'100%', padding:'5px 8px', fontSize:11.5, border:'1px solid var(--sep)',
                     borderRadius:6, background:'var(--bg)', color:'var(--text)' }}>
            <option value="">{t('sidebar.allWorkspaces')}</option>
            {compLibraries.map(c => (
              <option key={c.id} value={c.id}>{c.locked ? '🔒 ' : ''}{c.name}</option>
            ))}
          </select>
        </div>
      )}

      <div className="section">
        <div className="section-title">{t('sidebar.library')}</div>
        <div className={`nav-item ${!filter && !issueFilter ? 'active' : ''}`} onClick={() => { clearFilters(); setSelected(null); setTargetLib(''); loadSymbols() }}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="9" y1="9" x2="15" y2="9"/><line x1="9" y1="13" x2="15" y2="13"/><line x1="9" y1="17" x2="12" y2="17"/></svg>
          {t('sidebar.allSymbols')} <span className="badge">{status.symbols}</span>
        </div>
        <div className={`nav-item ${issueFilter ? 'active' : ''}`} onClick={() => { checkCorrespondence(); setSelected(null) }}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
          {t('sidebar.issues')} <span className="badge" style={issueFilter ? {background:'var(--orange)',color:'#fff'} : (issues.length > 0 ? {background:'var(--orange)',color:'#fff'} : {})}>{issues.length}</span>
        </div>
        <div className="nav-item" onClick={() => { autoMatch(); setSelected(null) }}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
          {t('sidebar.match')} <span className="badge">{matches.length}</span>
        </div>
      </div>

      <div className="section">
        <div className="section-title" onClick={() => setExpanded({...expanded, types: !expanded.types})}
          style={{cursor:'pointer',userSelect:'none'}}>
          {expanded.types ? '▾' : '▸'} {t('sidebar.componentTypes')}
        </div>
        {(expanded.types === true) && typeList.map(([id, name, n]) => (
          <div key={id} className={`nav-item ${filter === id ? 'active' : ''}`} onClick={() => { setFilter(id); setSelected(null) }}>
            <span className="type-dot">{TYPE_ICONS[id] || '○'}</span> {name} <span className="badge">{n}</span>
          </div>
        ))}
      </div>

      {/* Libraries — workspace-scoped flat list */}
      <div className="section" style={{ display:'flex', flexDirection:'column', flex:1, minHeight:0 }}>
        <div className="section-title">{t('sidebar.libraries')}</div>
        <div style={{ flex:1, overflowY:'auto', minHeight:0 }}>
          {visibleLibraries.map(l => (
            <div key={l.id} className={`nav-item ${targetLib === l.id ? 'active' : ''}`}
              style={{ display:'flex', justifyContent:'space-between', padding:'4px 8px', fontSize:11.5 }}
              onClick={() => onSelectLibrary?.(l.id)}>
              <span style={{overflow:'hidden',textOverflow:'ellipsis',whiteSpace:'nowrap'}}>
                <span className="dot dot-ok" /> {l.locked ? '🔒 ' : ''}{l.name}
              </span>
              {/* Delete — symbol library: unregister by default, opt-in file removal */}
              <span style={{ cursor:'pointer', color:'var(--red)', fontSize:12, padding:'0 2px' }}
                onClick={e => { e.stopPropagation(); onRequestDelete({ kind: 'symbol', lib: l }) }}>×</span>
            </div>
          ))}
          {visibleLibraries.length === 0 && (
            <div style={{ fontSize:11, color:'var(--text3)', padding:'8px 10px' }}>{t('sidebar.noLibraries')}</div>
          )}
        </div>
      </div>

      {/* Plugin action buttons — location="sidebar" */}
      <div className="section">
        <div className="section-title">{t('sidebar.tools')}</div>
        <div className="nav-item" onClick={() => { loadSettings(); setShowSettings(true) }}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
          {t('sidebar.settings')}
        </div>
      </div>

      {/* Plugin action buttons — pinned to the bottom so they don't push the
          library list down */}
      <div style={{ marginTop: 'auto', padding: '10px' }}>
        {(plugins || []).filter(p => p.actions?.some(a => a.button?.show !== false && a.button?.location === 'sidebar')).map(p =>
          p.actions.filter(a => a.button?.show !== false && a.button?.location === 'sidebar').map(action => {
            const btn = action.button || {}
            const iconSrc = action.icon_url || p.icon_url
            return (
              <div className="nav-item" key={`${p.id}/${action.id}`} title={btn.tooltip || action.description}
                onClick={() => {
                  loadLibraries()
                  // Pre-fill target_library for inline actions (mirrors Toolbar behavior)
                  const prefill = action.trigger === 'inline' ? { target_library: targetLib || '' } : {}
                  setPluginAction({ plugin: p, action })
                  setPluginFormData(prefill)
                }}>
                {iconSrc
                  ? <img src={iconSrc} alt="" width="14" height="14" style={{ objectFit: 'contain' }} />
                  : <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/></svg>}
                {action.name}
              </div>
            )
          })
        )}
        <div className="nav-item" onClick={() => i18n.changeLanguage(i18n.language === 'zh' ? 'en' : 'zh')}>
          {'🌐'} {i18n.language === 'zh' ? 'English' : '中文'}
        </div>
        <div className="nav-item" onClick={toggleDark}>
          {dark
            ? <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/></svg>
            : <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>
          }
          {dark ? t('sidebar.lightMode') : t('sidebar.darkMode')}
        </div>
      </div>
    </nav>
  )
}
