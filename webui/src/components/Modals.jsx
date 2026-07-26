import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'

// ---- Generic Modal shell ----
export function Modal({ title, children, onClose }) {
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

// ---- Settings Modal ----
export function SettingsModal({ settings, setSettings, onClose, loadSymbols, loadStatus, toastMsg }) {
  const [tab, setTab] = useState('paths')
  const { t, i18n } = useTranslation()
  const save = async () => {
    const r = await api.saveSettings(settings)
    if (r) {
      toastMsg(t('toast.saved', { symbols: r.imported_symbols || 0, footprints: r.imported_footprints || 0 }))
      onClose(); loadSymbols(); loadStatus()
    }
  }
  return (
    <Modal title={t('settings.title')} onClose={onClose}>
      <div style={{display:'flex', gap:16}}>
        <div style={{width:140, borderRight:'1px solid var(--sep)', paddingRight:12}}>
          {['paths','utils'].map(k => (
            <div key={k} onClick={() => setTab(k)} style={{padding:'8px 10px',borderRadius:6,cursor:'pointer',fontSize:12,fontWeight:tab===k?600:400,background:tab===k?'var(--accent)':'transparent',color:tab===k?'#fff':'var(--text)'}}>
              {k==='paths'?'📁 '+t('settings.libraryPaths'):'🔧 '+t('settings.utilities')}
            </div>
          ))}
        </div>
        <div style={{flex:1,minHeight:200}}>
          {tab==='paths'?<>
            <Field k="symbol_lib_path" label={t('settings.symbolPath')} hint={t('settings.symbolPathHint')} {...{settings,setSettings}} />
            <Field k="footprint_lib_path" label={t('settings.footprintPath')} hint={t('settings.footprintPathHint')} {...{settings,setSettings}} />
            <Field k="model_3d_path" label={t('settings.3dPath')} hint={t('settings.3dPathHint')} {...{settings,setSettings}} />
            <div className="btn-row"><button className="btn-primary" onClick={save}>{t('settings.saveImport')}</button></div>
          </>:<>
            <div style={{display:'flex',flexDirection:'column',gap:12}}>
              <UtilBox title={t('settings.resetDb')} desc={t('settings.resetDbHint')}>
                <button style={{padding:'6px 14px',border:'1px solid var(--red)',borderRadius:6,color:'var(--red)',background:'transparent',cursor:'pointer'}}
                  onClick={async()=>{if(!confirm(t('confirm.resetDb')))return;const r=await api.resetDb();toastMsg(r?.ok?r.message:'Reset failed')}}>{t('settings.resetDbButton')}</button>
              </UtilBox>
              <UtilBox title={t('settings.reimportAll')} desc={t('settings.reimportHint')}>
                <button className="btn-primary" style={{padding:'6px 14px'}} onClick={save}>{t('settings.reimportButton')}</button>
              </UtilBox>
              <UtilBox title="Language" desc="">
                <select value={i18n.language} onChange={e => i18n.changeLanguage(e.target.value)}
                  style={{padding:'6px 10px',border:'1px solid var(--sep)',borderRadius:6,fontSize:12,background:'var(--bg)',color:'var(--text)'}}>
                  <option value="en">English</option>
                  <option value="zh">中文</option>
                </select>
              </UtilBox>
              <UtilBox title={t('settings.dbLocation')} desc="%APPDATA%/kicad_forge/meta.db" />
            </div>
          </>}
        </div>
      </div>
    </Modal>
  )
}
function Field({ k, label, hint, settings, setSettings }) {
  return (
    <div className="form-group">
      <label>{label}</label>
      <input value={settings[k]||''} onChange={e => setSettings({...settings,[k]:e.target.value})} placeholder="e.g. C:\KiCad\symbols" />
      {hint && <span className="form-hint">{hint}</span>}
    </div>
  )
}
function UtilBox({ title, desc, children }) {
  return <div style={{padding:12,border:'1px solid var(--sep)',borderRadius:8}}>
    <strong>{title}</strong>
    <p style={{fontSize:11,color:'var(--text2)',margin:'4px 0',wordBreak:'break-all'}}>{desc}</p>
    {children}
  </div>
}

// ---- Rules Modal ----
export function RulesModal({ rules, onClose }) {
  const { t } = useTranslation()
  return (
    <Modal title={t('rules.title')} onClose={onClose}>
      <div className="rules-list">
        {rules.map((r,i)=>(<div key={i} className="rule-item"><div className="rule-name">{r.name}</div><div className="rule-arrow">→</div><div className="rule-target">{r.target}</div><div className="rule-confidence">{r.confidence}%</div></div>))}
        {rules.length===0 && <p style={{color:'var(--text3)',textAlign:'center',padding:20}}>{t('rules.noRules')}</p>}
      </div>
      <p style={{fontSize:11,color:'var(--text2)',marginTop:12}}>{t('rules.editHint')}</p>
    </Modal>
  )
}

// ---- Plugins Modal ----
export function PluginsModal({ plugins, onClose }) {
  const { t } = useTranslation()
  return (
    <Modal title={t('sidebar.plugins')} onClose={onClose}>
      {plugins.map((p,i)=>(
        <div key={i} className="plugin-card">
          <div className="plugin-icon">
            {p.icon_url?<img src={p.icon_url} alt={p.name} width="24" height="24" style={{objectFit:'contain'}}/>:
            <svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/><line x1="12" y1="22" x2="12" y2="15.5"/><polyline points="22 8.5 12 15.5 2 8.5"/></svg>}
          </div>
          <div className="plugin-info">
            <div className="plugin-name">{p.name}</div>
            <div className="plugin-meta">{p.id} · v{p.version} · <span className={`status-${p.status}`}>{p.status}</span></div>
            {p.description && <div className="plugin-meta" style={{marginTop:2}}>{p.description}</div>}
          </div>
        </div>
      ))}
      {plugins.length===0 && <div style={{textAlign:'center',padding:30,color:'var(--text3)'}}>
        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><polygon points="12 2 22 8.5 22 15.5 12 22 2 15.5 2 8.5 12 2"/></svg>
        <p style={{marginTop:12}}>{t('plugin.noPlugins')}</p>
        <p style={{fontSize:11}}>{t('plugin.pluginHint')}</p>
      </div>}
    </Modal>
  )
}

// ---- Type Modal ----
export function TypeModal({ title, items, onClose, onAdd, onRemove, showIcon, showCategory }) {
  const [newName, setNewName] = useState('')
  const [newCat, setNewCat] = useState('Other')
  const { t } = useTranslation()
  function handleAdd() { if(!newName.trim())return; onAdd(showCategory?[newName.trim(),newCat]:[newName.trim()]); setNewName('') }
  return (
    <Modal title={title} onClose={onClose}>
      <div className="rules-list" style={{maxHeight:350,overflow:'auto'}}>
        {items.map((t,i)=>(<div key={i} className="rule-item">
          {showIcon&&<span style={{fontSize:16,width:24}}>{t.icon||'○'}</span>}
          <span className="rule-name">{t.name}</span>
          {showCategory&&<span style={{fontSize:11,color:'var(--text2)'}}>{t.category}</span>}
          <button className="btn-ghost" style={{padding:'3px 10px',fontSize:11,color:'var(--red)'}} onClick={()=>onRemove(t.name)}>×</button>
        </div>))}
      </div>
      <div style={{display:'flex',gap:8,marginTop:16}}>
        <input placeholder={t('types.newTypePlaceholder')} value={newName}
          onChange={e=>setNewName(e.target.value)} onKeyDown={e=>e.key==='Enter'&&handleAdd()}
          style={{flex:1,padding:'6px 10px',border:'1px solid var(--sep)',borderRadius:6,fontSize:12,background:'var(--bg)',color:'var(--text)'}}/>
        {showCategory&&<select value={newCat} onChange={e=>setNewCat(e.target.value)}
          style={{padding:'6px 8px',border:'1px solid var(--sep)',borderRadius:6,fontSize:12,background:'var(--bg)',color:'var(--text)'}}>
          <option>Other</option><option>SMD Chip</option><option>SMD IC</option><option>Through-hole</option><option>Connector</option></select>}
        <button className="btn-primary" style={{padding:'6px 14px',fontSize:12}} onClick={handleAdd}>{t('types.add')}</button>
      </div>
      <p style={{fontSize:11,color:'var(--text2)',marginTop:8}}>{t('types.saveHint')}</p>
    </Modal>
  )
}

// ---- Plugin Action Modal ----
export function PluginActionModal({ pluginAction, setPluginAction, pluginFormData, setPluginFormData, libraries, toastMsg, loadSymbols, loadLibraries, loadStatus }) {
  const { t } = useTranslation()
  if (!pluginAction) return null
  const { plugin, action } = pluginAction
  const fields = action.schema?.fields || []

  async function execute() {
    for (const f of fields) { if (f.required && !pluginFormData[f.key]?.trim()) { toastMsg(t('toast.required', { label: f.label })); return } }
    setPluginAction(null); toastMsg(t('toast.executing'))
    const body = { action: action.id, ...pluginFormData }
    if (body.target_library) { const lib = libraries.find(l => l.id === body.target_library); if (lib) body.target_library = lib.file_path || body.target_library }
    const r = await api.executePlugin(plugin.id, body)
    if (r?.ok) { toastMsg(t('toast.done', { count: (r.imported_symbols||0) || (r.library_symbols||0) || '' })); loadSymbols(); loadLibraries(); loadStatus() }
    else toastMsg(t('toast.failed', { error: r?.error || 'unknown' }))
  }

  return (
    <Modal title={action.name} onClose={() => setPluginAction(null)}>
      {fields.map(f => (
        <div key={f.key} className="form-group">
          <label>{f.label}{f.required ? ' *' : ''}</label>
          {f.type === 'library_picker' ? (
            <select value={pluginFormData[f.key]||''} onChange={e => setPluginFormData({...pluginFormData,[f.key]:e.target.value})}
              style={{width:'100%',padding:'8px 12px',border:'1px solid var(--sep)',borderRadius:7,fontSize:12.5,background:'var(--bg)',color:'var(--text)'}}>
              <option value="">{t('plugin.select')}</option>
              {libraries.map(l => <option key={l.id} value={l.id}>{l.name} ({l.symbol_count||0})</option>)}
            </select>) : (
            <input value={pluginFormData[f.key]||''} onChange={e => setPluginFormData({...pluginFormData,[f.key]:e.target.value})}
              placeholder={f.key} autoFocus onKeyDown={e => { if (e.key === 'Enter') execute() }} />
          )}
        </div>
      ))}
      <div className="btn-row">
        <button className="btn-ghost" onClick={() => setPluginAction(null)}>{t('plugin.cancel')}</button>
        <button className="btn-primary" onClick={execute}>{t('plugin.execute')}</button>
      </div>
    </Modal>
  )
}
