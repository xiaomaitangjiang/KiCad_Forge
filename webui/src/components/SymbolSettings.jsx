import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import { Modal } from './Modals'
import { footprintView, modelView } from '../lib/footprintStatus'

// Per-symbol settings dialog: assign the footprint bound to the symbol
// and the 3D model bound to that footprint (KiCad symbol-properties style).
// Changes are staged (pendingFp / pendingModel) and applied via an explicit
// confirm button that only appears when something actually changed.
// bindingLoading / bindingError 来自父组件的绑定请求：请求还没回来或失败时，
// 不能用列表里那份可能过期的 available 快照显示「封装/模型正常」；
// bindingError 还带着真实失败原因（如 DB-DOWN），不能只剩一句通用文案。
export default function SymbolSettings({ open, symbol, binding, bindingLoading = false,
  bindingError = false, onClose, onChanged, toastMsg }) {
  const { t } = useTranslation()
  // 兼容两种形态：字符串（旧调用）或 { message }（Inspector 传入的真实原因）
  const bindingFailed = !!bindingError
  const bindingErrorText = !bindingError ? ''
    : (typeof bindingError === 'string' ? bindingError : (bindingError.message || ''))
  const statusText = (state) => {
    if (state.failed && bindingErrorText) {
      return t('footprintStatus.errorWithReason', { error: bindingErrorText })
    }
    return `${t(state.labelKey)}${state.reasonKey ? `（${t(state.reasonKey)}）` : ''}`
  }
  const [fpQ, setFpQ] = useState('')
  const [fpResults, setFpResults] = useState([])
  const [modelQ, setModelQ] = useState('')
  const [modelResults, setModelResults] = useState([])
  const [pendingFp, setPendingFp] = useState(null)    // staged footprint (not yet applied)
  const [pendingModel, setPendingModel] = useState(null) // staged 3D model
  const latestBinding = useRef(binding)
  latestBinding.current = binding
  const fpRequest = useRef(0)
  const modelRequest = useRef(0)

  // 查询回调保持稳定；near 只在发请求时取最新绑定。
  // 详情请求回来时不能触发会话重置，清掉用户已经选好的待提交项。
  const searchFp = useCallback(async (q) => {
    const request = ++fpRequest.current
    setFpQ(q)
    try {
      const results = await api.searchFootprints(q, latestBinding.current?.footprint_name || '')
      if (request === fpRequest.current) setFpResults(results || [])
    } catch {
      if (request === fpRequest.current) setFpResults([])
    }
  }, [])

  const searchModel = useCallback(async (q) => {
    const request = ++modelRequest.current
    setModelQ(q)
    try {
      const results = await api.searchModels(q, latestBinding.current?.model_name || '')
      if (request === modelRequest.current) setModelResults(results || [])
    } catch {
      if (request === modelRequest.current) setModelResults([])
    }
  }, [])

  useEffect(() => {
    if (!open) return
    setFpQ(''); setModelQ(''); setFpResults([]); setModelResults([])
    setPendingFp(null); setPendingModel(null)
    // 每次打开或换符号只加载一次默认列表；关闭、换符号、卸载都作废在途响应。
    searchFp('')
    searchModel('')
    return () => {
      fpRequest.current += 1
      modelRequest.current += 1
    }
  }, [open, symbol?.id, searchFp, searchModel])

  if (!open) return null

  // Stage a selection — no API call until the confirm button is pressed.
  // The confirm button only shows when the staged value differs from the
  // currently bound one (i.e. when something would actually change).
  function selectFp(fp) {
    // 失效候选不能选：选中的 UUID 到了宿主那里也会被拒（文件不存在）
    if (!footprintView(fp).available) return
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
    // 失败时保留待提交的选择（不 setPendingX(null)），用户可以重试或改选
    try {
      // The footprint id used for the model step must be the NEWLY bound one —
      // the `binding` prop is stale until onChanged() refreshes it.
      let fpId = binding?.footprint_id
      if (pendingFp) {
        const r = await api.bindFootprint(symbol.id, pendingFp.id)
        if (!r?.ok) { toastMsg(r?.error || t('toast.failedUnknown')); return }
        const fresh = await api.getBinding(symbol.id).catch(() => null)
        fpId = fresh?.footprint_id || pendingFp.id
        setPendingFp(null)
      }
      if (pendingModel) {
        if (!fpId) { alert(t('inspector.noFootprintFirst')); return }
        const r = await api.bindModel(fpId, pendingModel.id)
        if (!r?.ok) { toastMsg(r?.error || t('toast.failedUnknown')); return }
        setPendingModel(null)
      }
      setFpQ(''); setFpResults([]); setModelQ(''); setModelResults([])
      if (onChanged) onChanged()
    } catch (cause) {
      // 请求层异常（断网/服务端 5xx）不能静默：告诉用户，保留已选内容
      toastMsg(t('toast.failed', { error: cause?.message || t('toast.failedUnknown') }))
    }
  }

  const locked = !!symbol?.locked
  return (
    <Modal title={`${symbol.name} — ${t('inspector.settings')}`} onClose={onClose}>
        {locked && (
          <div className="inspector-locked" style={{ margin: '0 0 12px' }}>🔒 {t('inspector.lockedHint')}</div>
        )}
        <div className="modal-section">
          <label>{t('inspector.bindFootprint')}</label>
          {(() => {
            const state = footprintView(binding || symbol,
              { loading: bindingLoading, failed: bindingFailed })
            return (
              <div className="modal-current" data-testid="settings-current-footprint"
                data-status={state.status}>
                {binding?.footprint_name || state.reference || t('inspector.notAssigned')}
                {state.available ? null : (
                  <span className={`status-note ${state.tone}`}> · {statusText(state)}</span>
                )}
              </div>
            )
          })()}
          <input className="modal-search" value={fpQ} onChange={e => searchFp(e.target.value)}
            placeholder={t('inspector.searchFootprint')} disabled={locked} />
          <div className="modal-results">
            {fpResults.map(fp => {
              // 候选带服务端状态：失效/未检查的一律禁用（不显示成可用）
              const state = footprintView(fp)
              const selectable = state.available && !locked
              return (
                <div key={fp.id}
                  className={`search-popover-item${selectable ? '' : ' disabled'}`}
                  data-testid={`settings-fp-${fp.id}`}
                  data-status={state.status}
                  data-selectable={selectable ? 'true' : 'false'}
                  onClick={() => selectable && selectFp(fp)}>
                  <span className="search-popover-name">{fp.name}</span>
                  {fp.pad_count != null && <span className="search-popover-extra">{fp.pad_count}p</span>}
                  {!state.available && (
                    <span className={`status-note ${state.tone}`}>
                      {t(state.labelKey)}{state.reasonKey ? `（${t(state.reasonKey)}）` : ''}
                    </span>
                  )}
                </div>
              )
            })}
          </div>
        </div>

        <div className="modal-section">
          <label>{t('inspector.bindModel')}</label>
          {(() => {
            const state = modelView(binding || symbol,
              { loading: bindingLoading, failed: bindingFailed })
            return (
              <div className="modal-current" data-testid="settings-current-model"
                data-status={state.status}>
                {binding?.model_name || t('inspector.notAssigned')}
                {state.usable ? null : (
                  <span className={`status-note ${state.tone}`}> · {statusText(state)}</span>
                )}
              </div>
            )
          })()}
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
