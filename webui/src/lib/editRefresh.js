// Post-commit read-only refresh (R6).
//
// A successful paste/move/undo/redo only means the write committed. The UI may
// still show the old list if one of the follow-up reads fails. This module keeps
// the outcome classification and UUID selection pure so the App can:
//   - show success only when every required read actually published;
//   - keep the previous data and offer a read-only retry after a failure;
//   - select new/restored objects by UUID only after the list published.

import { committedRefreshOutcome } from './typeMatching.js'

/** Reads that must publish before the view counts as synchronized. */
export const EDIT_REFRESH_SOURCES = Object.freeze(['symbols', 'libraries', 'status', 'history'])

/** Normalize a refresh result into the shape rendered by the App. */
export function describeEditRefresh(results) {
  const report = committedRefreshOutcome(results, EDIT_REFRESH_SOURCES)
  const failed = report.details.filter((detail) => detail.status === 'failed')
  const superseded = report.details.filter((detail) => detail.status === 'superseded')
  return {
    status: report.status,
    details: report.details,
    failedSources: failed.map((detail) => detail.source),
    supersededSources: superseded.map((detail) => detail.source),
    error: failed.map((detail) => detail.error).filter(Boolean).join('; '),
  }
}

/**
 * UUIDs that are actually selectable in the published list.
 *
 * When locating a committed target, pass its library id: a UUID that exists in
 * another library must not be selected under the new target. Missing ids are
 * returned separately so the caller can report an incomplete refresh instead
 * of silently selecting fewer objects.
 */
export function selectableCommitIds(symbols, ids, libraryId = '') {
  const wanted = [...new Set((Array.isArray(ids) ? ids : []).filter((id) => typeof id === 'string' && id))]
  const rows = Array.isArray(symbols) ? symbols : []
  const byId = new Map(rows
    .filter((row) => row && typeof row.id === 'string' && row.id)
    .map((row) => [row.id, row]))
  const selected = []
  const missing = []
  for (const id of wanted) {
    const row = byId.get(id)
    if (!row || (libraryId && row.library_id !== libraryId)) missing.push(id)
    else selected.push(id)
  }
  return { selected, missing }
}

/** True when any required source failed; superseded reads are not failures. */
export function editRefreshFailed(report) {
  return report?.status === 'failed'
}

// ---- Post-commit refresh ownership (N1) ----------------------------------
//
// A refresh is read-only, but it was planned against the query/workspace/route
// the user had when the write committed. Every user query change, workspace or
// center-page switch and every newer refresh makes that plan obsolete: the old
// flow must not publish its list, rewrite the search/target library, select an
// object, clear a newer notice or toast success. These helpers keep the frozen
// identity and the comparisons pure so the App and node tests share one rule.

function text(value) {
  return typeof value === 'string' ? value : ''
}

/**
 * Monotonic refresh attempt allocator.
 *
 * The attempt is ownership identity, not a property of the currently visible
 * notice: clearing/dismissing a notice must never let a later refresh reuse an
 * older id while that older flow is still in flight. Callers keep the previous
 * value in a dedicated ref and never derive it from notice state.
 */
export function nextEditRefreshAttempt(previous) {
  return (Number.isSafeInteger(previous) && previous >= 0 ? previous : 0) + 1
}

/** Normalize the query carried by an edit-refresh request. */
export function editRefreshQuery(query) {
  return { search: text(query?.search), targetLib: text(query?.targetLib) }
}

/**
 * Freeze the identity a refresh attempt owns. Attempt is the App's notice
 * attempt; intentEpoch covers user query/filter/workspace/route changes, while
 * noticeEpoch lets a dismissed notice invalidate a late flow. selectionEpoch is
 * kept separately: a user selection change only suppresses the auto-selection,
 * not the already-requested list publication.
 */
export function captureEditRefreshOwnership({ attempt, request, context }) {
  return Object.freeze({
    attempt,
    intentEpoch: context?.intentEpoch ?? 0,
    workspaceId: text(context?.workspaceId),
    route: text(context?.route),
    noticeEpoch: context?.noticeEpoch ?? 0,
    selectionEpoch: context?.selectionEpoch ?? 0,
    query: Object.freeze(editRefreshQuery(request?.query)),
  })
}

/** True while this attempt is still the newest one for the same user intent. */
export function editRefreshOwnershipHolds(ownership, context) {
  return Boolean(ownership && context)
    && ownership.attempt === context.attempt
    && ownership.intentEpoch === context.intentEpoch
    && ownership.workspaceId === text(context.workspaceId)
    && ownership.route === text(context.route)
    && ownership.noticeEpoch === context.noticeEpoch
}

/** Compare only the user intent (not notice attempts/selection). */
export function sameEditRefreshIntent(left, right) {
  return Boolean(left && right)
    && left.intentEpoch === (right.intentEpoch ?? 0)
    && left.workspaceId === text(right.workspaceId)
    && left.route === text(right.route)
}

/**
 * Plan the read-only retry for a failed refresh. If the user's query/workspace/
 * route changed after the failure, retry only re-reads the current view; it must
 * not reuse the failed request's query, target library, locate step or success
 * toast. When the intent is unchanged the original read-only request (same ids,
 * locate and success key) is safe to replay.
 */
export function planEditRefreshRetry(notice, { query, context } = {}) {
  if (!notice || notice.status === 'refreshing') return null
  const reused = sameEditRefreshIntent(notice.context, context)
    && (notice.context?.selectionEpoch ?? 0) === (context?.selectionEpoch ?? 0)
  const common = {
    kind: notice.kind,
    targetLibraryId: text(notice.targetLibraryId),
  }
  if (!reused) {
    return {
      reused: false,
      request: {
        ...common,
        ids: [],
        query: editRefreshQuery(query),
        locate: false,
        requireAll: false,
        successKey: null,
        successParams: undefined,
      },
    }
  }
  return {
    reused: true,
    request: {
      ...common,
      ids: Array.isArray(notice.ids) ? [...notice.ids] : [],
      query: editRefreshQuery(notice.query),
      locate: notice.locate === true,
      requireAll: notice.requireAll === true,
      successKey: notice.successKey || null,
      successParams: notice.successParams,
    },
  }
}

/**
 * Run one post-commit refresh against a frozen ownership snapshot.
 *
 * The probe + parallel reads only classify the write's readback; the symbol
 * list itself is still published through the request model so its latest-epoch
 * rule keeps working. Ownership is re-checked before and after that second
 * publication, after every await, and before any UI side effect. A superseded
 * flow clears only its own notice (never a newer one) and returns without
 * touching the list, query, selection or toast.
 */
export async function runEditRefreshFlow({
  request,
  attempt,
  readContext,
  getNoticeAttempt,
  reads,
  present,
  toast = () => {},
  translate = (key) => key,
  selectable = selectableCommitIds,
}) {
  if (!request || !reads || !present) throw new TypeError('edit refresh flow needs request, reads and present')
  const pending = { ...request, attempt, status: 'refreshing', error: '' }
  present.pending(pending)
  const ownership = captureEditRefreshOwnership({ attempt, request, context: readContext() })
  const query = ownership.query
  const owns = () => editRefreshOwnershipHolds(ownership, readContext())
    && getNoticeAttempt() === attempt
  const release = () => present.clear(attempt)

  const probe = Promise.resolve()
    .then(() => reads.probe(query.search, query.targetLib))
    .then(data => ({ status: Array.isArray(data) ? 'published' : 'failed', data }))
    .catch(error => ({ status: 'failed', error }))
  const results = await Promise.allSettled([
    probe,
    Promise.resolve().then(() => reads.libraries()),
    Promise.resolve().then(() => reads.status()),
    Promise.resolve().then(() => reads.history()),
  ])
  const report = describeEditRefresh(results)
  if (!owns()) {
    release()
    return { ...report, status: 'superseded', reason: 'ownership' }
  }
  if (report.status !== 'published') {
    const error = report.error || translate('toast.failedUnknown')
    present.failed({ ...request, attempt, status: report.status, error, context: readContext() })
    toast(report.status === 'failed'
      ? translate('topbar.commitRefreshFailed', { error })
      : translate('topbar.commitRefreshSuperseded'))
    return report
  }

  // Second read: must still own the intent both immediately before the request
  // model takes a new epoch and after its response arrives.
  if (!owns()) {
    release()
    return { ...report, status: 'superseded', reason: 'ownership' }
  }
  const publish = await reads.publish(query.search, query.targetLib)
  if (!owns()) {
    release()
    return { ...report, status: 'superseded', reason: 'ownership' }
  }
  if (publish?.status !== 'published') {
    const error = publish?.error?.message || publish?.error || translate('toast.failedUnknown')
    const next = { ...request, attempt, status: publish?.status || 'failed', error, context: readContext() }
    present.failed(next)
    toast(translate('topbar.commitRefreshFailed', { error }))
    return { ...report, status: next.status }
  }

  // 发布已落地，但改查询/筛选、选中和清通知仍只属于发起时的那次刷新。
  // 到这里没有 await，复核仍是显式的：宁可留下旧值，也不覆盖后来的用户意图。
  if (!owns()) {
    release()
    return { ...report, status: 'superseded', reason: 'ownership' }
  }
  if (request.locate) present.applyQuery(request.targetLibraryId || '')
  const { selected, missing } = selectable(publish.data, request.ids, request.targetLibraryId || '')
  if (missing.length > 0) {
    const error = translate('topbar.commitRefreshMissing', { count: missing.length })
    if (request.requireAll) {
      present.failed({ ...request, attempt, status: 'failed', error, missingCount: missing.length, context: readContext() })
      toast(error)
      return { ...report, status: 'failed' }
    }
    release()
    return report
  }
  if (selected.length > 0 && readContext().selectionEpoch === ownership.selectionEpoch) {
    present.select(selected)
  }
  if (!owns()) {
    release()
    return { ...report, status: 'superseded', reason: 'ownership' }
  }
  release()
  if (request.successKey) toast(translate(request.successKey, request.successParams || {}))
  return report
}
