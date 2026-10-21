import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { footprintView, modelIsUsable } from '../lib/footprintStatus'
import { typeLabelOf } from '../lib/typeMetadata'
import { editingRuntime } from '../editing/runtime'
import { reasonLabel } from '../editing/reasons'
import { isSelected } from '../editing/selection-model'
import { shortcutLabel } from '../editing/command-service'
import { PluginEntries, PluginSlot, usePluginEntries } from './PluginSlot'
import { useOverlayLayer } from '../workbench/overlay'

const EDIT_ITEMS = [
  { id: 'copy', label: 'copy' },
  { id: 'cut', label: 'cut' },
  { id: 'paste', label: 'paste' },
  { id: 'selectAll', label: 'selectAll' },
  { id: 'undo', label: 'undo', history: 'undoLabel' },
  { id: 'redo', label: 'redo', history: 'redoLabel' },
]

// Vertical card list — one card per component (name / footprint / type).
// 圆点与问题筛选只看服务端状态：旧的 has_footprint/has_3d_model 不能把
// 失效封装显示成正常。
// 多选（ED-020..024）：集合按 UUID 维护；单击替换、Ctrl/Cmd 切换、Shift 按
// 当前排序连续选择；右键菜单与顶栏共用同一命令服务与可用状态。
export default function SymbolTable({
  symbols, selection, onSelect, onClearSelection, filter, issueFilter,
  typeMap, workspaceId, libraryNames, pendingCutIds, loading, error, onRetry, onCommandResult,
}) {
  const { t } = useTranslation()
  const [menu, setMenu] = useState(null)
  const [, setBarTick] = useState(0)
  const menuRef = useRef(null)
  const menuEntries = usePluginEntries('contextmenu.item')

  // 结果顺序由 App 提供（当前搜索/筛选/工作空间/排序），这里不再二次过滤，
  // 否则 Shift 连续选择与全选会和状态栏计数不一致。
  const filtered = symbols

  useOverlayLayer(() => setMenu(null), Boolean(menu))

  useEffect(() => {
    if (!menu) return undefined
    const closeOnOutside = (event) => {
      if (menuRef.current && event.target instanceof Node && menuRef.current.contains(event.target)) return
      setMenu(null)
    }
    const closeOnScroll = () => setMenu(null)
    document.addEventListener('pointerdown', closeOnOutside, true)
    window.addEventListener('scroll', closeOnScroll, true)
    return () => {
      document.removeEventListener('pointerdown', closeOnOutside, true)
      window.removeEventListener('scroll', closeOnScroll, true)
    }
  }, [menu])

  function selectFromEvent(event, symbol) {
    const mode = event.shiftKey ? 'range' : ((event.ctrlKey || event.metaKey) ? 'toggle' : 'single')
    onSelect?.(symbol, { mode })
  }

  function openContextMenu(event, symbol) {
    event.preventDefault()
    // 右键已选中的卡片保留现有多选；右键未选中的卡片先单选出它，避免命令
    // 作用在看不见的目标上。菜单打开前冻结快照（ED-002）。
    if (!isSelected(selection, symbol.id)) onSelect?.(symbol, { mode: 'single' })
    const frozen = editingRuntime.freeze('objects')
    const width = 224
    const height = 278
    const x = Math.max(8, Math.min(event.clientX, (window.innerWidth || 1024) - width - 8))
    const y = Math.max(8, Math.min(event.clientY, (window.innerHeight || 768) - height - 8))
    setMenu({ x, y, frozen, symbol })
  }

  function runMenuCommand(commandId) {
    const frozen = menu?.frozen
    setMenu(null)
    if (!frozen) return
    editingRuntime.execute(commandId, frozen).then((result) => onCommandResult?.(result))
  }

  function itemState(commandId) {
    return menu?.frozen ? editingRuntime.getState(commandId, menu.frozen) : { disabled: true }
  }

  function itemLabel(item) {
    if (item.history) {
      const action = menu?.frozen?.objects?.history?.[item.history]
      if (action) return t(item.id === 'undo' ? 'topbar.undoWith' : 'topbar.redoWith', { action })
    }
    return t(`topbar.${item.label}`)
  }

  function itemTitle(item) {
    const state = itemState(item.id)
    return state.disabled ? (reasonLabel(state.reason, t) || '') : ''
  }

  // 选择条和右键菜单共用同一套 command service 状态：按钮不可用时必须有原因，
  // 而不是只把按钮灰掉。barTick 只用来在异步命令返回后重新取一次状态。
  const barFrozen = selection?.ids?.length ? editingRuntime.freeze('objects') : null
  const barStates = barFrozen ? editingRuntime.getStates(barFrozen) : null

  function runBarCommand(commandId) {
    if (!barFrozen) return
    editingRuntime.execute(commandId, barFrozen).then((result) => {
      onCommandResult?.(result)
      setBarTick(value => value + 1)
    })
  }

  if (error && filtered.length === 0) {
    return (
      <div className="content" data-editing-domain="symbols">
        <div className="empty-state">
          <p style={{ fontWeight: 600 }}>{t('app.loadFailed')}</p>
          <p>{error.message || String(error)}</p>
          {onRetry ? (
            <button type="button" className="btn" onClick={onRetry}>{t('app.retryLoad')}</button>
          ) : null}
        </div>
      </div>
    )
  }

  if (loading && filtered.length === 0) {
    return (
      <div className="content" data-editing-domain="symbols">
        <div className="empty-state">
          <p style={{ fontWeight: 600 }}>{t('app.loadingSymbols')}</p>
        </div>
      </div>
    )
  }

  if (filtered.length === 0) return (
    <div className="content" data-editing-domain="symbols">
      <div className="empty-state">
        <div className="icon"><svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg></div>
        <p style={{ fontWeight:600 }}>{issueFilter ? t('toast.noIssues') : (filter ? t('app.noFilterMatch') : t('app.noSymbols'))}</p>
        <p>{issueFilter ? '' : (filter ? t('app.noFilterMatchHint') : t('app.noSymbolsHint'))}</p>
      </div>
    </div>
  )

  return (
    <div className="content" data-editing-domain="symbols">
      {selection?.ids?.length > 0 && (
        <div className="selection-bar" data-testid="selection-bar">
          <span className="selection-count">
            {t('status.selected', { count: selection.ids.length, total: filtered.length })}
          </span>
          <button type="button" className="selection-clear" onClick={() => onClearSelection?.()}>
            {t('app.clearSelection')}
          </button>
          <div className="selection-actions">
            {['copy', 'cut', 'paste', 'selectAll'].map(commandId => {
              const state = barStates?.[commandId]
              const label = t(`topbar.${commandId}`)
              const reason = state?.disabled ? (reasonLabel(state.reason, t) || '') : ''
              return (
                <button key={commandId} type="button"
                  className={`selection-action${state?.disabled ? ' disabled' : ''}`}
                  disabled={state?.disabled || undefined}
                  aria-label={label}
                  title={reason || `${label} ${shortcutLabel(commandId, editingRuntime.platform())}`}
                  onClick={() => runBarCommand(commandId)}>
                  {label}
                </button>
              )
            })}
          </div>
        </div>
      )}
      <div className="symbol-list" role="group" aria-label={t('app.symbols')}>
        <div className="symbol-list-head" aria-hidden="true">
          <span />
          <span>{t('app.columnName')}</span>
          <span className="symbol-col-fp">{t('app.columnFootprint')}</span>
          <span className="symbol-col-type">{t('app.columnType')}</span>
          <span>{t('app.columnRelation')}</span>
        </div>
        {filtered.map(s => {
          const fp = footprintView(s)
          const hasModel = modelIsUsable(s)
          const status = !fp.available
            ? { cls: fp.tone === 'ok' ? 'warn' : fp.tone, key: fp.labelKey }
            : hasModel
              ? { cls: 'ok', key: 'symbolStatus.ok' }
              : { cls: 'warn', key: 'symbolStatus.noModel' }
          const reason = fp.reasonKey ? ` (${t(fp.reasonKey)})` : ''
          const selected = isSelected(selection, s.id)
          const active = selection?.activeId === s.id
          // ED-020：selected 是集合，active 是当前对象；集合里的非活动项用
          // multi-selected，避免把“最近点击”误当成批量目标。
          const pendingCut = pendingCutIds?.has?.(s.id) === true
          const stateClass = (selected ? (active ? ' selected active' : ' selected multi-selected') : '') +
            (pendingCut ? ' pending-cut' : '')
          return (
            <div key={s.id} className={`symbol-card${stateClass}`}
              role="button" tabIndex={0}
              title={pendingCut ? t('topbar.movePending') : undefined}
              aria-pressed={selected}
              data-symbol-id={s.id}
              onClick={(event) => selectFromEvent(event, s)}
              onContextMenu={(event) => openContextMenu(event, s)}
              onKeyDown={e => {
                if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault(); selectFromEvent(e, s); return
                }
                if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
                  const cards = e.currentTarget.parentElement ? [...e.currentTarget.parentElement.querySelectorAll('.symbol-card')] : []
                  const index = cards.indexOf(e.currentTarget)
                  const next = cards[index + (e.key === 'ArrowDown' ? 1 : -1)]
                  if (next) { e.preventDefault(); next.focus() }
                }
              }}>
              <span className={`symbol-status ${status.cls === 'bad' ? 'bad' : status.cls}`}
                data-testid={`symbol-status-${s.id}`}
                data-status={fp.status}
                title={`${t(status.key)}${reason}`} />
              <div className="symbol-main">
                <div className="symbol-name">{s.name}</div>
                <div className="symbol-sub">
                  {s.pins ? <span className="symbol-meta">{s.pins}p</span> : null}
                  {s.mpn ? <span className="symbol-meta symbol-mpn">{s.mpn}</span> : null}
                  <span className="symbol-source" title={s.library_id || ''}>
                    {libraryNames?.[s.library_id] || s.library_id || '—'}
                  </span>
                  {pendingCut ? <span className="symbol-cut-badge">{t('topbar.movePending')}</span> : null}
                </div>
              </div>
              <span className="symbol-fp-cell" title={fp.reference || ''}>{fp.reference || '—'}</span>
              {/* 类型文字与 Sidebar/Inspector 同源：登记名优先，认不出的 ID 原样显示 */}
              <span className="type-tag">{typeLabelOf(typeMap, s.type_id || s.type, s.type) || 'Unknown'}</span>
              <div className="symbol-accessory">
                <PluginSlot slot="card.accessory"
                  ownerProps={{ workspaceId: workspaceId || null, symbol: s, symbolId: s.id, selected }} />
              </div>
            </div>
          )
        })}
      </div>
      {menu && (
        <div ref={menuRef} className="context-menu symbol-context-menu" role="menu"
          style={{ position: 'fixed', left: menu.x, top: menu.y }}
          onContextMenu={(event) => event.preventDefault()}>
          <div className="context-menu-title">{t('topbar.edit')}</div>
          {EDIT_ITEMS.map(item => {
            const state = itemState(item.id)
            return (
              <button key={item.id} type="button" role="menuitem"
                className={`context-menu-item${state.disabled ? ' disabled' : ''}`}
                aria-disabled={state.disabled || undefined}
                title={itemTitle(item)}
                onClick={() => { if (!state.disabled) runMenuCommand(item.id) }}>
                <span>{itemLabel(item)}</span>
                <span className="context-menu-shortcut">{shortcutLabel(item.id, editingRuntime.platform())}</span>
              </button>
            )
          })}
          {selection && selection.ids.length > 0 && (
            <button type="button" role="menuitem" className="context-menu-item"
              onClick={() => { setMenu(null); onClearSelection?.() }}>
              <span>{t('app.clearSelection')}</span>
            </button>
          )}
          {menuEntries.length > 0 && <div className="context-menu-separator" role="separator" />}
          {menuEntries.length > 0 && (
            <PluginEntries slot="contextmenu.item"
              ownerProps={{ workspaceId: workspaceId || null, targetKind: 'symbol', target: menu.symbol, symbolId: menu.symbol.id }} />
          )}
        </div>
      )}
    </div>
  )
}