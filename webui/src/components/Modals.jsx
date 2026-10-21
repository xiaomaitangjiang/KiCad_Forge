import { useState, useEffect, useRef, useCallback } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import { PluginSlot } from './PluginSlot'
import CenterPage from '../workbench/CenterPage'
import { useOverlayLayer } from '../workbench/overlay'
import { translateMatchingMessage } from '../lib/typeMatching'
import {
  TYPE_ICON_PLACEHOLDER, TYPE_ICON_MAX_BYTES, iconByteLength, normalizeColor,
  validateColorInput, validateIconInput, buildTypePatch, buildAddPayload, patchHasChanges,
  changedTypeFields,
} from '../lib/typeMetadata'

// ---- Generic Modal shell ----
// Dialog semantics + keyboard flow: Escape closes, focus moves into the
// dialog on open and returns to the trigger on close.
//
// onEscape（可选）只覆盖 Escape：嵌套弹窗里 Escape 应该先关最内层，
// 而 X / 遮罩仍走 onClose。默认行为与只有 onClose 时完全一致。
const FOCUSABLE = [
  'button:not([disabled])', '[href]', 'input:not([disabled]):not([type="hidden"])',
  'select:not([disabled])', 'textarea:not([disabled])', '[tabindex]:not([tabindex="-1"])',
].join(',')

export function Modal({ title, children, onClose, onEscape, className = '' }) {
  const { t } = useTranslation()
  const ref = useRef(null)
  // 关闭回调始终取「本次渲染」的那一个：Escape 监听只注册一次（effect []），
  // 若缓存首次的 onClose，嵌套弹窗里读到的旧闭包会绕过最新的冻结判断。
  const latest = useRef({ onClose, onEscape })
  latest.current = { onClose, onEscape }
  const requestClose = useCallback(() => { latest.current.onClose?.() }, [])
  const requestEscape = useCallback(() => { (latest.current.onEscape || latest.current.onClose)?.() }, [])
  // 有统一浮层栈时只让栈顶响应 Escape；单独挂 Modal 的测试保留 document 监听。
  const inOverlayStack = useOverlayLayer(requestEscape)
  useEffect(() => {
    const previous = document.activeElement
    const first = ref.current?.querySelector('[data-autofocus]')
      || ref.current?.querySelector('input, select, textarea, button')
    ;(first || ref.current)?.focus?.()
    return () => { previous?.focus?.() }
  }, [])
  useEffect(() => {
    if (inOverlayStack) return undefined
    const onKey = (e) => { if (e.key === 'Escape') { e.stopPropagation(); requestEscape() } }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [inOverlayStack, requestEscape])
  // 焦点循环只作用在弹窗内部；背景由遮罩阻断，不把页面其余控件卷进 Tab 顺序。
  const trapFocus = (event) => {
    if (event.key !== 'Tab' || !ref.current) return
    const nodes = [...ref.current.querySelectorAll(FOCUSABLE)]
      .filter(node => node.getClientRects().length > 0)
    if (!nodes.length) { event.preventDefault(); ref.current.focus(); return }
    const first = nodes[0]
    const last = nodes[nodes.length - 1]
    const active = document.activeElement
    if (event.shiftKey && (active === first || !ref.current.contains(active))) {
      event.preventDefault(); last.focus()
    } else if (!event.shiftKey && (active === last || !ref.current.contains(active))) {
      event.preventDefault(); first.focus()
    }
  }
  return (
    <div className="modal-overlay show" onClick={e => { if (e.target === e.currentTarget) requestClose() }}>
      <div className={`modal ${className}`.trim()} ref={ref} role="dialog" aria-modal="true"
        aria-label={title} tabIndex={-1} onKeyDown={trapFocus}>
        <div className="modal-header">
          <h3>{title}</h3>
          <button className="modal-close" aria-label={t('inspector.close')} onClick={requestClose}>
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>
          </button>
        </div>
        {children}
      </div>
    </div>
  )
}

// ---- Confirm Dialog ----
// 短确认也走统一 Modal：Escape 只关最上层、焦点进入确认按钮并在关闭后回到触发点。
export function ConfirmDialog({
  title, message, confirmLabel, cancelLabel, danger = false, onConfirm, onCancel,
}) {
  return (
    <Modal title={title} className="confirm-modal" onClose={onCancel}>
      <p className="confirm-message">{message}</p>
      <div className="btn-row confirm-actions">
        <button type="button" className="btn-ghost" data-autofocus onClick={onCancel}>{cancelLabel}</button>
        <button type="button" className={`btn-primary${danger ? ' confirm-danger' : ''}`}
          onClick={onConfirm}>{confirmLabel}</button>
      </div>
    </Modal>
  )
}

// ---- Settings Modal ----
export function SettingsModal({ settings, setSettings, compLibraries, loadCompLibraries, onClose, refreshSymbols, loadStatus, toastMsg, showConfirm, onRequestDelete, onOpenTool, workspaceId, dark = false, onToggleTheme = () => {}, inline = false }) {
  const [tab, setTab] = useState('paths')
  const [editingId, setEditingId] = useState(null)  // which library is being edited
  const [draftName, setDraftName] = useState('')
  const [draftSym, setDraftSym] = useState('')
  const [draftFp, setDraftFp] = useState('')
  const [draftM3d, setDraftM3d] = useState('')
  const [busy, setBusy] = useState('')
  const [actionError, setActionError] = useState('')
  const operationRef = useRef('')
  const mountedRef = useRef(true)
  const { t, i18n } = useTranslation()

  useEffect(() => {
    mountedRef.current = true
    return () => { mountedRef.current = false }
  }, [])

  const requireSuccess = (result) => {
    if (result && typeof result === 'object' && !Array.isArray(result) && result.ok === true) return
    throw Object.assign(new Error(typeof result?.error === 'string' && result.error
      ? result.error : t('settings.invalidResponse')), { payload: result })
  }
  // 同步占位覆盖尚未重绘的连点，也覆盖另一页签里的导入入口。
  // 关闭窗口不会取消已发送的任务；卸载后的回包不能关闭后来打开的新窗口。
  const runOperation = async (operation, task) => {
    if (operationRef.current || !mountedRef.current) return
    operationRef.current = operation
    setBusy(operation)
    setActionError('')
    try {
      await task()
    } catch (error) {
      const detail = error?.kind === 'network' ? t('settings.networkError')
        : error?.kind === 'parse' ? t('settings.invalidResponse')
        : error?.message || t('toast.failedUnknown')
      const pathError = error?.payload?.error_code === 'invalid_library_path'
        && error?.payload?.field === 'footprint_path'
      if (mountedRef.current) setActionError(pathError ? `${t('settings.footprintPathError')}\n${detail}` : detail)
    } finally {
      operationRef.current = ''
      if (mountedRef.current) setBusy('')
    }
  }

  const startEdit = (lib) => {
    if (operationRef.current) return
    if (lib.locked) { toastMsg(t('toast.locked')); return }
    setActionError('')
    setEditingId(lib.id)
    setDraftName(lib.name)
    setDraftSym(lib.symbol_path || '')
    setDraftFp(lib.footprint_path || '')
    setDraftM3d(lib.model_3d_path || '')
  }
  const cancelEdit = () => { setEditingId(null) }

  const saveLib = () => {
    if (operationRef.current) return
    if (!draftName.trim()) { setActionError(t('toast.required', { label: t('settings.libName') })); return }
    const body = editingId === 'new'
      ? { action: 'add', name: draftName.trim(), symbol_path: draftSym, footprint_path: draftFp, model_3d_path: draftM3d }
      : { action: 'update', id: editingId, name: draftName.trim(), symbol_path: draftSym, footprint_path: draftFp, model_3d_path: draftM3d }
    return runOperation('saving', async () => {
      const r = await api.saveCompLibrary(body)
      if (!mountedRef.current) return
      requireSuccess(r)
      cancelEdit(); loadCompLibraries(); toastMsg(t(editingId === 'new' ? 'toast.added' : 'toast.saved', { name: draftName }))
    })
  }

  // Remove (unregister) a component library from the DB. Locked libraries
  // can also be removed — this only clears the DB registration and does NOT
  // touch the user's local .kicad_sym files on disk.
  // Two-step via DeleteLibraryModal. This entry removes a *component*
  // library (registration only — no files are touched).
  const removeLib = (lib) => { if (!operationRef.current) onRequestDelete({ kind: 'component', lib }) }

  // Toggle library lock; official KiCad libs cannot be unlocked
  const toggleLock = (lib) => {
    if (operationRef.current) return
    if (lib.is_protected) { toastMsg(t('settings.officialLocked')); return }
    return runOperation('saving', async () => {
      const r = await api.saveCompLibrary({ action: 'update', id: lib.id, locked: !lib.locked })
      if (!mountedRef.current) return
      requireSuccess(r)
      loadCompLibraries(); toastMsg(t(lib.locked ? 'settings.unlocked' : 'settings.locked'))
    })
  }

  const importAll = () => runOperation('importing', async () => {
    const r = await api.saveCompLibrary({ action: 'import' })
    requireSuccess(r)
    // 全局刷新读取的是最新查询，即使用户已关闭设置也要发布导入结果。
    refreshSymbols(); loadStatus()
    if (!mountedRef.current) return
    toastMsg(t('toast.saved', { symbols: r.imported_symbols || 0, footprints: r.imported_footprints || 0 }))
    onClose()
  })

  const resetDatabase = async () => {
    if (operationRef.current || !(await showConfirm(t('confirm.resetDb')))) return
    return runOperation('saving', async () => {
      const r = await api.resetDb()
      if (!mountedRef.current) return
      requireSuccess(r)
      toastMsg(r.message)
    })
  }

  // 打开设置时加载元件库列表
  useEffect(() => { loadCompLibraries() }, []) // eslint-disable-line

  const Frame = inline ? CenterPage : Modal
  return (
    <Frame title={t('settings.title')} onClose={onClose}
      className={inline ? 'settings-page settings-modal' : 'settings-modal'}>
      {busy && <p role="status" style={{fontSize:12,color:'var(--accent)',margin:'0 0 12px'}}>
        {t(busy === 'importing' ? 'settings.importingStatus' : 'settings.savingStatus')}
      </p>}
      {actionError && <p role="alert" style={{fontSize:12,color:'var(--red)',margin:'0 0 12px',whiteSpace:'pre-wrap',overflowWrap:'anywhere'}}>
        {actionError}
      </p>}
      <fieldset disabled={!!busy} style={{border:0,padding:0,margin:0,minWidth:0}} aria-busy={!!busy}>
      <div style={{display:'flex', gap:16}}>
        <div style={{width:140, borderRight:'1px solid var(--sep)', paddingRight:12}}>
          {['paths','utils'].map(k => (
            <div key={k} onClick={() => setTab(k)} style={{padding:'8px 10px',borderRadius:6,cursor:'pointer',fontSize:12,fontWeight:tab===k?600:400,background:tab===k?'var(--accent)':'transparent',color:tab===k?'#fff':'var(--text)'}}>
              {k==='paths'?'📁 '+t('settings.libraryPaths'):'🔧 '+t('settings.utilities')}
            </div>
          ))}
        </div>
        <div style={{flex:1,minHeight:200,overflow:'auto'}}>
          {tab==='paths'?<>
            <div style={{marginBottom:12}}>
              <p style={{fontSize:12,color:'var(--text2)',margin:'0 0 8px 0'}}>{t('settings.compLibHint')}</p>
              <p style={{fontSize:11,color:'var(--text2)',margin:'0 0 4px 0'}}>{t('settings.symbolPathHint')}</p>
              <p style={{fontSize:11,color:'var(--text2)',margin:'0 0 12px 0'}}>{t('settings.footprintPathHint')}</p>
              <p style={{fontSize:11,color:'var(--red)',margin:'0 0 12px 0',padding:'8px 10px',border:'1px solid var(--red)',borderRadius:6,background:'rgba(220,38,38,0.06)'}}>
                {t('settings.standardLibraryGuard')}
              </p>

              {/* Existing component libraries */}
              {compLibraries.map(lib => (
                <div key={lib.id} style={{padding:'8px 10px',border:'1px solid var(--sep)',borderRadius:8,marginBottom:8,background:editingId === lib.id ? 'var(--accent-soft)' : 'transparent'}}>
                  {editingId === lib.id ? (
                    <div style={{display:'flex',flexDirection:'column',gap:6}}>
                      <input value={draftName} onChange={e => setDraftName(e.target.value)} placeholder={t('settings.libName')}
                        style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                      <input value={draftSym} onChange={e => setDraftSym(e.target.value)} placeholder={t('settings.symbolPath')}
                        style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                      <input value={draftFp} onChange={e => setDraftFp(e.target.value)} placeholder={t('settings.footprintPath')}
                        style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                      <input value={draftM3d} onChange={e => setDraftM3d(e.target.value)} placeholder={t('settings.3dPath')}
                        style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                      <div style={{display:'flex',gap:6,marginTop:2}}>
                        <button className="btn-primary" disabled={!!busy} style={{padding:'4px 12px',fontSize:11}} onClick={saveLib}>✓ Save</button>
                        <button className="btn-ghost" style={{padding:'4px 12px',fontSize:11}} onClick={cancelEdit}>✕ Cancel</button>
                      </div>
                    </div>
                  ) : (
                    <div style={{display:'flex',justifyContent:'space-between',alignItems:'center'}}>
                      <div style={{flex:1,minWidth:0}}>
                        <div style={{fontSize:13,fontWeight:600,whiteSpace:'nowrap',overflow:'hidden',textOverflow:'ellipsis'}}>
                          {lib.locked ? '🔒 ' : ''}{lib.id === 'default' ? '⭐ ' : '📦 '}{lib.name}
                        </div>
                        <div style={{fontSize:10,color:'var(--text3)',marginTop:2,lineHeight:1.4}}>
                          {lib.symbol_path && <div>⚡ {t('settings.symbol')}: {lib.symbol_path}</div>}
                          {lib.footprint_path && lib.footprint_path !== lib.symbol_path && <div>🦶 {t('settings.footprint')}: {lib.footprint_path}</div>}
                          {lib.model_3d_path && <div>🧊 {t('settings.3d')}: {lib.model_3d_path}</div>}
                          {!lib.symbol_path && !lib.footprint_path && !lib.model_3d_path && <div style={{color:'var(--text3)'}}>{t('settings.noPaths')}</div>}
                        </div>
                      </div>
                      <div style={{display:'flex',gap:4,flexShrink:0}}>
                        <button className="btn-ghost" style={{padding:'3px 8px',fontSize:11,opacity: lib.is_protected ? 0.5 : 1}}
                          onClick={() => toggleLock(lib)}
                          title={lib.is_protected ? t('settings.officialLocked') : t(lib.locked ? 'settings.unlock' : 'settings.lock')}>
                          {lib.locked ? '🔒' : '🔓'}
                        </button>
                        <button className="btn-ghost" style={{padding:'3px 8px',fontSize:10,opacity: lib.locked ? 0.35 : 1}}
                          onClick={() => startEdit(lib)} title={t('types.add')}>✎</button>
                        <button className="btn-ghost" style={{padding:'3px 8px',fontSize:10,color:'var(--red)',opacity: lib.locked ? 0.35 : 1}}
                          onClick={() => removeLib(lib)} title={t('inspector.deleteSymbol')}>×</button>
                      </div>
                    </div>
                  )}
                </div>
              ))}

              {/* "Add New Library" button or inline form */}
              {editingId === 'new' ? (
                <div style={{padding:'8px 10px',border:'2px dashed var(--accent)',borderRadius:8,marginBottom:8}}>
                  <div style={{display:'flex',flexDirection:'column',gap:6}}>
                    <input value={draftName} onChange={e => setDraftName(e.target.value)} placeholder={t('settings.libName')}
                      style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} autoFocus />
                    <input value={draftSym} onChange={e => setDraftSym(e.target.value)} placeholder={t('settings.symbolPath')+' ('+t('settings.requiredHint')+')'}
                      style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                    <input value={draftFp} onChange={e => setDraftFp(e.target.value)} placeholder={t('settings.footprintPath')}
                      style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                    <input value={draftM3d} onChange={e => setDraftM3d(e.target.value)} placeholder={t('settings.3dPath')}
                      style={{padding:'5px 8px',border:'1px solid var(--sep)',borderRadius:5,fontSize:12,background:'var(--bg)',color:'var(--text)'}} />
                    <div style={{display:'flex',gap:6,marginTop:2}}>
                      <button className="btn-primary" disabled={!!busy} style={{padding:'4px 12px',fontSize:11}} onClick={saveLib}>+ {t('types.add')}</button>
                      <button className="btn-ghost" style={{padding:'4px 12px',fontSize:11}} onClick={cancelEdit}>✕ {t('plugin.cancel')}</button>
                    </div>
                  </div>
                </div>
              ) : (
                <button className="btn-ghost" style={{width:'100%',padding:'8px',fontSize:12,border:'1px dashed var(--sep)',borderRadius:8,color:'var(--accent)'}}
                  onClick={() => { if (operationRef.current) return; setActionError(''); setEditingId('new'); setDraftName(''); setDraftSym(''); setDraftFp(''); setDraftM3d('') }}>
                  + {t('settings.addCompLib')}
                </button>
              )}

              {/* Import button */}
              {!editingId && (
                <div className="btn-row" style={{marginTop:16}}>
                  <button className="btn-primary" disabled={!!busy} onClick={importAll}>
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" style={{marginRight:6,verticalAlign:'middle'}}>
                      <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/>
                    </svg>
                    {t('settings.saveImport')}
                  </button>
                </div>
              )}
            </div>
          </>:<>
            <div style={{display:'flex',flexDirection:'column',gap:12}}>
              {/* Tool entries — moved here from the sidebar */}
              {onOpenTool && (
                <UtilBox title={t('settings.tools')} desc="">
                  <div style={{display:'flex',flexWrap:'wrap',gap:8}}>
                    {[
                      ['rules', t('sidebar.rules')],
                      ['plugins', t('sidebar.plugins')],
                      ['compTypes', t('sidebar.componentTypesMenu')],
                      ['pkgTypes', t('sidebar.packageTypes')],
                    ].map(([key, label]) => (
                      <button key={key} className="btn-ghost"
                        style={{padding:'6px 14px',fontSize:12,border:'1px solid var(--sep)',borderRadius:6}}
                        onClick={() => { if (!inline) onClose(); onOpenTool(key) }}>{label}</button>
                    ))}
                  </div>
                </UtilBox>
              )}
              <PluginSlot slot="settings.section" ownerProps={{ settings, workspaceId: workspaceId || null }} />
              <UtilBox title={t('settings.resetDb')} desc={t('settings.resetDbHint')}>
                <button style={{padding:'6px 14px',border:'1px solid var(--red)',borderRadius:6,color:'var(--red)',background:'transparent',cursor:'pointer'}}
                  onClick={resetDatabase}>{t('settings.resetDbButton')}</button>
              </UtilBox>
              <UtilBox title={t('settings.reimportAll')} desc={t('settings.reimportHint')}>
                <button className="btn-primary" disabled={!!busy} style={{padding:'6px 14px'}} onClick={importAll}>{t('settings.reimportButton')}</button>
              </UtilBox>
              <UtilBox title={t('settings.writeKfId')} desc={t('settings.writeKfIdHint')}>
                <label style={{display:'flex',alignItems:'center',gap:8,cursor:'pointer',fontSize:12}}>
                  <input type="checkbox" checked={!!settings.write_kf_id_to_file}
                    onChange={e => {
                      const v = e.target.checked
                      runOperation('saving', async () => {
                        const r = await api.saveConfig({write_kf_id_to_file: v})
                        if (!mountedRef.current) return
                        requireSuccess(r)
                        setSettings({...settings, write_kf_id_to_file: v})
                      })
                    }} />
                  {t('settings.writeKfIdEnable')}
                </label>
              </UtilBox>
              <UtilBox title={t('settings.appearance')} desc="">
                <label style={{display:'flex',alignItems:'center',gap:8,cursor:'pointer',fontSize:12}}>
                  <input type="checkbox" checked={!!dark} onChange={() => onToggleTheme()} />
                  {t('settings.darkMode')}
                </label>
                <div style={{display:'flex',alignItems:'center',gap:8,marginTop:10,fontSize:12}}>
                  <span>{t('settings.language')}</span>
                  <select className="settings-select" value={i18n.language} onChange={e => i18n.changeLanguage(e.target.value)}>
                    <option value="en">English</option>
                    <option value="zh">中文</option>
                  </select>
                </div>
              </UtilBox>
              <UtilBox title={t('settings.dbLocation')} desc="%APPDATA%/kicad_forge/meta.db" />
            </div>
          </>}
        </div>
      </div>
      </fieldset>
    </Frame>
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
// The legacy Python action interface is gone; this only manages plugin state.
export function PluginsModal({ plugins, onClose, onTogglePlugin }) {
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
            <div className="plugin-meta">{p.id} · v{p.version} · <span className={`status-${p.status}`}>{p.enabled === false ? 'disabled' : p.status}</span></div>
            {onTogglePlugin && (
              <button className="btn-ghost" data-testid={`plugin-toggle-${p.id}`}
                style={{ padding: '3px 10px', fontSize: 11, marginTop: 6 }}
                onClick={() => onTogglePlugin(p)}>
                {p.enabled === false ? t('plugin.enable') : t('plugin.disable')}
              </button>
            )}
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

export function TypeEditorModal({ mode, entry, onClose, onSubmit, onBusyChange, canWrite = true, blockReason = '' }) {
  const { t } = useTranslation()
  const editing = mode === 'edit'
  const [name, setName] = useState(entry?.name || '')
  const [icon, setIcon] = useState(entry?.icon || '')
  const [color, setColor] = useState(entry?.color || '')
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const submitLock = useRef(false)
  const savingRef = useRef(false)

  const setBusy = (next) => {
    savingRef.current = next
    setSaving(next)
    onBusyChange?.(next)
  }
  // 写入在飞时 X / 遮罩 / Escape / 取消全部冻结（savingRef 同步生效，
  // 不依赖 state 何时刷新）——否则旧写入的成功回包会关掉用户后来打开的表单
  const requestClose = () => { if (savingRef.current) return; onClose() }

  // 原生取色器只接受 #RRGGBB：历史颜色不是该格式时显示默认值，
  // 但草稿仍保留原值 —— 没动颜色就不提交颜色，改名不会顺手改色。
  const pickerValue = normalizeColor(color) || '#000000'
  const colorLabel = normalizeColor(color) || t('types.noColor')
  const blocked = !canWrite

  async function submit() {
    if (submitLock.current) return
    const trimmed = name.trim()
    if (!trimmed) { setError(t('types.required.name')); return }
    if (!editing) {
      // 新增：全部字段都要过校验（没有「没动所以跳过」的余地）
      if (validateIconInput(icon)) {
        setError(t('types.invalid.icon', { bytes: iconByteLength(icon), max: TYPE_ICON_MAX_BYTES }))
        return
      }
      if (validateColorInput(color)) { setError(t('types.invalid.color')); return }
    } else {
      // 编辑：只校验这次真正会写下去的字段。先按原文比出变化，再校验新值 ——
      // 只对补丁里存在的字段做校验会让「改成非法值」被当成没改而静默丢弃。
      const changes = changedTypeFields(entry, { name, icon, color })
      if ('icon' in changes && validateIconInput(changes.icon)) {
        setError(t('types.invalid.icon', { bytes: iconByteLength(changes.icon), max: TYPE_ICON_MAX_BYTES }))
        return
      }
      if ('color' in changes && validateColorInput(changes.color)) {
        setError(t('types.invalid.color'))
        return
      }
    }
    const payload = editing
      ? buildTypePatch(entry, { name, icon, color })
      : buildAddPayload({ name, icon, color })
    // 编辑没有任何字段变化：不算保存，也不发请求
    if (editing && !patchHasChanges(payload)) { onClose(); return }
    submitLock.current = true
    setBusy(true)
    setError('')
    try {
      const result = await onSubmit(payload)
      if (!result || result.busy) return      // 已经有写入在飞：等它，不重复提交
      if (!result.ok) {
        setError(result.error || (result.blocked ? t('types.writeBlocked') : t('toast.failedUnknown')))
        return
      }
      onClose()
    } finally {
      submitLock.current = false
      setBusy(false)
    }
  }

  return (
    <Modal title={editing ? t('types.editTitle', { name: entry?.name || '' }) : t('types.addTitle')}
      onClose={requestClose} onEscape={requestClose}>
      <div className="type-form">
        <label className="type-field">
          <span className="type-field-label">{t('types.name')}</span>
          <input className="type-input" data-testid="type-name" value={name} disabled={saving}
            onChange={e => { setName(e.target.value); setError('') }} />
        </label>

        <div className="type-field">
          <label className="type-field-label" htmlFor="type-editor-icon">{t('types.icon')}</label>
          <div className="type-icon-row">
            {/* 预览只是装饰：真正的描述是字段标签 + 提示文字 */}
            <span className="type-icon-preview" aria-hidden="true"
              style={normalizeColor(color) ? { color: normalizeColor(color) } : undefined}>
              {icon || TYPE_ICON_PLACEHOLDER}
            </span>
            <input id="type-editor-icon" className="type-input" value={icon} disabled={saving}
              aria-describedby="type-editor-icon-hint" maxLength={TYPE_ICON_MAX_BYTES}
              onChange={e => { setIcon(e.target.value); setError('') }} />
          </div>
          <p id="type-editor-icon-hint" className="type-hint">
            {t('types.iconHint', { max: TYPE_ICON_MAX_BYTES })}
          </p>
        </div>

        <div className="type-field">
          <label className="type-field-label" htmlFor="type-editor-color">{t('types.color')}</label>
          <div className="type-color-row">
            <input id="type-editor-color" type="color" className="type-color-input" value={pickerValue}
              disabled={saving} aria-describedby="type-editor-color-hint"
              onChange={e => { setColor(e.target.value); setError('') }} />
            <span id="type-editor-color-hint" className="type-color-value" data-testid="type-color-value">
              {colorLabel}
            </span>
            <button type="button" className="btn-ghost" data-testid="type-clear-color" disabled={saving}
              onClick={() => { setColor(''); setError('') }}>{t('types.clearColor')}</button>
          </div>
        </div>

        {/* 快照不可写（已保存待刷新 / 还没加载出有效表）：说清原因，草稿留着 */}
        {blocked && !error && (
          <div className="type-error" role="status" data-testid="type-editor-blocked">
            {blockReason || t('types.writeBlocked')}
          </div>
        )}
        {error && <div className="type-error" role="alert" data-testid="type-editor-error">{error}</div>}
      </div>
      <div className="btn-row" style={{ marginTop: 16, display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
        <button className="btn-ghost" data-testid="type-editor-cancel" disabled={saving} onClick={requestClose}>
          {t('types.cancel')}
        </button>
        <button className="btn-primary" data-testid="type-save" disabled={saving || blocked} onClick={submit}>
          {saving ? t('types.saving') : t('types.save')}
        </button>
      </div>
    </Modal>
  )
}

// ---- Type Modal ----
// showCategory（封装类型）：名称/分类的原有行为不变，不加元件类型字段；
// showIcon（元件类型）：条目用同一份类型配置的图标/颜色 + 编辑入口，
// 新增走同一张编辑器（name/icon/color），删除在内置条目上禁用。
export function TypeModal({
  title, items, onClose, onAdd, onRemove, onSave, showIcon, showCategory,
  loading, loadError, stale, onRetryLoad, canWrite = true,
  onOpenMatching, inline = false,
}) {
  const [newName, setNewName] = useState('')
  const [newCat, setNewCat] = useState('Other')
  const [editor, setEditor] = useState(null)   // null | { mode, entry }
  const [pendingRemove, setPendingRemove] = useState('')
  const [editorBusy, setEditorBusy] = useState(false)
  const removeLock = useRef(false)
  const editorBusyRef = useRef(false)
  const { t } = useTranslation()
  const list = Array.isArray(items) ? items : []
  // 快照不可写：已保存待刷新（只能重读）或还没有有效类型表 ——
  // 写入入口一律冻结，别让用户对着一份旧快照再写一次
  const writeBlocked = !canWrite
  const blockReason = stale
    ? (loadError ? t('types.savedRefreshFailed', { error: loadError }) : t('types.savedPendingRefresh'))
    : t('types.notWritable')

  const closeEditor = useCallback(() => {
    editorBusyRef.current = false
    setEditorBusy(false)
    setEditor(null)
  }, [])
  const handleEditorBusy = useCallback((busy) => {
    editorBusyRef.current = busy
    setEditorBusy(busy)
  }, [])
  // X / 遮罩：写入在飞时整体冻结（子编辑器也是这张弹窗的一部分），
  // 否则连编辑器一起关掉。Escape 只关最内层，交给下面对 onEscape 的处理。
  const requestParentClose = useCallback(() => {
    if (editorBusyRef.current) return
    closeEditor()
    onClose()
  }, [closeEditor, onClose])
  const requestParentEscape = useCallback(() => {
    if (editorBusyRef.current) return
    if (editor) { closeEditor(); return }
    onClose()
  }, [editor, closeEditor, onClose])

  function handleAdd() {
    if (writeBlocked || !newName.trim()) return
    onAdd(showCategory ? [newName.trim(), newCat] : [newName.trim()])
    setNewName('')
  }

  async function handleRemove(item) {
    if (writeBlocked || removeLock.current) return   // 删除期间冻结重复操作
    removeLock.current = true
    setPendingRemove(item.id)
    try {
      await onRemove(item)
    } finally {
      removeLock.current = false
      setPendingRemove('')
    }
  }

  async function handleSubmit(payload) {
    if (!onSave) return { ok: false, error: t('toast.failedUnknown') }
    return onSave(payload)
  }

  const Frame = inline ? CenterPage : Modal
  return (
    <Frame title={title} onClose={requestParentClose} onEscape={requestParentEscape}
      className={inline ? 'types-page' : ''}>
      {/* 已保存但快照没刷新到位：明确说「已保存」，只提供重新读取，绝不让用户再写一次 */}
      {stale && (
        <div className="type-notice" role="status" data-testid="types-stale">
          <span>{loadError
            ? t('types.savedRefreshFailed', { error: loadError })
            : t('types.savedPendingRefresh')}</span>
          <button className="btn-ghost" data-testid="types-retry-load" disabled={!!loading}
            onClick={() => onRetryLoad?.()}>{t('types.retryLoad')}</button>
        </div>
      )}
      {/* 读取失败 ≠ 空类型表：保留上一次快照，把原因和重试入口摆出来 */}
      {!stale && loadError && (
        <div className="type-notice error" role="alert" data-testid="types-load-failed">
          <span>{t('types.loadFailed', { error: loadError })}</span>
          <button className="btn-ghost" data-testid="types-retry-load" disabled={!!loading}
            onClick={() => onRetryLoad?.()}>{t('types.retryLoad')}</button>
        </div>
      )}
      {/* 还没有有效快照（首次读取失败/还在加载）：写入不可用，原因说清楚 */}
      {writeBlocked && !stale && !loadError && (
        <div className="type-notice" role="status" data-testid="types-not-writable">
          <span>{t('types.notWritable')}</span>
          <button className="btn-ghost" data-testid="types-retry-load" disabled={!!loading}
            onClick={() => onRetryLoad?.()}>{t('types.retryLoad')}</button>
        </div>
      )}
      <div className="rules-list" style={{maxHeight:350,overflow:'auto'}}>
        {list.map((item,i)=>(<div key={item.id || i} className="rule-item" data-testid={item.id ? `type-row-${item.id}` : undefined}>
          {showIcon&&<span className="type-icon" aria-hidden="true"
            style={item.color?{color:item.color,fontSize:16,width:24}:{fontSize:16,width:24}}>{item.icon||'○'}</span>}
          <span className="rule-name">{item.name}</span>
          {showCategory&&<span style={{fontSize:11,color:'var(--text2)'}}>{item.category}</span>}
          {showIcon&&(
            <button className="btn-ghost type-edit" style={{padding:'3px 10px',fontSize:11}}
              aria-label={`${t('types.edit')} ${item.name||''}`} title={t('types.edit')}
              disabled={!!pendingRemove || writeBlocked || editorBusy}
              onClick={()=>setEditor({ mode:'edit', entry:item })}>✎</button>
          )}
          <button className="btn-ghost type-remove" style={{padding:'3px 10px',fontSize:11,color:'var(--red)'}}
            aria-label={`${t('types.remove')} ${item.name||''}`}
            title={item.builtin ? t('types.builtinUndeletable') : t('types.remove')}
            disabled={item.builtin===true || pendingRemove===item.id || writeBlocked || editorBusy}
            onClick={()=>handleRemove(item)}>×</button>
        </div>))}
      </div>
      <div style={{display:'flex',gap:8,marginTop:16}}>
        {showIcon && onOpenMatching && <button className="btn-ghost" disabled={!!pendingRemove || editorBusy}
          onClick={onOpenMatching}>{t('classifying.matchingRules')}</button>}
        {showIcon ? (
          // 元件类型：新增与编辑共用编辑器，字段（名称/图标/颜色）完全一致
          <button className="btn-primary" data-testid="type-add-open"
            style={{padding:'6px 14px',fontSize:12}} disabled={!!pendingRemove || writeBlocked || editorBusy}
            onClick={()=>setEditor({ mode:'add' })}>{t('types.add')}</button>
        ) : (
          <>
            <input placeholder={t('types.newTypePlaceholder')} value={newName}
              onChange={e=>setNewName(e.target.value)} onKeyDown={e=>e.key==='Enter'&&handleAdd()}
              style={{flex:1,padding:'6px 10px',border:'1px solid var(--sep)',borderRadius:6,fontSize:12,background:'var(--bg)',color:'var(--text)'}}/>
            {showCategory&&<select value={newCat} onChange={e=>setNewCat(e.target.value)}
              style={{padding:'6px 8px',border:'1px solid var(--sep)',borderRadius:6,fontSize:12,background:'var(--bg)',color:'var(--text)'}}>
              <option>Other</option><option>SMD Chip</option><option>SMD IC</option><option>Through-hole</option><option>Connector</option></select>}
            <button className="btn-primary" style={{padding:'6px 14px',fontSize:12}} onClick={handleAdd}>{t('types.add')}</button>
          </>
        )}
      </div>
      <p style={{fontSize:11,color:'var(--text2)',marginTop:8}}>{t('types.saveHint')}</p>
      {editor && (
        <TypeEditorModal mode={editor.mode} entry={editor.entry}
          onClose={closeEditor} onSubmit={handleSubmit}
          onBusyChange={handleEditorBusy} canWrite={!writeBlocked} blockReason={blockReason} />
      )}
    </Frame>
  )
}

// ---- Import Progress Modal ----
const IMPORT_PHASES = new Set([
  'scanning', 'symbols_parse', 'symbols_import', 'footprints_parse', 'footprints', 'models',
  'linking', 'done', 'idle', 'failed', 'cancelled',
])
const isFileCount = (value) => Number.isSafeInteger(value) && value >= 0
function validImportStatus(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || typeof value.importing !== 'boolean') return false
  if (Object.hasOwn(value, 'progress')) {
    const progress = value.progress
    if (!progress || typeof progress !== 'object' || Array.isArray(progress)
      || !IMPORT_PHASES.has(progress.phase)
      || !isFileCount(progress.completed) || !isFileCount(progress.total)
      || (progress.total > 0 && progress.completed > progress.total)) return false
  }
  return true
}

export function ImportingModal({ onDone }) {
  const { t } = useTranslation()
  const [progress, setProgress] = useState(null)
  const [error, setError] = useState(null)
  const [hidden, setHidden] = useState(false)
  const latestOnDone = useRef(onDone)
  latestOnDone.current = onDone

  useEffect(() => {
    let active = true
    let timer = null
    const controller = new AbortController()
    async function poll() {
      let problem = null
      let completed = false
      try {
        const response = await fetch('/api/import-status', { signal: controller.signal, cache: 'no-store' })
        if (!response.ok) {
          problem = { key: 'importProgress.httpFailed', params: { status: response.status } }
        } else {
          const status = await response.json()
          if (!validImportStatus(status)) {
            problem = { key: 'importProgress.invalidResponse' }
          } else if (active) {
            // Only task progress belongs here. Legacy global database counts
            // are ignored; an omitted progress snapshot keeps the last value.
            if (status.progress) setProgress(status.progress)
            completed = !status.importing
          }
        }
      } catch {
        problem = { key: 'importProgress.readFailed' }
      }
      if (!active) return
      setError(problem)
      if (completed) {
        setHidden(true)
        latestOnDone.current?.()
        return
      }
      // Schedule after the response is handled: a slow request must never
      // overlap another request or let an older response overwrite a newer one.
      timer = setTimeout(poll, 2000)
    }
    void poll()
    return () => {
      active = false
      clearTimeout(timer)
      controller.abort()
    }
  }, [])

  if (hidden) return null

  return (
    <Modal title={t('app.importingTitle')} onClose={() => setHidden(true)}>
      <div className="import-progress" style={{textAlign:'center',padding:20}}>
        <p style={{fontSize:14,color:'var(--text2)',marginBottom:16}}>
          {t('app.importingHint')}
        </p>
        <div role="status" aria-live="polite" style={{marginBottom:20}}>
          <p style={{fontWeight:600,marginBottom:6}}>
            {progress ? t(`importProgress.phases.${progress.phase}`) : t('importProgress.waitingProgress')}
          </p>
          {progress && <>
            <p style={{fontSize:13,color:'var(--text2)',marginBottom:8}}>
              {t(progress.total > 0 ? 'importProgress.filesProgress' : 'importProgress.filesProcessed', {
                completed: progress.completed.toLocaleString(), total: progress.total.toLocaleString(),
              })}
            </p>
            {progress.total > 0
              ? <progress aria-label={t('importProgress.fileProgress')} max={progress.total} value={progress.completed} style={{width:'100%',maxWidth:360}} />
              : <progress aria-label={t('importProgress.fileProgress')} style={{width:'100%',maxWidth:360}} />}
          </>}
        </div>
        {error && <p role="alert" style={{fontSize:12,color:'var(--red)',marginTop:12}}>{t(error.key, error.params)}</p>}
        <p style={{fontSize:11,color:'var(--text3)',marginTop:12}}>
          {t('app.waitHint')}
        </p>
      </div>
    </Modal>
  )
}

// ---- Create Library Modal ----
export function CreateLibraryModal({ onClose, loadLibraries, toastMsg }) {
  const [name, setName] = useState('')
  const [symPath, setSymPath] = useState('')
  const [fpPath, setFpPath] = useState('')
  const { t } = useTranslation()

  async function browse(setter) {
    const r = await api.pickFolder()
    if (r?.path) setter(r.path)
  }

  async function create() {
    if (!name.trim()) { toastMsg(t('toast.required', { label: 'Name' })); return }
    const r = await api.createLibrary(name.trim())
    if (r?.ok) {
      toastMsg(t('toast.added', { name: name.trim() })); loadLibraries(); onClose()
    } else toastMsg(r?.error || t('toast.failedUnknown'))
  }

  return (
    <Modal title={t('sidebar.newLibrary')} onClose={onClose}>
      <div className="form-group">
        <label>{t('settings.libName')} *</label>
        <input value={name} onChange={e => setName(e.target.value)} autoFocus
          placeholder="e.g. My Library"
          onKeyDown={e => { if (e.key === 'Enter') create() }} />
      </div>
      <div className="form-group">
        <label>{t('settings.symbolPath')}</label>
        <div style={{display:'flex',gap:6}}>
          <input value={symPath} onChange={e => setSymPath(e.target.value)} style={{flex:1}}
            placeholder="D:\KiCad\Symbols" />
          <button className="btn-ghost" style={{padding:'6px 10px',whiteSpace:'nowrap'}}
            onClick={() => browse(setSymPath)}>Browse...</button>
        </div>
      </div>
      <div className="form-group">
        <label>{t('settings.footprintPath')}</label>
        <div style={{display:'flex',gap:6}}>
          <input value={fpPath} onChange={e => setFpPath(e.target.value)} style={{flex:1}}
            placeholder="D:\KiCad\Footprints" />
          <button className="btn-ghost" style={{padding:'6px 10px',whiteSpace:'nowrap'}}
            onClick={() => browse(setFpPath)}>Browse...</button>
        </div>
      </div>
      <div className="btn-row">
        <button className="btn-ghost" onClick={onClose}>{t('plugin.cancel')}</button>
        <button className="btn-primary" onClick={create}>{t('types.add')}</button>
      </div>
    </Modal>
  )
}

// ---- Classify Result Modal ----
export function ClassifyResultModal({ result, refresh, onRetryRefresh, onClose }) {
  const { t } = useTranslation()
  const summary = result?.summary || result
  const rows = result?.details || result?.results || []
  const writeStatuses = ['updated', 'rolled_back', 'not_attempted', 'unchanged', 'not_eligible',
    'read_only', 'source_changed', 'cas_skipped', 'deleted']
  const sourceLabel = source => source?.startsWith('classifying.sources.') ? t(source) : source
  return (
    <Modal title={t('inspector.classifyResult')} onClose={onClose}>
      <p style={{ fontSize: 12, color: 'var(--text2)', margin: '0 0 12px 0' }}>
        {result?.error ? t('classifying.partialMatched', { matched: summary?.matched ?? 0, total: summary?.total ?? 0 })
          : t('toast.classified', { matched: summary?.matched ?? 0, total: summary?.total ?? 0 })}
        {summary?.type_updated != null && ` · ${t('inspector.typeUpdated', { count: summary.type_updated })}`}
      </p>
      {result?.error && <p role="alert" style={{ fontSize: 12, color: 'var(--red)', whiteSpace: 'pre-wrap' }}>{result.error}</p>}
      {refresh && <div className="type-notice" role="status">
        <span>{t(refresh.status === 'published' ? 'classifying.refreshPublished'
          : refresh.status === 'pending' ? 'classifying.refreshPending' : 'classifying.refreshRequired')}</span>
        {refresh.details?.filter(item => item.status !== 'published').map(item =>
          <span key={item.source}>{t('classifying.refreshDetail', { source: sourceLabel(item.source),
            reason: item.status === 'superseded' ? t('classifying.refreshSuperseded')
              : translateMatchingMessage(item.errorMessage, t) || item.error || t('classifying.refreshUnpublished') })}</span>)}
        {['failed', 'superseded'].includes(refresh.status) && onRetryRefresh &&
          <button className="btn-ghost" onClick={onRetryRefresh}>{t('classifying.retryRefresh')}</button>}
      </div>}
      <div className="modal-results">
        {rows.length === 0 && <div className="search-popover-empty">—</div>}
        {rows.map((r, i) => (
          <div key={i} className="search-popover-item">
            <span className="search-popover-name">{r.name}</span>
            {r.library && <span className="search-popover-extra">{r.library}</span>}
            {r.type && <span className="search-popover-extra">{r.type}</span>}
            {r.write_status && <span className="search-popover-extra" data-write-status={r.write_status}>
              {writeStatuses.includes(r.write_status) ? t(`classifying.writeStatus.${r.write_status}`) : r.write_status}</span>}
            {(r.write_reason || r.reason) && <span className="search-popover-extra">{r.write_reason || r.reason}</span>}
            {r.confidence != null && (
              <span className="search-popover-extra">
                {t('inspector.confidence')}: {fmtConfidence(r.confidence)}
              </span>
            )}
          </div>
        ))}
      </div>
    </Modal>
  )
}

// ---- Match Suggestions Modal (table: symbol | footprint | score) ----
export function MatchSuggestModal({ matches, onClose }) {
  const { t } = useTranslation()
  const rows = matches || []
  return (
    <Modal title={t('inspector.matchSuggestions')} onClose={onClose}>
      <div className="match-head match-row">
        <span>{t('app.symbols')}</span>
        <span>{t('inspector.footprint')}</span>
        <span>{t('inspector.score')}</span>
      </div>
      <div className="modal-results">
        {rows.length === 0 && <div className="search-popover-empty">—</div>}
        {rows.map((m, i) => (
          <div key={i} className="search-popover-item match-row">
            <span className="match-col">{m.symbol}</span>
            <span className="match-col">{m.footprint}</span>
            <span className="match-col" style={{ textAlign: 'right' }}>
              {m.score != null ? `${m.score}%` : '—'}
            </span>
          </div>
        ))}
      </div>
    </Modal>
  )
}

// ---- Per-symbol match suggestions (lazy, click to bind; table: footprint | score) ----
export function SymbolMatchModal({ symbol, matches, onClose, onPick }) {
  const { t } = useTranslation()
  const rows = matches || []
  return (
    <Modal title={t('inspector.matchFor', { name: symbol.name })} onClose={onClose}>
      <div className="match-head match-row two-col">
        <span>{t('inspector.footprint')}</span>
        <span>{t('inspector.score')}</span>
      </div>
      <div className="modal-results">
        {rows.length === 0 && <div className="search-popover-empty">{t('inspector.noMatches')}</div>}
        {rows.map((m, i) => (
          <div key={i} className="search-popover-item match-row two-col" onClick={() => onPick(m)}>
            <span className="match-col">{m.footprint}</span>
            <span className="match-col" style={{ textAlign: 'right' }}>
              {m.score != null ? `${m.score}%` : '—'}
            </span>
          </div>
        ))}
      </div>
    </Modal>
  )
}

// 0-1 scale doubles → percent, otherwise assume already 0-100
function fmtConfidence(c) {
  const n = Number(c)
  if (!Number.isFinite(n)) return '—'
  return n > 1 ? `${Math.round(n)}%` : `${Math.round(n * 100)}%`
}

// ---- Two-step Delete Library Confirmation ----
// request.kind: 'symbol'    → symbol library (.kicad_sym); unregister by
//                             default, opt-in checkbox removes the file
//               'component' → component library (workspace registration);
//                             never touches any file
export function DeleteLibraryModal({ request, onClose, onConfirm }) {
  const { t } = useTranslation()
  const [deleteFile, setDeleteFile] = useState(false)
  const { kind, lib } = request
  const isSymbol = kind === 'symbol'
  return (
    <Modal title={t('confirm.deleteLibraryTitle', { name: lib.name })} onClose={onClose}>
      <p style={{ fontSize: 12, color: 'var(--text2)', margin: '0 0 12px 0', whiteSpace: 'pre-line' }}>
        {isSymbol ? t('confirm.deleteLibraryHint') : t('confirm.deleteComponentLibraryHint')}
      </p>
      {isSymbol && (
        <label style={{
          display: 'flex', alignItems: 'flex-start', gap: 8, cursor: 'pointer',
          fontSize: 12, padding: '8px 10px', border: '1px solid var(--sep)', borderRadius: 6,
          background: deleteFile ? 'rgba(220,38,38,0.08)' : 'transparent'
        }}>
          <input type="checkbox" checked={deleteFile}
            onChange={e => setDeleteFile(e.target.checked)} style={{ marginTop: 2 }} />
          <span>
            {t('confirm.deleteLibraryFile')}
            {lib.file_path && (
              <span style={{
                display: 'block', fontSize: 10, color: 'var(--text3)',
                wordBreak: 'break-all', marginTop: 2
              }}>
                {lib.file_path}
              </span>
            )}
          </span>
        </label>
      )}
      {isSymbol && deleteFile && (
        <p style={{ fontSize: 11, color: 'var(--red)', margin: '8px 0 0 0' }}>
          ⚠ {t('confirm.deleteLibraryFileWarning')}
        </p>
      )}
      <div className="btn-row" style={{
        marginTop: 16, display: 'flex', gap: 8, justifyContent: 'flex-end'
      }}>
        <button className="btn-ghost" onClick={onClose}>{t('plugin.cancel')}</button>
        <button className="btn-primary" style={{ background: 'var(--red)' }}
          onClick={() => onConfirm(isSymbol ? deleteFile : false)}>
          {t('confirm.deleteLibraryOk')}
        </button>
      </div>
    </Modal>
  )
}
