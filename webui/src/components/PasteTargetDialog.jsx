import { useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Modal } from './Modals'
import { api } from '../api'
import { conflictNames, pasteBudgetViolation, pasteItems, unresolvedConflicts } from '../editing/paste-model'
import { blockingMoveConflicts, moveDirectItems, moveNameConflicts, moveRequest } from '../editing/move-model'
import { createCommitSession } from '../editing/paste-commit'

// 对象粘贴/移动的目标选择与冲突处理（ED-031/032、ED-042）。
// 预检不写文件：确认按钮先 dry_run，拿到目标摘要与冲突清单；
// 冲突必须由用户显式改名，默认不覆盖；依赖项由宿主处理，这里只展示。
// 移动复用同一对话框：它带 token 走 /api/editing/move，其他原因（缺父、
// 未移动派生、已在目标）不能靠改名解决，必须阻止提交并说明。
export default function PasteTargetDialog({ intent, libraries, initialTargetId, onClose, onCommitted }) {
  const { t } = useTranslation()
  const isMove = intent?.mode === 'move'
  const items = Array.isArray(intent?.items) ? intent.items : []
  const candidates = useMemo(
    () => (libraries || []).filter(lib => !lib.locked && lib.id !== 'default'),
    [libraries],
  )
  const [targetId, setTargetId] = useState(() => (
    candidates.some(lib => lib.id === initialTargetId) ? initialTargetId : (candidates[0]?.id || '')
  ))
  const [renames, setRenames] = useState({})
  const [conflicts, setConflicts] = useState([])
  const [blocking, setBlocking] = useState([])
  const [warnings, setWarnings] = useState([])
  const [noop, setNoop] = useState(false)
  const [phase, setPhase] = useState('idle')
  const [error, setError] = useState('')
  const phaseRef = useRef('idle')
  const busy = phase !== 'idle'
  const operationIdRef = useMemo(
    () => ({ current: globalThis.crypto?.randomUUID?.() || `edit-${Date.now()}-${Math.random().toString(16).slice(2)}` }),
    [],
  )

  const selectedLibrary = candidates.find(lib => lib.id === targetId)
  const payloadItems = pasteItems({ items })
  const directItems = isMove ? moveDirectItems(items) : items
  const unresolved = unresolvedConflicts(conflicts, renames)

  function changeTarget(value) {
    setTargetId(value)
    setConflicts([])
    setBlocking([])
    setWarnings([])
    setNoop(false)
    setRenames({})
    setError('')
  }

  function moveToPhase(next) {
    phaseRef.current = next
    setPhase(next)
  }

  function failureMessage(result, fallbackKey) {
    const failure = new Error(result?.error || t(fallbackKey, { error: '' }))
    failure.code = result?.code
    return failure
  }

  function buildPreflightRequest() {
    if (isMove) {
      return moveRequest({
        token: intent?.token, targetId, operationId: operationIdRef.current,
        dryRun: true, renames,
      })
    }
    return {
      target_library_id: targetId,
      operation_id: operationIdRef.current,
      dry_run: true,
      renames,
      items: payloadItems,
    }
  }

  // 一次提交只冻结一份请求：预检和真正提交共用同一 operation_id / 目标摘要。
  // 回包丢失时只重放这份请求，不再预检（预检会把已建对象报成同名冲突，R4）。
  const session = useMemo(() => createCommitSession({
    preflight: async (body) => {
      const result = isMove
        ? await api.moveSymbols({ ...body, dry_run: true })
        : await api.pasteSymbols({ ...body, dry_run: true })
      if (!result?.ok) throw failureMessage(result, isMove ? 'topbar.moveFailed' : 'topbar.pasteFailed')
      return result.plan || null
    },
    commit: async (body, nextPlan) => {
      const request = { ...body, dry_run: false }
      if (nextPlan?.target?.digest) request.target_digest = nextPlan.target.digest
      const result = isMove ? await api.moveSymbols(request) : await api.pasteSymbols(request)
      if (!result?.ok) {
        const failure = failureMessage(result, isMove ? 'topbar.moveFailed' : 'topbar.pasteFailed')
        // 200 但业务失败属于明确拒绝；交给状态机回到 idle，不进入 uncertain。
        failure.kind = 'http'
        failure.status = 400
        throw failure
      }
      return result
    },
  }), []) // eslint-disable-line react-hooks/exhaustive-deps

  function applyPlan(nextPlan) {
    const nextConflicts = isMove ? moveNameConflicts(nextPlan) : conflictNames(nextPlan)
    const nextBlocking = isMove ? blockingMoveConflicts(nextPlan) : []
    setConflicts(nextConflicts)
    setBlocking(nextBlocking)
    setWarnings(Array.isArray(nextPlan?.warnings) ? nextPlan.warnings : [])
    setNoop(Boolean(nextPlan?.noop))
    return {
      nextConflicts,
      nextBlocking,
      stop: nextBlocking.length > 0 || Boolean(nextPlan?.noop)
        || unresolvedConflicts(nextConflicts, renames).length > 0,
    }
  }

  function finish(outcome) {
    if (outcome.status === 'committed') {
      moveToPhase('committed')
      setError('')
      onCommitted?.(outcome.result, outcome.plan)
      return
    }
    if (outcome.status === 'uncertain') {
      moveToPhase('uncertain')
      setError(t('topbar.commitUnknown'))
      return
    }
    moveToPhase('idle')
    if (outcome.error) setError(outcome.error.message || String(outcome.error))
  }

  async function submit() {
    if (!targetId || phaseRef.current !== 'idle') return
    if (!isMove && pasteBudgetViolation(payloadItems, renames)) {
      setError(t('topbar.clipboardTooLarge'))
      return
    }
    setError('')
    moveToPhase('preflight')
    const started = await session.submit(buildPreflightRequest())
    if (started.status !== 'preflighted') {
      finish(started)
      return
    }
    const decision = applyPlan(started.plan)
    if (decision.stop) {
      session.cancel()
      moveToPhase('idle')
      return
    }
    moveToPhase('committing')
    finish(await session.commitPlan())
  }

  async function retryCommit() {
    if (phaseRef.current !== 'uncertain') return
    setError('')
    moveToPhase('committing')
    finish(await session.retry())
  }

  return (
    <Modal title={t(isMove ? 'topbar.moveTitle' : 'topbar.pasteTitle')} className="paste-target-modal" onClose={busy ? undefined : onClose}>
      <div className="paste-target-body">
        <p className="paste-target-summary">{intent?.summary || t(isMove ? 'topbar.moveSummary' : 'topbar.pasteSummary', { count: directItems.length })}</p>
        <div className="paste-item-list">
          {items.map((item, index) => (
            <div key={`${item.name}-${index}`} className="paste-item">
              <span>{item.name}</span>
              {item.dependency ? <span className="paste-item-dep">{t('topbar.pasteDependency')}</span> : null}
            </div>
          ))}
        </div>

        {candidates.length === 0 ? (
          <p className="paste-target-error" role="alert">{t('topbar.pasteNoTarget')}</p>
        ) : (
          <label className="paste-target-field">
            <span>{t('topbar.pasteTarget')}</span>
            <select value={targetId} onChange={(event) => changeTarget(event.target.value)} disabled={busy}>
              {candidates.map(lib => (
                <option key={lib.id} value={lib.id}>{lib.name}</option>
              ))}
            </select>
          </label>
        )}
        {selectedLibrary?.file_path ? (
          <p className="paste-target-path">{selectedLibrary.file_path}</p>
        ) : null}

        {noop ? <p className="paste-target-note">{t('topbar.moveNoop')}</p> : null}
        {warnings.length > 0 ? (
          <div className="paste-warnings" role="status">
            {warnings.map((warning, index) => (
              <p key={`${index}-${warning}`} className="paste-target-note">{warning}</p>
            ))}
          </div>
        ) : null}
        {blocking.length > 0 ? (
          <div className="paste-conflicts" role="alert">
            <p>{t('topbar.moveBlocked')}</p>
            {blocking.map(conflict => (
              <p key={`${conflict.name}-${conflict.reason}`} className="paste-conflict-row">
                <span>{conflict.name}</span>
                <span>{t(`topbar.moveConflict.${conflict.reason}`, { defaultValue: conflict.reason })}</span>
              </p>
            ))}
          </div>
        ) : null}
        {conflicts.length > 0 ? (
          <div className="paste-conflicts" role="group" aria-label={t('topbar.pasteConflicts')}>
            <p>{t('topbar.pasteConflictHint')}</p>
            {conflicts.map(name => (
              <label key={name} className="paste-conflict-row">
                <span>{name}</span>
                <input value={renames[name] || ''} disabled={busy}
                  placeholder={t('topbar.pasteNewName')}
                  onChange={(event) => setRenames(current => ({ ...current, [name]: event.target.value }))} />
              </label>
            ))}
          </div>
        ) : null}

        {phase === 'uncertain' ? <p className="paste-target-note">{t('topbar.commitUncertainHint')}</p> : null}
        {error ? <p className="paste-target-error" role="alert">{error}</p> : null}
        <div className="btn-row confirm-actions">
          <button type="button" className="btn-ghost" onClick={onClose} disabled={busy}>
            {t('topbar.cancel')}
          </button>
          {phase === 'uncertain' ? (
            <button type="button" className="btn-primary" onClick={retryCommit}>
              {t('topbar.retryCommit')}
            </button>
          ) : (
            <button type="button" className="btn-primary" onClick={submit}
              disabled={busy || !targetId || candidates.length === 0 || noop || blocking.length > 0 || unresolved.length > 0}>
              {phase === 'preflight' ? t('topbar.checking')
                : phase === 'committing' ? t(isMove ? 'topbar.moving' : 'topbar.pasting')
                  : t(isMove ? 'topbar.move' : 'topbar.paste')}
            </button>
          )}
        </div>
      </div>
    </Modal>
  )
}