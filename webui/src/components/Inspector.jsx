import { useCallback, useEffect, useLayoutEffect, useRef, useState, useSyncExternalStore } from 'react'
import { createPortal } from 'react-dom'
import { useTranslation } from 'react-i18next'
import { PluginTabs } from './PluginSlot'
import { inspectorService } from '../services/inspector'
import { createInspectorModel } from '../models/inspector.js'
import SymbolSettings from './SymbolSettings'
import { SymbolMatchModal } from './Modals'
import { footprintView, modelView } from '../lib/footprintStatus'
import { typeColorOf, typeIconOf, typeLabelOf } from '../lib/typeMetadata'
import { useOverlayLayer } from '../workbench/overlay'

// Fixed-position popover rendered via portal — escapes the .inspector scroll
// container so it is never clipped. Prefers popping above the anchor row.
// kind='footprint' 时候选按服务端状态显示原因：失效/未检查的一律不可选
// （选中了宿主也会拒绝，不能先让用户以为能换）；error 是搜索请求的真实错误。
function SearchPopover({ open, anchorEl, query, results, kind, error, placeholder, onQuery, onSelect, onClose }) {
  const { t } = useTranslation()
  const popRef = useRef(null)
  const inputRef = useRef(null)
  const focusedRef = useRef(false)
  const [pos, setPos] = useState(null)
  // 搜索浮层进同一浮层栈：Escape 只关它；关闭后焦点回到打开它的字段。
  const popInStack = useOverlayLayer(onClose, open)
  useEffect(() => {
    if (!open || popInStack) return undefined
    const onKey = (event) => {
      if (event.key !== 'Escape') return
      event.stopPropagation()
      onClose()
    }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [open, popInStack, onClose])
  useEffect(() => {
    if (!open) return undefined
    return () => { if (anchorEl?.isConnected) anchorEl.focus?.() }
  }, [open, anchorEl])
  // 首次定位前浮层是 visibility:hidden，autoFocus 在那个阶段拿不到焦点；可见后再聚焦一次。
  useEffect(() => {
    if (!open) { focusedRef.current = false; return }
    if (!pos || focusedRef.current) return
    focusedRef.current = true
    inputRef.current?.focus()
  }, [open, pos])

  const place = useCallback(() => {
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
  }, [open, anchorEl])

  // Re-measure only when the popover's inputs change. Without a dependency
  // array this ran on EVERY render, and place() always setPos()s a fresh
  // object literal → render → effect → setPos → render → … infinite loop
  // (browser freeze / white screen).
  useLayoutEffect(() => { place() }, [place, query, results, error])
  useEffect(() => {
    if (!open || !anchorEl) return
    window.addEventListener('resize', place)
    return () => window.removeEventListener('resize', place)
  }, [open, anchorEl, place])

  if (!open) return null
  return createPortal(
    <div className="search-popover" ref={popRef}
      style={pos ? { top: pos.top, left: pos.left, width: pos.width, maxHeight: pos.maxHeight } : { visibility: 'hidden' }}>
      <div className="search-popover-header">
        <input ref={inputRef} autoFocus value={query} onChange={e => onQuery(e.target.value)}
          placeholder={placeholder} className="search-popover-input" />
        <button className="search-popover-close" onClick={onClose}>✕</button>
      </div>
      <div className="search-popover-list">
        {/* 搜索失败要显示真实错误，不能伪装成「没有候选」 */}
        {error && <div className="search-popover-error" data-testid="search-popover-error">{error}</div>}
        {!error && results.length === 0 && <div className="search-popover-empty">{query.length > 0 ? '—' : '...'}</div>}
        {results.map(r => {
          // 封装候选只在服务端说 available 时可选；缺少状态=未检查，也不是可用
          const state = kind === 'footprint' ? footprintView(r) : null
          const selectable = state === null || state.available
          return (
            <div key={r.id}
              className={`search-popover-item${selectable ? '' : ' disabled'}`}
              data-status={state ? state.status : undefined}
              data-selectable={state ? (selectable ? 'true' : 'false') : undefined}
              onClick={() => selectable && onSelect(r)}>
              <span className="search-popover-name">{r.name}</span>
              {r.pad_count != null && <span className="search-popover-extra">{r.pad_count}p</span>}
              {r.format != null && <span className="search-popover-extra">{r.format}</span>}
              {state && !state.available && (
                <span className={`status-note ${state.tone}`}>
                  {t(state.labelKey)}{state.reasonKey ? `（${t(state.reasonKey)}）` : ''}
                </span>
              )}
            </div>
          )
        })}
      </div>
    </div>,
    document.body)
}

export default function Inspector({ selected, selectionCount = 0, onClearSelection, classify, onDeleteSymbol, onRefresh, toastMsg, typeMap, workspaceId }) {
  const { t } = useTranslation()
  const callbacks = useRef({ onRefresh })
  callbacks.current = { onRefresh }
  const [model] = useState(() => createInspectorModel({
    service: inspectorService,
    refresh: () => callbacks.current.onRefresh?.(),
  }))
  const snapshot = useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)
  const [pickerAnchor, setPickerAnchor] = useState(null)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const { picker, query: searchQ } = snapshot
  const binding = snapshot.binding.data
  const bindingError = snapshot.binding.error
  const results = snapshot.search.data || []
  const searchError = snapshot.search.error
    ? snapshot.search.error.message || t('toast.failedUnknown') : ''
  const symMatches = snapshot.matchesOpen && snapshot.matches.status !== 'loading'
    ? snapshot.matches.data || [] : null

  useLayoutEffect(() => {
    model.select(selected)
    setPickerAnchor(null)
    // Clear resources, rather than terminally dispose the model: React's
    // StrictMode repeats setup after cleanup on this same component instance.
    return () => { model.select(null) }
  }, [model, selected])

  function notify(message) {
    if (toastMsg) toastMsg(message)
    else alert(message)
  }
  function showOutcome(outcome) {
    if (outcome.status === 'missing-footprint') notify(t('inspector.noFootprintFirst'))
    else if (outcome.status === 'failed' || outcome.status === 'refresh-failed') {
      notify(t('toast.failed', { error: outcome.error?.message || t('toast.failedUnknown') }))
    }
    return outcome
  }

  // 多选（ED-023）：集合态只给只读摘要与清除入口；单对象写操作必须指向
  // 明确的活动对象，不能把“最近点击的对象”当成批量目标。
  if (selectionCount > 1 || (selectionCount > 0 && !selected)) return (
    <div className="inspector">
      <div className="inspector-multi" data-testid="inspector-multi">
        <h4>{t('inspector.multiTitle', { count: selectionCount })}</h4>
        <p>{t('inspector.multiHint')}</p>
        {onClearSelection ? (
          <button type="button" className="btn" onClick={onClearSelection}>{t('app.clearSelection')}</button>
        ) : null}
        {/* 多选时插件仍应拿到兼容的活动对象，而不是整块区域消失（plan §3） */}
        {selected ? (
          <PluginTabs slot="inspector.tab" ownerProps={{ workspaceId: workspaceId || null, symbolId: selected.id, binding: null, typeMap }} defaultLabel="详情">
            <div className="inspector-section">
              <div className="inspector-row"><span className="inspector-label">{t('inspector.multiActive')}</span><span>{selected.name}</span></div>
            </div>
          </PluginTabs>
        ) : null}
      </div>
    </div>
  )

  if (!selected) return (
    <div className="inspector">
      <div className="inspector-empty">
        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><circle cx="12" cy="12" r="10"/><path d="M8 14s1.5 2 4 2 4-2 4-2"/><line x1="9" y1="9" x2="9.01" y2="9"/><line x1="15" y1="9" x2="15.01" y2="9"/></svg>
        <p>{t('app.selectComponent')}<br/>{t('app.selectComponentHint')}</p>
      </div>
    </div>
  )

  function closePicker() {
    model.closePicker()
    setPickerAnchor(null)
  }
  function openPicker(kind, event) {
    setPickerAnchor(event?.currentTarget || null)
    model.openPicker(kind)
  }
  function onQuery(query) { model.search(query) }
  async function onSelect(item) { showOutcome(await model.selectCandidate(item)) }
  async function onSettingsChanged() { showOutcome(await model.refresh()) }
  async function findMatches() { showOutcome(await model.findMatches()) }
  async function pickMatch(item) { showOutcome(await model.pickMatch(item)) }

  // 身份是 type_id（旧响应才回退到 type），展示名/图标/颜色取自同一份类型配置：
  // 改名不改 ID，重排与改图标都不影响已落库的数据；认不出的历史 ID 原样显示。
  const typeId = selected.type_id || selected.type
  const typeIcon = typeIconOf(typeMap, typeId)
  const typeColor = typeColorOf(typeMap, typeId)
  const typeLabel = typeLabelOf(typeMap, typeId, selected.type) || 'Unknown'

  const locked = !!selected.locked
  // 存在性只认服务端状态：加载中/请求失败都不显示正常，也不靠 selected.footprint 猜
  const bindingFailed = !!bindingError
  const bindingErrorText = bindingError?.message || ''
  const bindingLoading = snapshot.binding.status === 'loading' || (binding === null && !bindingFailed)
  const fpState = footprintView(binding || selected, { loading: bindingLoading, failed: bindingFailed })
  const modelState = modelView(binding || selected, { loading: bindingLoading, failed: bindingFailed })
  const statusSuffix = (state) => {
    if (state.loading) return t('footprintStatus.loading')
    // 失败时带上真实原因（服务端的 DB-DOWN、网络错误……），不能只剩一句通用文案
    if (state.failed) {
      return bindingErrorText
        ? t('footprintStatus.errorWithReason', { error: bindingErrorText })
        : t('footprintStatus.error')
    }
    const reason = state.reasonKey ? `（${t(state.reasonKey)}）` : ''
    return `${t(state.labelKey)}${reason}`
  }
  const footprintText = fpState.available
    ? (binding?.footprint_name || fpState.reference || t('inspector.notAssigned'))
    : `${fpState.reference || '—'} · ${statusSuffix(fpState)}`
  const modelText = modelState.usable
    ? (binding?.model_name || t('inspector.notLinked'))
    : `${binding?.model_name || t('inspector.notLinked')} · ${statusSuffix(modelState)}`
  return (
    <div className="inspector">
      <div className="inspector-header">
        <div className="inspector-icon" aria-hidden="true"
          style={typeColor ? { color: typeColor } : undefined}>{typeIcon}</div>
        <div><h4>{selected.name}</h4><div className="inspector-type" data-testid="inspector-type">{typeLabel}</div></div>
        <button className="inspector-gear" onClick={() => setSettingsOpen(true)} disabled={locked}
          title={t('inspector.settings')}>⚙</button>
      </div>
      {locked && (
        <div className="inspector-locked">🔒 {t('inspector.lockedHint')}</div>
      )}
      <PluginTabs slot="inspector.tab" ownerProps={{ workspaceId: workspaceId || null, symbolId: selected.id, binding, typeMap }} defaultLabel="详情">
      <div className="inspector-section">
        {[
          ['footprint', footprintText,
           locked ? undefined : (ev) => openPicker('footprint', ev),
           fpState.available ? 'ok' : (fpState.loading || fpState.failed ? 'unknown' : 'bad'),
           `fp-state-${fpState.status}`],
          ['3dModel', modelText,
           locked ? undefined : (ev) => openPicker('model', ev),
           modelState.usable ? 'ok' : (modelState.status === 'loading' || modelState.status === 'error' ? 'unknown' : 'bad'),
           `model-state-${modelState.status}`],
          ['pinCount', selected.pins || '—'],
          ['mpn', selected.mpn || '—'],
          ['value', selected.value || '—'],
        ].map(([key, val, onClick, cls, testid]) => (
          <div className="field" key={key}>
            <label>{t(`inspector.${key}`)}</label>
            <div className={`val ${cls || ''} ${onClick ? 'wrap' : ''}`}
              data-testid={testid}
              tabIndex={onClick ? 0 : undefined}
              role={onClick ? 'button' : undefined}
              onKeyDown={onClick ? (event) => {
                if (event.key !== 'Enter' && event.key !== ' ') return
                event.preventDefault()
                onClick(event)
              } : undefined}
              data-status={key === 'footprint' ? fpState.status : (key === '3dModel' ? modelState.status : undefined)}
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
        kind={picker}
        error={searchError}
        placeholder={picker === 'footprint' ? t('inspector.searchFootprint') : t('inspector.searchModel')}
        onQuery={onQuery}
        onSelect={onSelect}
        onClose={closePicker}
      />

      <div className="inspector-section">
        <h5>{t('inspector.quickActions')}</h5>
        <div className="actions">
          <button className="primary" onClick={classify}>{t('toolbar.autoClassify')}</button>
          {!bindingLoading && !fpState.reference && (
            <button onClick={findMatches}>{t('inspector.findFootprint')}</button>
          )}
          {/* 未找到/已失效/多个同名来源/检查失败：给一个明确的更换入口 */}
          {!locked && fpState.canReplace && (
            <button data-testid="inspector-replace-footprint"
              onClick={(ev) => openPicker('footprint', ev)}>{t('inspector.replaceFootprint')}</button>
          )}
          <button style={{ color:'var(--red)', borderColor:'var(--red)', opacity: locked ? 0.4 : 1 }}
            onClick={() => onDeleteSymbol(selected)} disabled={locked}>{t('inspector.deleteSymbol')}</button>
        </div>
      </div>
      </PluginTabs>

      {/* 加载中/请求失败（含真实原因）必须一起传进去：
          设置弹窗不能拿列表里的旧 available 快照当结论，也不能丢掉失败原因 */}
      <SymbolSettings open={settingsOpen} symbol={selected} binding={binding}
        bindingLoading={bindingLoading} bindingError={bindingError}
        onClose={() => setSettingsOpen(false)} onChanged={onSettingsChanged} toastMsg={toastMsg} />

      {symMatches !== null && (
        <SymbolMatchModal symbol={selected} matches={symMatches}
          onClose={model.closeMatches} onPick={pickMatch} />
      )}
    </div>
  )
}
