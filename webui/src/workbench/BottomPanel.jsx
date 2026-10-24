import { useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { formatTaskDiagnosticTime, formatTaskDiagnostics } from '../lib/task-diagnostics'

const VIEWS = [
  { id: 'tasks', labelKey: 'workbench.tasks' },
  { id: 'problems', labelKey: 'workbench.problems' },
]
const ERROR_PREVIEW_CHARS = 120

function resolveViewId(value) {
  // 旧配置里的 output 在 layout 层已迁移；这里再做一次防御，避免中心面板闪现已删除视图。
  return value === 'problems' ? 'problems' : 'tasks'
}

/** Read-only task/problems panel. Tasks are observed, never owned. */
export default function BottomPanel({
  activeViewId = 'tasks', onChange, onClose, tasks = [], problems = [],
}) {
  const { t } = useTranslation()
  const [expandedErrors, setExpandedErrors] = useState(() => new Set())
  const [copyState, setCopyState] = useState(null)
  const copyTimerRef = useRef(null)
  const viewId = resolveViewId(activeViewId)
  const counts = { tasks: tasks.length, problems: problems.length }

  useEffect(() => () => {
    if (copyTimerRef.current) clearTimeout(copyTimerRef.current)
  }, [])

  function toggleError(taskId) {
    setExpandedErrors(previous => {
      const next = new Set(previous)
      if (next.has(taskId)) next.delete(taskId)
      else next.add(taskId)
      return next
    })
  }

  async function copyDiagnostics(task) {
    if (copyTimerRef.current) clearTimeout(copyTimerRef.current)
    try {
      if (typeof navigator === 'undefined' || !navigator.clipboard?.writeText) {
        throw new Error('clipboard unavailable')
      }
      await navigator.clipboard.writeText(formatTaskDiagnostics(task))
      setCopyState({ id: task.id, ok: true })
    } catch {
      setCopyState({ id: task.id, ok: false })
    }
    copyTimerRef.current = setTimeout(() => setCopyState(null), 2600)
  }

  return (
    <section className="bottom-panel" aria-label={t('workbench.bottomPanel')}>
      <div className="bottom-panel-header">
        <div className="bottom-panel-tabs" role="tablist">
          {VIEWS.map(view => (
            <button
              key={view.id}
              type="button"
              role="tab"
              data-view={view.id}
              aria-selected={viewId === view.id}
              className={`bottom-panel-tab${viewId === view.id ? ' selected' : ''}`}
              onClick={() => onChange?.(view.id)}
            >
              {t(view.labelKey)}
              {counts[view.id] > 0 && <span className="bottom-panel-count">{counts[view.id]}</span>}
            </button>
          ))}
        </div>
        <button type="button" className="bottom-panel-close" title={t('workbench.closePanel')} onClick={onClose}>×</button>
      </div>
      <div className="bottom-panel-body bottom-panel-body-list">
        {viewId === 'tasks' && (tasks.length ? (
          <ul className="bottom-panel-list" data-testid="bottom-task-list">
            {tasks.map((task, index) => {
              const error = task.error ? String(task.error) : ''
              const expanded = expandedErrors.has(task.id)
              const timestamp = formatTaskDiagnosticTime(task.updatedAt)
              const errorId = `bottom-task-error-${index}`
              const feedback = copyState?.id === task.id ? copyState : null
              return (
                <li key={task.id} className="bottom-panel-task" data-state={task.state} data-task-id={task.id}>
                  <span className={`bottom-panel-task-state state-${task.state}`}>{task.state}</span>
                  {task.uncertain === true && (
                    <span
                      className="bottom-panel-task-uncertain"
                      data-uncertain="true"
                      role="note"
                      title={t('workbench.taskUncertainHint')}
                      aria-label={t('workbench.taskUncertainHint')}
                    >
                      {t('workbench.taskUncertain')}
                    </span>
                  )}
                  <span className="bottom-panel-task-action" title={task.id}>{task.action}</span>
                  <span className="bottom-panel-task-plugin">{task.pluginId}</span>
                  {task.phase && <span className="bottom-panel-task-phase">{task.phase}</span>}
                  {timestamp && (
                    <time className="bottom-panel-task-time" dateTime={timestamp} title={timestamp}>{timestamp}</time>
                  )}
                  {error && (
                    <span className="bottom-panel-task-error-wrap">
                      <span id={errorId} className={`bottom-panel-task-error${expanded ? ' expanded' : ''}`} title={error}>{error}</span>
                      {error.length > ERROR_PREVIEW_CHARS && (
                        <button
                          type="button"
                          className="bottom-panel-error-toggle"
                          aria-expanded={expanded}
                          aria-controls={errorId}
                          onClick={() => toggleError(task.id)}
                        >
                          {expanded ? t('workbench.hideError') : t('workbench.showError')}
                        </button>
                      )}
                    </span>
                  )}
                  <button
                    type="button"
                    className="bottom-panel-task-copy"
                    aria-label={`${t('workbench.copyDiagnostics')}: ${task.action || task.id}`}
                    title={`${t('workbench.copyDiagnostics')}: ${task.action || task.id}`}
                    onClick={() => copyDiagnostics(task)}
                  >
                    {t('workbench.copyDiagnostics')}
                  </button>
                  {feedback && (
                    <span className={`bottom-panel-copy-feedback${feedback.ok ? '' : ' is-error'}`} role="status" aria-live="polite">
                      {feedback.ok ? t('workbench.diagnosticsCopied') : t('workbench.diagnosticsCopyFailed')}
                    </span>
                  )}
                </li>
              )
            })}
          </ul>
        ) : <span className="bottom-panel-empty">{t('workbench.noTasks')}</span>)}
        {viewId === 'problems' && (problems.length ? (
          <ul className="bottom-panel-list" data-testid="bottom-problem-list">
            {problems.map((problem, index) => (
              <li key={`${problem.source || 'problem'}:${index}`} className={`bottom-panel-problem severity-${problem.severity || 'error'}`}>
                <span className="bottom-panel-problem-source">{problem.source || 'app'}</span>
                <span className="bottom-panel-problem-message">{problem.message}</span>
              </li>
            ))}
          </ul>
        ) : <span className="bottom-panel-empty">{t('workbench.noProblems')}</span>)}
      </div>
    </section>
  )
}
