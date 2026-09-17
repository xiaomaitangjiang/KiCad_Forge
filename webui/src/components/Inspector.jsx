import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import SymbolSettings from './SymbolSettings'
import { SymbolMatchModal } from './Modals'

const TYPE_ICONS = {
  Resistor:'⊟',Capacitor:'⊞',Inductor:'◠',Diode:'▷',LED:'☀',
  Microcontroller:'⬡',Connector:'▣',Crystal:'◇',VoltageRegulator:'▦',
  Transistor:'△',MOSFET:'△',FPGA:'◰',Memory:'▤',Oscillator:'◆',
  Fuse:'◎',Switch:'⏣',Relay:'⏻',Jumper:'⌸',TestPoint:'◎',
}

// Fixed-position popover rendered via portal — escapes the .inspector scroll
// container so it is never clipped. Prefers popping above the anchor row.
function SearchPopover({ open, anchorEl, query, results, placeholder, onQuery, onSelect, onClose }) {
  const popRef = useRef(null)
  const [pos, setPos] = useState(null)

  const place = () => {
    if (!open || !anchorEl) return
    const a = anchorEl.getBoundingClientRect()
    const width = Math.min(360, window.innerWidth - 24)
    // Measure real height (capped) from the DOM if mounted, else estimate
    const h = popRef.current ? popRef.current.offsetHeight : 200
    const above = a.top - 8
    let top, maxH
    if (above >= Math.min(240, h)) {           // enough room above
      top = a.top - h - 8; maxH = a.top - 16
    } else {                                    // pop below
      top = a.bottom + 8; maxH = window.innerHeight - a.bottom - 24
    }
    let left = Math.min(a.left, window.innerWidth - width - 16)
    left = Math.max(8, left)
    setPos({ top, left, width, maxHeight: Math.max(160, Math.min(320, maxH)) })
  }

  // Re-measure only when the popover's inputs change. Without a dependency
  // array this ran on EVERY render, and place() always setPos()s a fresh
  // object literal → render → effect → setPos → render → … infinite loop
  // (browser freeze / white screen).
  useLayoutEffect(() => { place() }, [open, anchorEl, query, results])
  useEffect(() => {
    if (!open || !anchorEl) return
    const onR = () => place()
    window.addEventListener('resize', onR)
    return () => window.removeEventListener('resize', onR)
  }, [open, anchorEl])

  if (!open) return null
  return createPortal(
    <div className="search-popover" ref={popRef}
      style={pos ? { top: pos.top, left: pos.left, width: pos.width, maxHeight: pos.maxHeight } : { visibility: 'hidden' }}>
      <div className="search-popover-header">
        <input autoFocus value={query} onChange={e => onQuery(e.target.value)}
          placeholder={placeholder} className="search-popover-input" />
        <button className="search-popover-close" onClick={onClose}>✕</button>
      </div>
      <div className="search-popover-list">
        {results.length === 0 && <div className="search-popover-empty">{query.length > 0 ? '—' : '...'}</div>}
        {results.map(r => (
          <div key={r.id} className="search-popover-item" onClick={() => onSelect(r)}>
            <span className="search-popover-name">{r.name}</span>
            {r.pad_count != null && <span className="search-popover-extra">{r.pad_count}p</span>}
            {r.format != null && <span className="search-popover-extra">{r.format}</span>}
          </div>
        ))}
      </div>
    </div>,
    document.body)
}

export default function Inspector({ selected, classify, onDeleteSymbol, onRefresh }) {
  const { t } = useTranslation()
  const [picker, setPicker] = useState(null) // 'footprint' | 'model' | null
  const [pickerAnchor, setPickerAnchor] = useState(null) // triggering row element
  const [searchQ, setSearchQ] = useState('')
  const [results, setResults] = useState([])
  const [binding, setBinding] = useState(null) // null = loading
  const [settingsOpen, setSettingsOpen] = useState(false)
  const [symMatches, setSymMatches] = useState(null) // null = closed

  async function loadBinding() {
    try { const b = await api.getBinding(selected.id); setBinding(b) } catch {}
  }

  // Preload binding whenever the selected symbol changes
  useEffect(() => {
    setBinding(null)
    if (selected) loadBinding()
  }, [selected?.id])

  if (!selected) return (
    <div className="inspector">
      <div className="inspector-empty">
        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><circle cx="12" cy="12" r="10"/><path d="M8 14s1.5 2 4 2 4-2 4-2"/><line x1="9" y1="9" x2="9.01" y2="9"/><line x1="15" y1="9" x2="15.01" y2="9"/></svg>
        <p>{t('app.selectComponent')}<br/>{t('app.selectComponentHint')}</p>
      </div>
    </div>
  )

  function openPicker(type, ev) {
    setPicker(type)
    setPickerAnchor(ev?.currentTarget || null)
    setSearchQ('')
    setResults([])
    if (type === 'footprint') handleSearch('')
    else handleModelSearch('')
  }

  async function handleSearch(q) {
    setSearchQ(q)
    const near = binding?.footprint_name || selected.footprint || ''
    try { const r = await api.searchFootprints(q, near); setResults(r || []) } catch { setResults([]) }
  }

  async function handleModelSearch(q) {
    setSearchQ(q)
    const near = binding?.model_name || ''
    try { const r = await api.searchModels(q, near); setResults(r || []) } catch { setResults([]) }
  }

  function onQuery(q) {
    if (picker === 'footprint') handleSearch(q)
    else handleModelSearch(q)
  }

  async function onSelect(item) {
    try {
      if (picker === 'footprint') {
        await api.bindFootprint(selected.id, item.id)
      } else {
        // Bind the model to the footprint linked to this symbol
        const b = binding || await api.getBinding(selected.id)
        if (!b.footprint_id) {
          alert(t('inspector.noFootprintFirst'))
          return
        }
        await api.bindModel(b.footprint_id, item.id)
      }
      setPicker(null)
      await loadBinding()
      if (onRefresh) onRefresh()
    } catch {}
  }

  async function onSettingsChanged() {
    await loadBinding()
    if (onRefresh) onRefresh()
  }

  // Lazy per-symbol match suggestions — computed only when clicked
  async function findMatches() {
    try { const m = await api.symbolMatches(selected.id); setSymMatches(m || []) }
    catch { setSymMatches([]) }
  }

  async function pickMatch(fp) {
    try {
      await api.bindFootprint(selected.id, fp.footprint_id)
      setSymMatches(null)
      await loadBinding()
      if (onRefresh) onRefresh()
    } catch {}
  }

  const locked = !!selected.locked
  return (
    <div className="inspector">
      <div className="inspector-header">
        <div className="inspector-icon">{TYPE_ICONS[selected.type] || '○'}</div>
        <div><h4>{selected.name}</h4><div className="inspector-type">{selected.type || 'Unknown'}</div></div>
        <button className="inspector-gear" onClick={() => setSettingsOpen(true)} disabled={locked}
          title={t('inspector.settings')}>⚙</button>
      </div>
      {locked && (
        <div className="inspector-locked">🔒 {t('inspector.lockedHint')}</div>
      )}
      <div className="inspector-section">
        {[
          ['footprint', binding?.footprint_name || selected.footprint || t('inspector.notAssigned'),
           locked ? undefined : (ev) => openPicker('footprint', ev),
           (binding?.footprint_name || selected.footprint) ? 'ok' : 'warn'],
          ['3dModel', binding === null ? '…' : (binding.model_name || `⚠ ${t('inspector.notLinked')}`),
           locked ? undefined : (ev) => openPicker('model', ev), binding?.model_name ? 'ok' : 'warn'],
          ['pinCount', selected.pins || '—'],
          ['mpn', selected.mpn || '—'],
          ['value', selected.value || '—'],
        ].map(([key, val, onClick, cls]) => (
          <div className="field" key={key}>
            <label>{t(`inspector.${key}`)}</label>
            <div className={`val ${cls || ''} ${onClick ? 'wrap' : ''}`}
              style={{ fontSize: (key === 'mpn' || key === 'value' || key === 'footprint' || key === '3dModel') ? 12 : undefined,
                      cursor: onClick ? 'pointer' : undefined,
                      opacity: locked && onClick ? 0.5 : undefined }}
              onClick={onClick} title={locked ? t('inspector.lockedHint') : (onClick ? t('inspector.clickToChange') : undefined)}>
              {val}
            </div>
          </div>
        ))}
      </div>

      <SearchPopover
        open={!!picker}
        anchorEl={pickerAnchor}
        query={searchQ}
        results={results}
        placeholder={picker === 'footprint' ? t('inspector.searchFootprint') : t('inspector.searchModel')}
        onQuery={onQuery}
        onSelect={onSelect}
        onClose={() => setPicker(null)}
      />

      <div className="inspector-section">
        <h5>{t('inspector.quickActions')}</h5>
        <div className="actions">
          <button className="primary" onClick={classify}>{t('toolbar.autoClassify')}</button>
          {binding && !binding.footprint_name && !selected.footprint && (
            <button onClick={findMatches}>{t('inspector.findFootprint')}</button>
          )}
          <button style={{ color:'var(--red)', borderColor:'var(--red)', opacity: locked ? 0.4 : 1 }}
            onClick={() => onDeleteSymbol(selected)} disabled={locked}>{t('inspector.deleteSymbol')}</button>
        </div>
      </div>

      <SymbolSettings open={settingsOpen} symbol={selected} binding={binding}
        onClose={() => setSettingsOpen(false)} onChanged={onSettingsChanged} />

      {symMatches !== null && (
        <SymbolMatchModal symbol={selected} matches={symMatches}
          onClose={() => setSymMatches(null)} onPick={pickMatch} />
      )}
    </div>
  )
}
