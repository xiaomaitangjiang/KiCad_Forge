import { useEffect, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import { Modal } from './Modals'

// Per-symbol settings dialog: assign the footprint bound to the symbol
// and the 3D model bound to that footprint (KiCad symbol-properties style).
// Changes are staged (pendingFp / pendingModel) and applied via an explicit
// confirm button that only appears when something actually changed.
export default function SymbolSettings({ open, symbol, binding, onClose, onChanged }) {
  const { t } = useTranslation()
  const [fpQ, setFpQ] = useState('')
  const [fpResults, setFpResults] = useState([])
  const [modelQ, setModelQ] = useState('')
  const [modelResults, setModelResults] = useState([])
  const [pendingFp, setPendingFp] = useState(null)    // staged footprint (not yet applied)
  const [pendingModel, setPendingModel] = useState(null) // staged 3D model

  useEffect(() => {
    if (open) {
      setFpQ(''); setModelQ(''); setPendingFp(null); setPendingModel(null)
      // Load default lists (backend returns the first N entries for an empty query)
      searchFp('')
      searchModel('')
    }
  }, [open, symbol?.id])

  if (!open) return null

  async function searchFp(q) {
    setFpQ(q)
    try {
      setFpResults(await api.searchFootprints(q, binding?.footprint_name || '') || [])
    } catch { setFpResults([]) }
  }

  async function searchModel(q) {
    setModelQ(q)
    try {
      setModelResults(await api.searchModels(q, binding?.model_name || '') || [])
    } catch { setModelResults([]) }
  }

  // Stage a selection — no API call until the confirm button is pressed.
  // The confirm button only shows when the staged value differs from the
  // currently bound one (i.e. when something would actually change).
  function selectFp(fp) {
    if (binding?.footprint_id === fp.id) { setPendingFp(null); return }
    setPendingFp(fp)
  }

  function selectModel(m) {
    if (!binding?.footprint_id) {
      alert(t('inspector.noFootprintFirst'))
      return
    }
    if (binding?.model_id === m.id) { setPendingModel(null); return }
    setPendingModel(m)
  }

  async function applyChanges() {
    try {
      if (pendingFp) {
        await api.bindFootprint(symbol.id, pendingFp.id)
        setPendingFp(null)
      }
      if (pendingModel) {
        await api.bindModel(binding.footprint_id, pendingModel.id)
        setPendingModel(null)
      }
      setFpQ(''); setFpResults([]); setModelQ(''); setModelResults([])
      if (onChanged) onChanged()
    } catch {}
  }

  const locked = !!symbol?.locked
  return (
    <Modal title={`${symbol.name} — ${t('inspector.settings')}`} onClose={onClose}>
        {locked && (
          <div className="inspector-locked" style={{ margin: '0 0 12px' }}>🔒 {t('inspector.lockedHint')}</div>
        )}
        <div className="modal-section">
          <label>{t('inspector.bindFootprint')}</label>
          <div className="modal-current">{binding?.footprint_name || t('inspector.notAssigned')}</div>
          <input className="modal-search" value={fpQ} onChange={e => searchFp(e.target.value)}
            placeholder={t('inspector.searchFootprint')} disabled={locked} />
          <div className="modal-results">
            {fpResults.map(fp => (
              <div key={fp.id} className="search-popover-item" onClick={() => !locked && selectFp(fp)}>
                <span className="search-popover-name">{fp.name}</span>
                {fp.pad_count != null && <span className="search-popover-extra">{fp.pad_count}p</span>}
              </div>
            ))}
          </div>
        </div>

        <div className="modal-section">
          <label>{t('inspector.bindModel')}</label>
          <div className="modal-current">{binding?.model_name || t('inspector.notAssigned')}</div>
          <input className="modal-search" value={modelQ} onChange={e => searchModel(e.target.value)}
            placeholder={t('inspector.searchModel')} disabled={locked} />
          <div className="modal-results">
            {modelResults.map(m => (
              <div key={m.id} className="search-popover-item" onClick={() => !locked && selectModel(m)}>
                <span className="search-popover-name">{m.name}</span>
                {m.format != null && <span className="search-popover-extra">{m.format}</span>}
              </div>
            ))}
          </div>
        </div>

        <div className="modal-actions">
          {pendingFp || pendingModel ? (
            <>
              <button onClick={() => { setPendingFp(null); setPendingModel(null) }}>{t('inspector.cancelApply')}</button>
              <button className="btn-primary" onClick={applyChanges}>{t('inspector.confirmApply')}</button>
            </>
          ) : (
            <button onClick={onClose}>{t('inspector.close')}</button>
          )}
        </div>
    </Modal>
  )
}
