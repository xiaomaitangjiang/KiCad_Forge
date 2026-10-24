import { useEffect, useLayoutEffect, useMemo, useSyncExternalStore } from 'react'
import { useTranslation } from 'react-i18next'
import { Modal } from './Modal.jsx'
import { candidateIdentity, candidateSelectable, formatScore, symbolLibraryPath } from './lcscImport.js'
import { createLcscModel } from './model.js'
import { footprintView } from './footprintStatus.js'

// 候选行（自动匹配与手动搜索共用同一个组件）：整行就是一个 label，点名称、点 radio、
// 点行内空白都作用在同一行上。UUID 才是身份 —— 没有身份的行不可选，也绝不用名称凑
// 一个 id 出来。选中状态由调用方按「身份 + 点的是哪一侧」算好传进来。
function CandidateRow({ testid, candidate, selectable, checked, disabled, onPick, badges }) {
  const { t } = useTranslation()
  // 服务端状态决定这一行是不是还能用：失效/未检查的候选显示原因并禁用
  const state = footprintView(candidate)
  const reason = candidateIdentity(candidate)
    ? `${t(state.labelKey)}${state.reasonKey ? `（${t(state.reasonKey)}）` : ''}`
    : t('lcsc.candidateNoId')
  return (
    <label
      data-testid={testid}
      data-selectable={selectable ? 'true' : 'false'}
      data-status={state.status}
      className={`lcsc-option${selectable ? '' : ' disabled'}${checked ? ' selected' : ''}`}>
      <input type="radio" name="lcsc-footprint" disabled={disabled || !selectable}
        checked={checked} onChange={onPick} />
      <span className="lcsc-option-body">
        <span className="lcsc-option-name">{candidate.name || '—'}</span>
        {candidate.library && <span className="lcsc-meta"> · {candidate.library}</span>}
        {badges}
        {!selectable && <span className="lcsc-reason"> · {reason}</span>}
      </span>
    </label>
  )
}

// 立创导入专用流程（template="lcsc_import"）：
//   1. 只输编号 → 查找（analyze，不写库）
//   2. 选目标元件库（符号库 + 封装库的包）→ 确认
//   3. 选具体符号子库，或明确新建一个子库
//   4. 在自动匹配候选里显式选一个复用，或选“新建封装” → 确认导入
// 目标文件落点、数据库登记全由宿主决定；这里只负责让用户做出明确选择。
export default function LcscImportModal(props) {
  const { t } = useTranslation()
  const { model: suppliedModel, inline = false, ...options } = props
  // Callbacks and current library inputs are refreshed below; a model is never
  // recreated merely because React rendered another frame.
  const model = useMemo(() => suppliedModel || createLcscModel({ ...options, t }), [suppliedModel]) // eslint-disable-line react-hooks/exhaustive-deps
  useLayoutEffect(() => { model.configure({ ...options, t }) })
  useEffect(() => {
    model.activate()
    return () => { if (!suppliedModel) model.suspend() }
  }, [model, suppliedModel])
  const snapshot = useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)
  const { step, partNumber, inputError, looking, lookupError, device, targetId, target, symbolCatalog, symbolLoading, symbolError, symbolChoice, newSymbolName, choice, choiceFrom, manualQuery, manualResults, manualLoading, manualError, manualDropped, importError, busy, completed, refreshing, refreshError, targetOptions, selectedTarget, selectedTargetUsable, symbolCheck, manualChoice, deviceLabel, liveTargetCheck, canImport, frozenEdits, canBeginNewIntent } = snapshot
  const { requestClose, isFrozen, clearFootprintIntent, changePartNumber, runLookup, selectTarget, confirmTarget, backToTargets, loadSymbolLibraries, selectSymbolLibrary, confirmSymbolLibrary, backToSymbols, pickCandidate, pickNew, runManualSearch, reasonText, refreshAfterImport, runImport, setNewSymbolName, loadCompLibraries } = model
  const pluginAction = snapshot.pluginAction
  const action = pluginAction?.action
  const task = props.task || snapshot.task
  const onCancelTask = props.onCancelTask || model.cancelTask
  if (!pluginAction) return null

  const content = (
    <>
      {task && (
        <div data-testid="lcsc-task-state" data-state={task.state} data-phase={task.phase || ''}
          style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '4px 0 8px', fontSize: 12 }}>
          <span>任务：{task.state}{task.phase ? ` / ${task.phase}` : ''}</span>
          {task.state === 'running' && task.cancelable && (
            <button className="btn-ghost" data-testid="lcsc-task-cancel" onClick={onCancelTask}>取消任务</button>
          )}
        </div>
      )}
      {/* ---- 第一步：编号 ---- */}
      <div className="form-group">
        <label>{t('lcsc.partNumber')} *</label>
        <div style={{ display: 'flex', gap: 8 }}>
          <input
            data-testid="lcsc-id-input"
            value={partNumber}
            disabled={frozenEdits}
            placeholder={t('lcsc.partNumberPlaceholder')}
            onChange={e => changePartNumber(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') runLookup() }}
            style={{ flex: 1 }} />
          <button
            data-testid="lcsc-lookup"
            className="btn-primary"
            style={{ padding: '6px 16px', whiteSpace: 'nowrap' }}
            disabled={looking || frozenEdits}
            onClick={runLookup}>
            {looking ? t('lcsc.looking') : t('lcsc.lookup')}
          </button>
        </div>
        {inputError && (
          <p data-testid="lcsc-input-error" style={{ fontSize: 11.5, color: 'var(--red)', margin: '6px 0 0 0' }}>
            {inputError}
          </p>
        )}
        {lookupError && (
          <p data-testid="lcsc-lookup-error" style={{ fontSize: 11.5, color: 'var(--red)', margin: '6px 0 0 0' }}>
            {t('lcsc.lookupFailed', { error: lookupError })}
          </p>
        )}
        {!device && !lookupError && !inputError && (
          <p style={{ fontSize: 11, color: 'var(--text3)', margin: '6px 0 0 0' }}>{t('lcsc.lookupHint')}</p>
        )}
      </div>

      {importError && (
        <p data-testid="lcsc-import-error" className="lcsc-warning" style={{ color: 'var(--red)' }}>
          {importError}
        </p>
      )}

      {/* ---- 第二步：目标元件库 ---- */}
      {device && step === 'target' && (
        <div data-testid="lcsc-target-step">
          <div data-testid="lcsc-device" className="lcsc-device">
            <div><b>{deviceLabel}</b></div>
            <div className="lcsc-meta">
              {t('lcsc.partNumber')}: {partNumber.trim() || device.id}
              {device.footprintName ? ` · ${t('lcsc.originalFootprint')}: ${device.footprintName}` : ''}
              {device.modelName ? ` · 3D: ${device.modelName}` : ''}
            </div>
          </div>
          {device.warning && <p className="lcsc-warning" data-testid="lcsc-warning">{device.warning}</p>}

          <div className="form-group" style={{ marginTop: 12 }}>
            <label>{t('lcsc.target')} *</label>
            <p style={{ fontSize: 11, color: 'var(--text2)', margin: '0 0 6px 0' }}>{t('lcsc.targetHint')}</p>
            <div data-testid="lcsc-target-list" className="lcsc-list">
              {targetOptions.map(({ lib, check }) => (
                <label
                  key={lib.id}
                  data-testid={`lcsc-target-${lib.id}`}
                  data-selectable={check.ok ? 'true' : 'false'}
                  className={`lcsc-option${check.ok ? '' : ' disabled'}${targetId === lib.id ? ' selected' : ''}`}>
                  <input type="radio" name="lcsc-target" disabled={!check.ok || frozenEdits}
                    checked={targetId === lib.id} onChange={() => selectTarget(lib)} />
                  <span className="lcsc-option-body">
                    <span className="lcsc-option-name">{lib.name}</span>
                    {!check.ok && <span className="lcsc-reason"> · {reasonText(check.reason)}</span>}
                    <span className="lcsc-meta">
                      {t('settings.symbolPath')}: {lib.symbol_path || '—'} · {t('settings.footprintPath')}: {lib.footprint_path || '—'}
                    </span>
                  </span>
                </label>
              ))}
              {targetOptions.length === 0 && (
                <p style={{ fontSize: 11.5, color: 'var(--text2)', margin: 0 }}>{t('lcsc.noTargets')}</p>
              )}
            </div>
            {selectedTarget && !selectedTargetUsable && (
              <p data-testid="lcsc-target-blocked" className="lcsc-warning" style={{ color: 'var(--red)' }}>
                {t('lcsc.targetBlocked', { reason: reasonText(selectedTarget.check.reason) })}
              </p>
            )}
            <div className="btn-row" style={{ marginTop: 12 }}>
              <button className="btn-ghost" data-testid="lcsc-refresh-targets" disabled={frozenEdits}
                onClick={() => { if (!isFrozen()) loadCompLibraries?.() }}>{t('lcsc.refreshList')}</button>
              <button className="btn-primary" data-testid="lcsc-target-next"
                disabled={!targetId || !selectedTargetUsable || frozenEdits}
                onClick={confirmTarget}>{t('lcsc.next')}</button>
            </div>
          </div>
        </div>
      )}

      {/* ---- 第三步：具体符号子库 ---- */}
      {device && step === 'symbol' && (
        <div data-testid="lcsc-symbol-step">
          <div className="lcsc-device"><b>{deviceLabel}</b> · {target?.name}</div>
          {!liveTargetCheck.ok && <p className="lcsc-warning" data-testid="lcsc-symbol-target-invalid" style={{ color: 'var(--red)' }}>
            {t('lcsc.targetBlocked', { reason: reasonText(liveTargetCheck.reason) })}
          </p>}
          <div className="form-group" style={{ marginTop: 12 }}>
            <label>{t('lcsc.symbolLibrary')} *</label>
            <p className="lcsc-meta">{t('lcsc.symbolLibraryHint')}</p>
            {symbolLoading && <p data-testid="lcsc-symbol-loading">{t('lcsc.symbolLoading')}</p>}
            {symbolError && <p data-testid="lcsc-symbol-error" className="lcsc-warning" style={{ color: 'var(--red)' }}>
              {t('lcsc.symbolLoadFailed', { error: symbolError })}</p>}
            {symbolCatalog?.ok && !symbolLoading && <div data-testid="lcsc-symbol-list" className="lcsc-list">
              {symbolCatalog.libraries.map(lib => <label key={lib.path}
                data-testid={`lcsc-symbol-${lib.path}`} className={`lcsc-option${symbolChoice?.mode === 'existing' && symbolChoice.path === lib.path ? ' selected' : ''}`}>
                <input type="radio" name="lcsc-symbol" disabled={frozenEdits}
                  checked={symbolChoice?.mode === 'existing' && symbolChoice.path === lib.path}
                  onChange={() => selectSymbolLibrary({ mode: 'existing', path: lib.path })} />
                <span className="lcsc-option-body"><span className="lcsc-option-name">{lib.name}</span>
                  <span className="lcsc-meta">{lib.path}</span></span>
              </label>)}
              {!symbolCatalog.libraries.length && <p data-testid="lcsc-symbol-empty" className="lcsc-meta">{t('lcsc.symbolEmpty')}</p>}
              {symbolCatalog.canCreate && <label data-testid="lcsc-symbol-new"
                className={`lcsc-option${symbolChoice?.mode === 'new' ? ' selected' : ''}`}>
                <input type="radio" name="lcsc-symbol" disabled={frozenEdits} checked={symbolChoice?.mode === 'new'}
                  onChange={() => selectSymbolLibrary({ mode: 'new', name: newSymbolName })} />
                <span className="lcsc-option-body"><span className="lcsc-option-name">{t('lcsc.newSymbolLibrary')}</span></span>
              </label>}
            </div>}
            {symbolCatalog?.ok && symbolChoice?.mode === 'new' && <input data-testid="lcsc-symbol-name"
              value={newSymbolName} disabled={frozenEdits} placeholder={t('lcsc.symbolNamePlaceholder')}
              onChange={e => { if (!isFrozen()) { setNewSymbolName(e.target.value); clearFootprintIntent() } }} />}
            {symbolCatalog?.ok && symbolChoice && !symbolCheck.ok && <p data-testid="lcsc-symbol-validation"
              className="lcsc-warning" style={{ color: 'var(--red)' }}>{t(`lcsc.symbolReason.${symbolCheck.reason}`)}</p>}
            <div className="btn-row" style={{ marginTop: 12 }}>
              <button className="btn-ghost" data-testid="lcsc-symbol-back" disabled={frozenEdits}
                onClick={backToTargets}>{t('lcsc.back')}</button>
              <button className="btn-ghost" data-testid="lcsc-symbol-refresh" disabled={frozenEdits || symbolLoading}
                onClick={() => loadSymbolLibraries()}>{t('lcsc.refreshList')}</button>
              <button className="btn-primary" data-testid="lcsc-symbol-next"
                disabled={frozenEdits || symbolLoading || !symbolCheck.ok || !liveTargetCheck.ok}
                onClick={confirmSymbolLibrary}>{t('lcsc.next')}</button>
            </div>
          </div>
        </div>
      )}

      {/* ---- 第四步：候选封装 + 确认 ---- */}
      {device && step === 'footprint' && (
        <div data-testid="lcsc-footprint-step">
          <div data-testid="lcsc-device" className="lcsc-device">
            <div><b>{deviceLabel}</b></div>
            <div className="lcsc-meta">
              {t('lcsc.partNumber')}: {device.id || partNumber.trim()}
              {device.footprintName ? ` · ${t('lcsc.originalFootprint')}: ${device.footprintName}` : ''}
            </div>
          </div>
          {device.warning && <p className="lcsc-warning" data-testid="lcsc-warning">{device.warning}</p>}
          {device.droppedCandidates > 0 && (
            <p className="lcsc-warning" data-testid="lcsc-candidates-dropped">
              {t('lcsc.candidatesDropped', { count: device.droppedCandidates })}
            </p>
          )}
          {target && !liveTargetCheck.ok && (
            <p className="lcsc-warning" data-testid="lcsc-target-invalid" style={{ color: 'var(--red)' }}>
              {t('lcsc.targetBlocked', { reason: reasonText(liveTargetCheck.reason) })}
            </p>
          )}

          <div className="form-group" style={{ marginTop: 12 }}>
            <label>{t('lcsc.candidates')} *</label>
            <p style={{ fontSize: 11, color: 'var(--text2)', margin: '0 0 6px 0' }}>{t('lcsc.candidatesHint')}</p>
            <div data-testid="lcsc-candidates" className="lcsc-list">
              {device.candidates.map((c, index) => {
                const id = candidateIdentity(c)
                const selectable = candidateSelectable(c)
                return (
                  <CandidateRow
                    key={id || `idx-${index}`}
                    testid={id ? `lcsc-cand-${id}` : `lcsc-cand-none-${index}`}
                    candidate={c}
                    selectable={selectable}
                    disabled={frozenEdits}
                    checked={!!id && choice?.mode === 'reuse' && choiceFrom === 'auto' && choice.id === id}
                    onPick={() => pickCandidate(c, 'auto')}
                    badges={<>
                      {formatScore(c.score) && <span className="lcsc-badge">{formatScore(c.score)}</span>}
                      {c.pad_count != null && <span className="lcsc-badge">{c.pad_count}p</span>}
                    </>} />
                )
              })}
              {device.candidates.length === 0 && (
                <p style={{ fontSize: 11.5, color: 'var(--text2)', margin: 0 }}>
                  {device.warning ? t('lcsc.noCandidatesWarning') : t('lcsc.noCandidates')}
                </p>
              )}
              <label
                data-testid="lcsc-new-footprint"
                data-selectable="true"
                className={`lcsc-option${choice?.mode === 'new' ? ' selected' : ''}`}>
                <input type="radio" name="lcsc-footprint" disabled={frozenEdits}
                  checked={choice?.mode === 'new'} onChange={pickNew} />
                <span className="lcsc-option-body">
                  <span className="lcsc-option-name">{t('lcsc.newFootprint')}</span>
                  <span className="lcsc-meta">
                    {' '}· {t('lcsc.newFootprintHint', { name: device.footprintName || deviceLabel })}
                  </span>
                </span>
              </label>
            </div>
          </div>

          <div className="form-group">
            <label>{t('lcsc.searchOther')}</label>
            <input data-testid="lcsc-manual-search" value={manualQuery} disabled={frozenEdits}
              placeholder={t('lcsc.searchOtherPlaceholder')}
              onChange={e => runManualSearch(e.target.value)} />
            {manualError && (
              <p data-testid="lcsc-manual-error" className="lcsc-warning" style={{ color: 'var(--red)' }}>
                {t('lcsc.manualFailed', { error: manualError })}
              </p>
            )}
            {manualDropped > 0 && (
              <p data-testid="lcsc-manual-dropped" className="lcsc-warning">
                {t('lcsc.candidatesDropped', { count: manualDropped })}
              </p>
            )}
            {manualResults && (
              <div data-testid="lcsc-manual-results" className="lcsc-list" style={{ marginTop: 6 }}>
                {manualResults.map((r, index) => {
                  const id = candidateIdentity(r)
                  const selectable = candidateSelectable(r)
                  return (
                    <CandidateRow
                      key={id || `m-${index}`}
                      testid={id ? `lcsc-manual-${id}` : `lcsc-manual-none-${index}`}
                      candidate={r}
                      selectable={selectable}
                      disabled={frozenEdits}
                      checked={!!id && choice?.mode === 'reuse' && choiceFrom === 'manual' && choice.id === id}
                      onPick={() => pickCandidate(r, 'manual')} />
                  )
                })}
                {manualResults.length === 0 && !manualLoading && (
                  <p style={{ fontSize: 11.5, color: 'var(--text2)', margin: 0 }}>{t('lcsc.noManualResults')}</p>
                )}
              </div>
            )}
          </div>

          <div data-testid="lcsc-summary" className="lcsc-summary">
            <div>{t('lcsc.partNumber')}: <b>{device.id || partNumber.trim()}</b> · {t('lcsc.device')}: <b>{deviceLabel}</b></div>
            <div>{t('lcsc.target')}: <b>{target?.name}</b>
              <span className="lcsc-meta"> · {t('settings.symbolPath')}: {target?.symbolPath || '—'} · {t('settings.footprintPath')}: {target?.footprintPath || '—'}</span>
            </div>
            <div>{t('lcsc.symbolLibrary')}: <b data-testid="lcsc-symbol-summary">{symbolLibraryPath(symbolChoice)}</b></div>
            <div>{t('lcsc.footprint')}: {choice
              ? (choice.mode === 'new'
                ? <b>{t('lcsc.summaryNew')}</b>
                : <b>{t('lcsc.summaryReuse', { name: manualChoice ? `${manualChoice.library || ''}:${manualChoice.name}` : (choice.library ? `${choice.library}:${choice.name}` : choice.name) })}</b>)
              : <span className="lcsc-meta">{t('lcsc.summaryPick')}</span>}
            </div>
          </div>

          {refreshError && (
            <p data-testid="lcsc-refresh-error" className="lcsc-warning" style={{ color: 'var(--red)' }}>
              {t('lcsc.importedRefreshFailed', { error: refreshError })}
            </p>
          )}
          {refreshing && (
            <p data-testid="lcsc-refreshing" style={{ fontSize: 11.5, color: 'var(--text2)', margin: 0 }}>
              {t('lcsc.refreshing')}
            </p>
          )}

          <div className="btn-row" style={{ marginTop: 12 }}>
            <button className="btn-ghost" data-testid="lcsc-back-target" disabled={frozenEdits}
              onClick={backToSymbols}>{t('lcsc.back')}</button>
            <button className="btn-primary" data-testid="lcsc-import" disabled={!canImport || completed}
              onClick={runImport}>{busy ? t('lcsc.importing') : t('lcsc.import')}</button>
            {completed && !canBeginNewIntent && (
              <button className="btn-ghost" data-testid="lcsc-retry-refresh" disabled={refreshing || busy}
                onClick={async () => { if (await refreshAfterImport()) model.requestClose() }}>
                {t('lcsc.retryRefresh')}
              </button>
            )}
            {inline && canBeginNewIntent && (
              <button className="btn-primary" data-testid="lcsc-next-import" onClick={model.beginNewIntent}>
                {t('lcsc.importAnother')}
              </button>
            )}
          </div>
        </div>
      )}

      {!inline && (
        <div className="btn-row" style={{ marginTop: 12 }}>
          <button className="btn-ghost" data-testid="lcsc-close" disabled={busy}
            onClick={requestClose}>{t('plugin.cancel')}</button>
        </div>
      )}
    </>
  )

  if (inline) {
    return <div className="lcsc-inline" data-testid="lcsc-inline">{content}</div>
  }
  return (
    <Modal title={action?.name || t('lcsc.title')} onClose={requestClose}>
      {content}
    </Modal>
  )
}
