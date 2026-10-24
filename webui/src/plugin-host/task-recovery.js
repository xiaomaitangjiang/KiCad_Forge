// On-demand recovery for tasks the backend still owns after a frontend reload.
// The host reads each enabled plugin list once, records terminal tasks as
// read-only summaries, and attaches the normal task adapter to live ones.
// It never starts a task, never scans on a timer, and never duplicates a
// poller that is already tracked by taskOverview.
import { createRecoveredTask } from './tasks-api.js'
import { taskOverview } from './task-overview.js'

const recoveredPlugins = new Set()
const inFlight = new Set()

function pluginTaskUrl(pluginId) {
  return '/api/plugins/' + encodeURIComponent(pluginId) + '/tasks'
}

export async function recoverTasks(pluginIds, {
  fetchImpl = globalThis.fetch, overview = taskOverview,
} = {}) {
  const targets = [...new Set(pluginIds)]
    .filter(id => typeof id === 'string' && id && !recoveredPlugins.has(id) && !inFlight.has(id))
  await Promise.all(targets.map(async (pluginId) => {
    inFlight.add(pluginId)
    try {
      const response = await fetchImpl(pluginTaskUrl(pluginId), { cache: 'no-store' })
      if (!response.ok) return
      const body = await response.json()
      const tasks = Array.isArray(body?.tasks) ? body.tasks : []
      // The list is newest-first; apply oldest first so the newest entry wins
      // the overview ordering.
      for (const task of [...tasks].reverse()) {
        const id = task?.task_id ?? task?.id
        if (!id || overview.hasTask(id)) continue
        createRecoveredTask(pluginId, task, { fetchImpl, overview })
      }
      recoveredPlugins.add(pluginId)
    } catch {
      // A transient read failure stays retryable on the next panel open.
    } finally {
      inFlight.delete(pluginId)
    }
  }))
}

/** Test-only: clear the per-page recovery guard. */
export function resetTaskRecovery() {
  recoveredPlugins.clear()
  inFlight.clear()
}
