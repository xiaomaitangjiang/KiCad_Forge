// Host-side read-only task overview. It observes the same TaskHandle that the
// plugin already polls; it never starts a second request loop and never cancels
// work when the panel closes. Late terminal updates keep a small bounded record.
export const MAX_TASK_OVERVIEW = 20

const TERMINAL = new Set(['succeeded', 'failed', 'cancelled'])

function sameTask(a, b) {
  return a.id === b.id && a.state === b.state && a.phase === b.phase
    && a.error === b.error && a.uncertain === b.uncertain && a.cancelable === b.cancelable
}

export function createTaskOverview({ max = MAX_TASK_OVERVIEW, now = () => Date.now() } = {}) {
  const entries = new Map()
  const listeners = new Set()
  let order = 0
  let snapshot = Object.freeze({ tasks: Object.freeze([]) })

  function publish() {
    const tasks = [...entries.values()]
      .sort((a, b) => b.order - a.order)
      .slice(0, max)
    snapshot = Object.freeze({ tasks: Object.freeze(tasks.map(task => Object.freeze(task))) })
    for (const listener of [...listeners]) listener()
  }

  function getSnapshot() { return snapshot }

  /** True when this task is already in the overview (live or terminal). */
  function hasTask(id) { return entries.has(id) }

  function subscribe(listener) {
    listeners.add(listener)
    return () => { listeners.delete(listener) }
  }

  function apply(handle, pluginId, task) {
    if (!task?.id) return
    const previous = entries.get(task.id)
    if (previous && sameTask(previous, task)) return
    entries.set(task.id, {
      id: task.id,
      pluginId,
      action: task.action || '',
      state: task.state || 'queued',
      phase: task.phase || '',
      cancelable: task.cancelable === true,
      error: task.error || '',
      uncertain: task.uncertain === true,
      result: task.result,
      order: previous?.order ?? ++order,
      updatedAt: now(),
    })
    publish()
  }

  /** Track one host-created handle. Returns an idempotent observer detach. */
  function track(pluginId, handle) {
    if (!handle?.id || typeof handle.subscribe !== 'function') return () => {}
    let detached = false
    const read = () => { if (!detached) apply(handle, pluginId, handle.getSnapshot()) }
    const off = handle.subscribe(read)
    read()
    Promise.resolve(handle.done).then(
      task => { read(); if (task) apply(handle, pluginId, task) },
      () => {},
    ).finally(() => { detached = true; off?.() })
    return () => { detached = true; off?.() }
  }

  function dismiss(id) {
    if (entries.delete(id)) publish()
  }

  function clearFinished() {
    let changed = false
    for (const [id, task] of entries) {
      if (TERMINAL.has(task.state)) { entries.delete(id); changed = true }
    }
    if (changed) publish()
  }

  return { getSnapshot, subscribe, hasTask, track, dismiss, clearFinished }
}

/** One process-wide overview shared by the host task API and the bottom panel. */
export const taskOverview = createTaskOverview()
