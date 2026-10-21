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
