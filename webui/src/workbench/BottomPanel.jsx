import { useTranslation } from 'react-i18next'

const VIEWS = [
  { id: 'tasks', labelKey: 'workbench.tasks' },
  { id: 'output', labelKey: 'workbench.output' },
  { id: 'problems', labelKey: 'workbench.problems' },
]

function taskOutput(task) {
  if (!task) return ''
  if (task.error) return task.error
  const output = task.result?.output
  if (typeof output === 'string') return output
  if (task.result === undefined) return ''
  try { return JSON.stringify(task.result, null, 2) } catch { return '' }
}

/** Read-only task/output/problems panel. Tasks are observed, never owned. */
export default function BottomPanel({
  activeViewId = 'tasks', onChange, onClose, tasks = [], problems = [],
}) {
  const { t } = useTranslation()
  const counts = { tasks: tasks.length, output: tasks.length, problems: problems.length }
  const latest = tasks[0] || null
  return (
    <section className="bottom-panel" aria-label={t('workbench.bottomPanel')}>
      <div className="bottom-panel-header">
        <div className="bottom-panel-tabs" role="tablist">
          {VIEWS.map(view => (
            <button
              key={view.id}
              type="button"
              role="tab"
              aria-selected={activeViewId === view.id}
              className={`bottom-panel-tab${activeViewId === view.id ? ' selected' : ''}`}
              onClick={() => onChange?.(view.id)}
            >
              {t(view.labelKey)}
              {counts[view.id] > 0 && <span className="bottom-panel-count">{counts[view.id]}</span>}
            </button>
          ))}
        </div>
        <button type="button" className="bottom-panel-close" title={t('workbench.closePanel')} onClick={onClose}>×</button>
      </div>
      <div className={`bottom-panel-body${activeViewId === 'problems' || activeViewId === 'tasks' ? ' bottom-panel-body-list' : ''}`}>
        {activeViewId === 'tasks' && (tasks.length ? (
          <ul className="bottom-panel-list" data-testid="bottom-task-list">
            {tasks.map(task => (
              <li key={task.id} className="bottom-panel-task" data-state={task.state}>
                <span className={`bottom-panel-task-state state-${task.state}`}>{task.state}</span>
                <span className="bottom-panel-task-action" title={task.id}>{task.action}</span>
                <span className="bottom-panel-task-plugin">{task.pluginId}</span>
                {task.phase && <span className="bottom-panel-task-phase">{task.phase}</span>}
                {task.error && <span className="bottom-panel-task-error">{task.error}</span>}
              </li>
            ))}
          </ul>
        ) : <span className="bottom-panel-empty">{t('workbench.noTasks')}</span>)}
        {activeViewId === 'output' && (latest ? (
          <div className="bottom-panel-output" data-testid="bottom-task-output">
            <div className="bottom-panel-output-head">{latest.action} · {latest.state} · {latest.pluginId}</div>
            <pre>{taskOutput(latest) || t('workbench.emptyOutput')}</pre>
          </div>
        ) : <span className="bottom-panel-empty">{t('workbench.noTasks')}</span>)}
        {activeViewId === 'problems' && (problems.length ? (
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
