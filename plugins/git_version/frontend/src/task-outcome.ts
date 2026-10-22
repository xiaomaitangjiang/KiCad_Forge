// Pure presentation of a Git write task outcome for the primary sidebar.
//
// The host marks a failed write as uncertain when the backend was interrupted
// after it may already have changed the repository. That flag must stay
// separate from the original task state/error/output, and the only actions it
// offers are read-only checks. Never turn uncertainty into a write retry here.

export type TaskOutcomeLanguage = 'zh' | 'en'

/** Structural subset of the host TaskSnapshot needed by the panel. */
export interface TaskOutcomeSource {
  state?: unknown
  error?: unknown
  result?: unknown
  uncertain?: unknown
}

/** Verification is deliberately limited to reads; commit/init are not actions. */
export type TaskVerificationActionId = 'status' | 'history'

export interface TaskVerificationAction {
  id: TaskVerificationActionId
  label: string
}

export interface TaskOutcome {
  /** True only when the host explicitly marks this task uncertain. */
  uncertain: boolean
  /** Original task state, never replaced with a synthetic "uncertain" state. */
  state: string
  /** Original task error, preserved verbatim. */
  error: string
  /** Original result.output, preserved verbatim. */
  output: string
  /**
   * Text for the existing result line. An uncertain write keeps both error and
   * output; an ordinary task keeps the historical error-first, succeeded-only output.
   */
  resultText: string
  /** Empty unless the task is uncertain. */
  title: string
  /** Empty unless the task is uncertain. */
  detail: string
  /** Empty unless the task is uncertain; keeps the original state visible. */
  stateLabel: string
  /** Empty unless the task is uncertain. */
  readOnlyNote: string
  /** Empty unless the task is uncertain. Status/history are both read-only. */
  verifyActions: TaskVerificationAction[]
}

function asText(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

/**
 * Describe a Git task without hiding its original state, error or output.
 * This function is pure: callers supply the language and render the result.
 */
export function describeTaskOutcome(
  task: TaskOutcomeSource | null | undefined,
  language: TaskOutcomeLanguage,
): TaskOutcome {
  const source = task && typeof task === 'object' ? task : null
  const uncertain = source?.uncertain === true
  const state = asText(source?.state)
  const error = asText(source?.error)
  const result = source?.result && typeof source.result === 'object'
    ? source.result as Record<string, unknown>
    : null
  const output = asText(result?.output)
  const parts = [error, output].filter((part) => part !== '')
  // An uncertain outcome must keep every original detail: a timeout can carry
  // both a diagnostic error and a partial output, and neither may be dropped.
  // Ordinary tasks keep the long-standing error-first, succeeded-only output.
  const resultText = uncertain ? parts.join('\n') : error || (state === 'succeeded' ? output : '')

  if (!uncertain) {
    return {
      uncertain: false,
      state,
      error,
      output,
      resultText,
      title: '',
      detail: '',
      stateLabel: '',
      readOnlyNote: '',
      verifyActions: [],
    }
  }

  const zh = language === 'zh'
  const shownState = state || (zh ? '未知' : 'unknown')
  return {
    uncertain: true,
    state,
    error,
    output,
    resultText,
    title: zh ? '结果待核对' : 'Result needs review',
    stateLabel: zh ? `状态：${shownState}` : `State: ${shownState}`,
    detail: zh
      ? `写入结果不确定：任务状态为“${shownState}”，但写入是否已经生效无法确认。请先只读刷新状态和历史核对；不要重复提交或初始化仓库。`
      : `Write outcome is uncertain: the task reported “${shownState}”, but whether the write took effect cannot be confirmed. Refresh status and history (read-only) first; do not commit or initialize again.`,
    readOnlyNote: zh ? '只读核对，不会提交或初始化' : 'Read-only check; it does not commit or initialize',
    verifyActions: [
      { id: 'status', label: zh ? '刷新状态' : 'Refresh status' },
      { id: 'history', label: zh ? '刷新历史' : 'Refresh history' },
    ],
  }
}
