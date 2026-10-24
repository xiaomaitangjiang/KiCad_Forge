// Paste/move commit state machine (R4).
//
// The dialog must never re-run preflight after a write whose response was lost:
// doing so reports the already-created objects as conflicts and can leave the
// cut token consumed. This module freezes the request once, distinguishes a
// deterministic rejection from an unknown transport outcome, and retries the
// original commit (same operation_id, same target_digest) while uncertain.

export const COMMIT_PHASES = Object.freeze({
  idle: 'idle',
  preflight: 'preflight',
  committing: 'committing',
  uncertain: 'uncertain',
  committed: 'committed',
})

const BUSY_PHASES = new Set([
  COMMIT_PHASES.preflight,
  COMMIT_PHASES.committing,
  COMMIT_PHASES.uncertain,
  COMMIT_PHASES.committed,
])

/** A lost reply / timeout / 5xx may mean the write already succeeded. */
export function isUncertainCommitError(error) {
  if (!error) return false
  if (error.kind === 'network' || error.kind === 'parse') return true
  const status = Number(error.status)
  if (!Number.isFinite(status)) return true
  return status >= 500 || status === 408 || status === 429
}

function freezeRequest(request) {
  if (!request || typeof request !== 'object') return {}
  return {
    ...request,
    renames: { ...(request.renames || {}) },
    items: Array.isArray(request.items) ? request.items.map((item) => ({ ...item })) : [],
  }
}

export function createCommitSession(options = {}) {
  const preflight = typeof options.preflight === 'function' ? options.preflight : null
  const commit = typeof options.commit === 'function' ? options.commit : null
  let phase = COMMIT_PHASES.idle
  let frozen = null
  let plan = null
  let result = null
  let error = null

  function state() {
    return {
      phase,
      busy: BUSY_PHASES.has(phase),
      frozen,
      plan,
      result,
      error,
      canRetry: phase === COMMIT_PHASES.uncertain && Boolean(frozen),
    }
  }

  function reset() {
    if (phase === COMMIT_PHASES.preflight || phase === COMMIT_PHASES.committing) return false
    phase = COMMIT_PHASES.idle
    frozen = null
    plan = null
    result = null
    error = null
    return true
  }

  /** Start one attempt. Only a preflighted plan may proceed to commitPlan(). */
  async function submit(request) {
    if (phase !== COMMIT_PHASES.idle || !preflight) {
      return { status: 'ignored', state: state() }
    }
    phase = COMMIT_PHASES.preflight
    error = null
    result = null
    frozen = freezeRequest(request)
    try {
      plan = await preflight(frozen)
      return { status: 'preflighted', plan, state: state() }
    } catch (cause) {
      phase = COMMIT_PHASES.idle
      frozen = null
      plan = null
      error = cause
      return { status: 'failed', stage: 'preflight', error: cause, state: state() }
    }
  }

  /** Call only after the UI accepted the plan (no conflicts, no blocking, no noop). */
  async function commitPlan() {
    if (phase !== COMMIT_PHASES.preflight || !frozen || !commit) {
      return { status: 'ignored', state: state() }
    }
    phase = COMMIT_PHASES.committing
    return runCommit()
  }

  async function runCommit() {
    try {
      result = await commit(frozen, plan)
      phase = COMMIT_PHASES.committed
      error = null
      return { status: 'committed', result, plan, state: state() }
    } catch (cause) {
      if (isUncertainCommitError(cause)) {
        phase = COMMIT_PHASES.uncertain
        error = cause
        return { status: 'uncertain', error: cause, state: state() }
      }
      phase = COMMIT_PHASES.idle
      error = cause
      result = null
      return { status: 'failed', stage: 'commit', error: cause, state: state() }
    }
  }

  /** Retry the exact frozen commit; never preflight, rename or change the target. */
  async function retry() {
    if (phase !== COMMIT_PHASES.uncertain || !frozen || !commit) {
      return { status: 'ignored', state: state() }
    }
    phase = COMMIT_PHASES.committing
    return runCommit()
  }

  /** Cancel an uncommitted preflight so the user can change target/renames. */
  function cancel() {
    if (phase !== COMMIT_PHASES.preflight) return false
    phase = COMMIT_PHASES.idle
    frozen = null
    plan = null
    error = null
    return true
  }

  return { state, submit, commitPlan, retry, cancel, reset }
}
