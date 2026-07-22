import { useState, useEffect, useCallback, useRef } from 'react'
import './App.css'

const API = '/api'

const TYPE_ICONS = {
  Resistor: '⊟', Capacitor: '⊞', Inductor: '◠', Diode: '▷', LED: '☀',
  Microcontroller: '⬡', Connector: '▣', Crystal: '◇', VoltageRegulator: '▦',
  Transistor: '△', MOSFET: '△', FPGA: '◰', Memory: '▤', Oscillator: '◆',
  Fuse: '◎', Switch: '⏣', Relay: '⏻', Jumper: '⌸', TestPoint: '◎',
  OpAmp: '▷', Comparator: '≽', LogicGate: '⊡',
}

export default function App() {
  const [symbols, setSymbols] = useState([])
  const [filter, setFilter] = useState('')
  const [search, setSearch] = useState('')
  const [status, setStatus] = useState({ symbols: 0, footprints: 0 })
  const [selected, setSelected] = useState(null)
  const [dark, setDark] = useState(() => localStorage.getItem('kf-dark') === '1')
  const [toast, setToast] = useState('')
  const [loading, setLoading] = useState(false)
  const [issues, setIssues] = useState([])
  const [matches, setMatches] = useState([])
  const [showSettings, setShowSettings] = useState(false)
  const [showRules, setShowRules] = useState(false)
  const [showPlugins, setShowPlugins] = useState(false)
  const [showCompTypes, setShowCompTypes] = useState(false)
  const [showPkgTypes, setShowPkgTypes] = useState(false)
  const [settings, setSettings] = useState({ symbol_lib_path: '', footprint_lib_path: '', model_3d_path: '' })
  const [rules, setRules] = useState([])
  const [plugins, setPlugins] = useState([])
  const [compTypes, setCompTypes] = useState([])
  const [pkgTypes, setPkgTypes] = useState([])
  const toastTimer = useRef(null)

  const toastMsg = useCallback((msg) => {
    setToast(msg)
    clearTimeout(toastTimer.current)
    toastTimer.current = setTimeout(() => setToast(''), 2800)
  }, [])

  const api = useCallback(async (url, opts = {}) => {
    setLoading(true)
    try { const r = await fetch(url, opts); setLoading(false); return r.json() }
    catch { setLoading(false); toastMsg('Network error'); return null }
  }, [toastMsg])

  async function loadSymbols(q) {
    const url = q ? `${API}/symbols?q=${encodeURIComponent(q)}` : `${API}/symbols`
    const data = await api(url)
    if (data) setSymbols(Array.isArray(data) ? data : [])
  }

  async function loadStatus() { const d = await api(`${API}/status`); if (d) setStatus(d) }
  async function loadIssues() { const d = await api(`${API}/check`, { method: 'POST' }); if (d) setIssues(Array.isArray(d) ? d : []) }
  async function loadMatches() { const d = await api(`${API}/automatch`, { method: 'POST' }); if (d) setMatches(Array.isArray(d) ? d : []) }
  async function loadSettings() { const d = await api(`${API}/settings`); if (d) setSettings(d) }
  async function loadRules() { const d = await api(`${API}/rules`); if (d) setRules(d || []) }
  async function loadPlugins() { const d = await api(`${API}/plugins`); if (d) setPlugins(d || []) }
  async function loadCompTypes() { const d = await api(`${API}/component-types`); if (d) setCompTypes(Array.isArray(d) ? d : []) }
  async function loadPkgTypes() { const d = await api(`${API}/package-types`); if (d) setPkgTypes(Array.isArray(d) ? d : []) }
  async function manageType(url, action, name, extra = {}) {
    const r = await api(url, { method: 'POST', body: JSON.stringify({ action, name, ...extra }) })
    if (r && r.ok) { toastMsg(`${action === 'add' ? 'Added' : 'Removed'} ${name}`); return true }
    toastMsg(r?.error || 'Failed'); return false
  }

  async function classify() {
    toastMsg('Classifying...')
    const r = await api(`${API}/classify`, { method: 'POST' })
    if (r) { toastMsg(`Classified: ${r.matched}/${r.total} matched`); loadSymbols(); loadStatus() }
  }

  async function checkCorrespondence() {
    await loadIssues()
    toastMsg(issues.length === 0 ? 'No issues found' : `${issues.length} issue(s) found`)
  }

  async function autoMatch() {
    await loadMatches()
    toastMsg(`${matches.length} match suggestion(s)`)
  }

  async function saveSettings() {
    const r = await api(`${API}/settings`, { method: 'POST', body: JSON.stringify(settings) })
    if (r) {
      toastMsg(`Saved — imported ${r.imported_symbols || 0} symbols, ${r.imported_footprints || 0} footprints`)
      setShowSettings(false); loadSymbols(); loadStatus()
    }
  }

  const toggleDark = () => {
    const next = !dark
    setDark(next)
    localStorage.setItem('kf-dark', next ? '1' : '0')
  }

  useEffect(() => { loadStatus(); loadSymbols(); loadIssues(); loadMatches(); loadSettings() }, [])

  const filtered = filter ? symbols.filter(s => s.type === filter) : symbols
  const typeCounts = {}
  symbols.forEach(s => { typeCounts[s.type] = (typeCounts[s.type] || 0) + 1 })
  const typeList = Object.entries(typeCounts).sort((a, b) => b[1] - a[1])

  return (
    <div className={dark ? 'app dark' : 'app'}>
      <div id="loading" style={{ display: loading ? 'block' : 'none' }} />

      {/* Sidebar */}
      <nav className="sidebar">
        <div className="logo">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"><rect x="2" y="3" width="8" height="8" rx="2"/><rect x="14" y="3" width="8" height="8" rx="2"/><rect x="2" y="13" width="8" height="8" rx="2"/><rect x="14" y="13" width="8" height="8" rx="2"/></svg>
          KiCad Forge
        </div>

        <div className="section">
          <div className="section-title">Library</div>
          <div className={`nav-item ${!filter ? 'active' : ''}`} onClick={() => { setFilter(''); setSelected(null); loadSymbols() }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="9" y1="9" x2="15" y2="9"/><line x1="9" y1="13" x2="15" y2="13"/><line x1="9" y1="17" x2="12" y2="17"/></svg>
            All Symbols
            <span className="badge">{status.symbols}</span>
          </div>
          <div className="nav-item" onClick={() => { checkCorrespondence(); setSelected(null) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
            Issues
            <span className="badge" style={issues.length > 0 ? {background: 'var(--orange)',color:'#fff'} : {}}>{issues.length}</span>
          </div>
          <div className="nav-item" onClick={() => { autoMatch(); setSelected(null) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
            Match
            <span className="badge">{matches.length}</span>
          </div>
        </div>

        <div className="section">
          <div className="section-title">Component Types</div>
          {typeList.map(([t, n]) => (
            <div key={t} className={`nav-item ${filter === t ? 'active' : ''}`} onClick={() => { setFilter(t); setSelected(null) }}>
              <span className="type-dot">{TYPE_ICONS[t] || '○'}</span>
              {t}
              <span className="badge">{n}</span>
            </div>
          ))}
        </div>

        <div className="section">
          <div className="section-title">Tools</div>
          <div className="nav-item" onClick={() => { loadRules(); setShowRules(true) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polyline points="4 7 12 4 20 7"/><line x1="12" y1="4" x2="12" y2="20"/><polyline points="4 7 4 17 12 20"/><polyline points="20 7 20 17 12 20"/></svg>
            Rules
          </div>
          <div className="nav-item" onClick={() => { loadPlugins(); setShowPlugins(true) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/><line x1="12" y1="22" x2="12" y2="15.5"/><polyline points="22 8.5 12 15.5 2 8.5"/></svg>
            Plugins
          </div>
          <div className="nav-item" onClick={() => { loadCompTypes(); setShowCompTypes(true) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/></svg>
            Component Types
          </div>
          <div className="nav-item" onClick={() => { loadPkgTypes(); setShowPkgTypes(true) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><rect x="3" y="3" width="18" height="18" rx="3"/><rect x="7" y="7" width="10" height="10" rx="1"/></svg>
            Package Types
          </div>
          <div className="nav-item" onClick={() => { loadSettings(); setShowSettings(true) }}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>
            Settings
          </div>
        </div>

        <div style={{ marginTop: 'auto', padding: '10px' }}>
          <div className="nav-item" onClick={toggleDark}>
            {dark ? (
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/></svg>
            ) : (
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>
            )}
            {dark ? 'Light Mode' : 'Dark Mode'}
          </div>
        </div>
      </nav>

      {/* Main Content */}
      <div className="main">
        <div className="toolbar">
          <button className="btn-primary" onClick={classify}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
            Auto Classify
          </button>
          <button className="btn-ghost" onClick={checkCorrespondence}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
            Check
          </button>
          <button className="btn-ghost" onClick={autoMatch}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
            Auto Match
          </button>
          <span className="sep" />
          <input
            style={{ width: 150 }}
            placeholder="LCSC ID (e.g. C83091)..."
            title="Fetch component from LCSC via easyeda2kicad plugin"
            onKeyDown={e => {
              if (e.key === 'Enter') {
                const v = e.target.value.trim()
                if (v) {
                  toastMsg('Plugin: fetching ' + v + '...')
                  api(`${API}/plugins/execute?id=com.kicad_forge.lcsc_import`, {
                    method: 'POST',
                    body: JSON.stringify({ lcsc_id: v })
                  })
                    .then(d => {
                      if (d?.ok) { toastMsg('Plugin: imported ' + (d.created||0) + ' items'); loadSymbols(); loadStatus() }
                      else toastMsg('Plugin: ' + (d?.error||d?.messages?.join?.(' ')||'failed'))
                    })
                  e.target.value = ''
                }
              }
            }}
          />
          <span className="sep" />
          <input
            placeholder="Search symbols..."
            value={search}
            onChange={e => { setSearch(e.target.value); loadSymbols(e.target.value) }}
          />
          <span className="sep" />
          <span style={{ fontSize: 11, color: 'var(--text2)', whiteSpace: 'nowrap' }}>
            {status.symbols} symbols · {status.footprints} footprints
          </span>
        </div>

        <div className="content">
          <table>
            <thead>
              <tr>
                <th style={{ width: 36 }}></th>
                <th>Name</th>
                <th style={{ width: 140 }}>Type</th>
                <th style={{ width: 200 }}>Footprint</th>
                <th style={{ width: 64, textAlign: 'center' }}>Pins</th>
                <th style={{ width: 160 }}>MPN</th>
              </tr>
            </thead>
            <tbody>
              {filtered.length === 0 ? (
                <tr>
                  <td colSpan={6}>
                    <div className="empty-state">
                      <div className="icon">
                        <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="9" y1="9" x2="15" y2="9"/><line x1="9" y1="13" x2="15" y2="13"/><line x1="9" y1="17" x2="12" y2="17"/></svg>
                      </div>
                      <p style={{ fontWeight: 600 }}>No symbols loaded</p>
                      <p>Click <strong>Settings</strong> in the sidebar to configure your symbol library path, then click <strong>Save & Import</strong>.</p>
                    </div>
                  </td>
                </tr>
              ) : (
                filtered.map(s => (
                  <tr key={s.id} className={selected?.id === s.id ? 'selected' : ''} onClick={() => setSelected(s)}>
                    <td>
                      {s.has_3d_model
                        ? <span style={{ color: 'var(--green)', fontSize: 14 }} title="3D model linked">●</span>
                        : s.has_footprint
                          ? <span style={{ color: 'var(--orange)', fontSize: 14 }} title="Footprint assigned, no 3D">●</span>
                          : <span style={{ color: 'var(--red)', fontSize: 14 }} title="No footprint">●</span>
                      }
                    </td>
                    <td className="name">{s.name}</td>
                    <td><span className="type-tag">{s.type || 'Unknown'}</span></td>
                    <td style={{ fontFamily: 'var(--mono)', fontSize: 11 }}>{s.footprint || '—'}</td>
                    <td style={{ textAlign: 'center', fontFamily: 'var(--mono)' }}>{s.pins || '—'}</td>
                    <td style={{ fontFamily: 'var(--mono)', fontSize: 11 }}>{s.mpn || '—'}</td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
        </div>

        <div className="statusbar">
          <span>{filtered.length} components{filter ? ` · filtered by "${filter}"` : ''}</span>
          <span style={{ marginLeft: 'auto' }}>{symbols.length} total</span>
        </div>
      </div>

      {/* Inspector */}
      <div className="inspector">
        {selected ? (
          <>
            <div className="inspector-header">
              <div className="inspector-icon">{TYPE_ICONS[selected.type] || '○'}</div>
              <div>
                <h4>{selected.name}</h4>
                <div className="inspector-type">{selected.type || 'Unknown'}</div>
              </div>
            </div>

            <div className="inspector-section">
              <div className="field">
                <label>Footprint</label>
                <div className={`val ${selected.footprint ? 'ok' : 'warn'}`}>
                  {selected.footprint || 'Not assigned'}
                </div>
              </div>
              <div className="field">
                <label>3D Model</label>
                <div className={`val ${selected.has_3d_model ? 'ok' : 'warn'}`}>
                  {selected.has_3d_model ? '✓ Linked' : '⚠ Not assigned'}
                </div>
              </div>
              <div className="field">
                <label>Pin Count</label>
                <div className="val">{selected.pins || '—'}</div>
              </div>
              <div className="field">
                <label>MPN</label>
                <div className="val" style={{ fontSize: 12 }}>{selected.mpn || '—'}</div>
              </div>
              <div className="field">
                <label>Value</label>
                <div className="val" style={{ fontSize: 12 }}>{selected.value || '—'}</div>
              </div>
            </div>

            <div className="inspector-section">
              <h5>Quick Actions</h5>
              <div className="actions">
                <button className="primary" onClick={classify}>Auto Classify</button>
                <button onClick={autoMatch}>Find Matching Footprint</button>
              </div>
            </div>
          </>
        ) : (
          <div className="inspector-empty">
            <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><circle cx="12" cy="12" r="10"/><path d="M8 14s1.5 2 4 2 4-2 4-2"/><line x1="9" y1="9" x2="9.01" y2="9"/><line x1="15" y1="9" x2="15.01" y2="9"/></svg>
            <p>Select a component<br/>to view details</p>
          </div>
        )}
      </div>

      {/* Toast */}
      <div id="toast" className={toast ? 'show' : ''}>{toast}</div>

      {/* Settings Modal */}
      {showSettings && (
        <Modal title="Settings" onClose={() => setShowSettings(false)}>
          <div className="form-group">
            <label>Symbol Library Path</label>
            <input value={settings.symbol_lib_path || ''} onChange={e => setSettings({ ...settings, symbol_lib_path: e.target.value })} placeholder="e.g. C:\Users\xiaom\Desktop\EXT_Internal_kicad_Lib\Symbols" />
            <span className="form-hint">Directory containing .kicad_sym files</span>
          </div>
          <div className="form-group">
            <label>Footprint Library Path</label>
            <input value={settings.footprint_lib_path || ''} onChange={e => setSettings({ ...settings, footprint_lib_path: e.target.value })} placeholder="e.g. C:\KiCad\footprints" />
            <span className="form-hint">Directory containing .kicad_mod files</span>
          </div>
          <div className="form-group">
            <label>3D Model Path</label>
            <input value={settings.model_3d_path || ''} onChange={e => setSettings({ ...settings, model_3d_path: e.target.value })} placeholder="e.g. C:\KiCad\3dmodels" />
            <span className="form-hint">Directory containing .step / .wrl files</span>
          </div>
          <div className="btn-row">
            <button className="btn-ghost" onClick={() => setShowSettings(false)}>Cancel</button>
            <button className="btn-primary" onClick={saveSettings}>Save & Import</button>
          </div>
        </Modal>
      )}

      {/* Rules Modal */}
      {showRules && (
        <Modal title="Classification Rules" onClose={() => setShowRules(false)}>
          <div className="rules-list">
            {rules.map((r, i) => (
              <div key={i} className="rule-item">
                <div className="rule-name">{r.name}</div>
                <div className="rule-arrow">→</div>
                <div className="rule-target">{r.target}</div>
                <div className="rule-confidence">{r.confidence}%</div>
              </div>
            ))}
            {rules.length === 0 && <p style={{ color: 'var(--text3)', textAlign: 'center', padding: 20 }}>No rules configured</p>}
          </div>
          <p style={{ fontSize: 11, color: 'var(--text2)', marginTop: 12 }}>Edit <code>config/classification_rules/*.toml</code> to add custom rules</p>
        </Modal>
      )}

      {/* Plugins Modal */}
      {showPlugins && (
        <Modal title="Plugins" onClose={() => setShowPlugins(false)}>
          {plugins.map((p, i) => (
            <div key={i} className="plugin-card">
              <div className="plugin-icon">
                <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/></svg>
              </div>
              <div className="plugin-info">
                <div className="plugin-name">{p.name}</div>
                <div className="plugin-meta">{p.id} · v{p.version} · <span className={`status-${p.status}`}>{p.status}</span></div>
              </div>
            </div>
          ))}
          {plugins.length === 0 && (
            <div style={{ textAlign: 'center', padding: 30, color: 'var(--text3)' }}>
              <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/></svg>
              <p style={{ marginTop: 12 }}>No plugins installed</p>
              <p style={{ fontSize: 11 }}>Place .kfplug files in the plugins directory</p>
            </div>
          )}
        </Modal>
      )}

      {/* Component Types Modal */}
      {showCompTypes && <TypeModal title="Component Types" items={compTypes} onClose={() => setShowCompTypes(false)}
        onAdd={(name) => manageType(`${API}/component-types`, 'add', name).then(ok => ok && loadCompTypes())}
        onRemove={(name) => manageType(`${API}/component-types`, 'remove', name).then(ok => ok && loadCompTypes())}
        showIcon={true} />}

      {/* Package Types Modal */}
      {showPkgTypes && <TypeModal title="Package Types" items={pkgTypes} onClose={() => setShowPkgTypes(false)}
        onAdd={(name) => manageType(`${API}/package-types`, 'add', name).then(ok => ok && loadPkgTypes())}
        onRemove={(name) => manageType(`${API}/package-types`, 'remove', name).then(ok => ok && loadPkgTypes())}
        showCategory={true} />}
    </div>
  )
}

function Modal({ title, children, onClose }) {
  return (
    <div className="modal-overlay show" onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div className="modal">
        <div className="modal-header">
          <h3>{title}</h3>
          <button className="modal-close" onClick={onClose}>
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
          </button>
        </div>
        {children}
      </div>
    </div>
  )
}

function TypeModal({ title, items, onClose, onAdd, onRemove, showIcon, showCategory }) {
  const [newName, setNewName] = useState('')
  const [newCat, setNewCat] = useState('Other')

  function handleAdd() {
    if (!newName.trim()) return
    if (showCategory) onAdd(newName.trim(), newCat)
    else onAdd(newName.trim())
    setNewName('')
  }

  return (
    <Modal title={title} onClose={onClose}>
      <div className="rules-list" style={{ maxHeight: 350, overflow: 'auto' }}>
        {items.map((t, i) => (
          <div key={i} className="rule-item">
            {showIcon && <span style={{ fontSize: 16, width: 24 }}>{t.icon || '○'}</span>}
            <span className="rule-name">{t.name}</span>
            {showCategory && <span style={{ fontSize: 11, color: 'var(--text2)' }}>{t.category}</span>}
            <button className="btn-ghost" style={{ padding: '3px 10px', fontSize: 11, color: 'var(--red)' }}
              onClick={() => onRemove(t.name)}>×</button>
          </div>
        ))}
      </div>
      <div style={{ display: 'flex', gap: 8, marginTop: 16 }}>
        <input placeholder="New type name..." value={newName}
          onChange={e => setNewName(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && handleAdd()}
          style={{ flex: 1, padding: '6px 10px', border: '1px solid var(--sep)', borderRadius: 6, fontSize: 12, background: 'var(--bg)', color: 'var(--text)' }} />
        {showCategory && (
          <select value={newCat} onChange={e => setNewCat(e.target.value)}
            style={{ padding: '6px 8px', border: '1px solid var(--sep)', borderRadius: 6, fontSize: 12, background: 'var(--bg)', color: 'var(--text)' }}>
            <option>Other</option><option>SMD Chip</option><option>SMD IC</option>
            <option>Through-hole</option><option>Connector</option>
          </select>
        )}
        <button className="btn-primary" style={{ padding: '6px 14px', fontSize: 12 }} onClick={handleAdd}>Add</button>
      </div>
      <p style={{ fontSize: 11, color: 'var(--text2)', marginTop: 8 }}>
        Types are saved to <code>config/*_types.json</code>. Changes take effect immediately.
      </p>
    </Modal>
  )
}
