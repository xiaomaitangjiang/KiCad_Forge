// Host-side read-only task overview. It observes the same TaskHandle that the
// plugin already polls; it never starts a second request loop and never cancels
// work when the panel closes. Terminal records are really evicted from the
// entries map, and only bounded identity/status text is retained: the plugin
// model keeps the full business result.
export const MAX_TASK_OVERVIEW = 20
export const MAX_TASK_OVERVIEW_BYTES = 16 * 1024
export const MAX_TASK_TEXT_CHARS = 512

const TERMINAL = new Set(['succeeded', 'failed', 'cancelled'])

function boundedText(value, max = MAX_TASK_TEXT_CHARS) {
  if (typeof value !== 'string' || value.length === 0) return ''
  if (value.length <= max) return value
  return value.slice(0, Math.max(0, max - 1)) + '…'
}

function summaryOf(result) {
  if (typeof result === 'string') return result
  if (result && typeof result.output === 'string') return result.output
  return ''
}

function sameTask(a, b) {
  return a.id === b.id && a.action === b.action && a.state === b.state && a.phase === b.phase
    && a.error === b.error && a.summary === b.summary
    && a.uncertain === b.uncertain && a.cancelable === b.cancelable
}

// The budget is defined in UTF-8 bytes of the retained payload. TextEncoder is
// available in the browser and Node; String.length (UTF-16 code units) would
// under-count Chinese/emoji text by up to 3x.
const encoder = new TextEncoder()

/** UTF-8 byte size of one retained record (no full result). */
export function estimateTaskBytes(task) {
  const serialized = JSON.stringify({
    id: task.id,
    pluginId: task.pluginId,
    action: task.action,
    state: task.state,
    phase: task.phase,
    cancelable: task.cancelable === true,
    uncertain: task.uncertain === true,
    error: task.error || '',
    summary: task.summary || '',
    updatedAt: task.updatedAt,
  })
  return encoder.encode(serialized).byteLength
}

function publicTask(entry) {
  return {
    id: entry.id,
    pluginId: entry.pluginId,
    action: entry.action,
    state: entry.state,
    phase: entry.phase,
    cancelable: entry.cancelable,
    uncertain: entry.uncertain,
    error: entry.error,
    summary: entry.summary,
    updatedAt: entry.updatedAt,
  }
}

export function createTaskOverview({ max = MAX_TASK_OVERVIEW, maxBytes = MAX_TASK_OVERVIEW_BYTES, now = () => Date.now() } = {}) {
  const terminalLimit = Math.max(0, Math.floor(Number(max) || 0))
  const byteLimit = Math.max(0, Math.floor(Number(maxBytes) || 0))
  const entries = new Map()
  const listeners = new Set()
  let sequence = 0
  let terminalSequence = 0
  let snapshot = Object.freeze({ tasks: Object.freeze([]) })

  function bytesOf(records) {
    let total = 0
    for (const entry of records) total += estimateTaskBytes(entry)
    return total
  }

  // Oldest terminal records are evicted first. Active work is never evicted:
  // the task service owns its lifetime, and eviction must not cancel it. The
  // byte budget covers terminal records only; active records are protected in
  // full and the terminal allowance shrinks when active records are retained.
  function trimTerminal() {
    const terminal = [...entries.values()]
      .filter(entry => TERMINAL.has(entry.state))
      .sort((a, b) => a.terminalOrder - b.terminalOrder)
    while (terminal.length > 0) {
      const active = [...entries.values()].filter(entry => !TERMINAL.has(entry.state))
      const terminalBudget = Math.max(0, byteLimit - bytesOf(active))
      if (terminal.length <= terminalLimit && bytesOf(terminal) <= terminalBudget) break
      entries.delete(terminal.shift().id)
    }
  }

  function publish() {
    const tasks = [...entries.values()]
      .sort((a, b) => b.sequence - a.sequence)
      .map(entry => Object.freeze(publicTask(entry)))
    snapshot = Object.freeze({ tasks: Object.freeze(tasks) })
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
    const state = task.state || 'queued'
    const next = {
      id: task.id,
      pluginId,
      action: task.action || '',
      state,
      phase: task.phase || '',
      cancelable: task.cancelable === true,
      error: boundedText(typeof task.error === 'string' ? task.error : task.error ? String(task.error) : ''),
      uncertain: task.uncertain === true,
      summary: boundedText(summaryOf(task.result)),
      updatedAt: now(),
    }
    if (previous && sameTask(previous, next)) return

    const terminal = TERMINAL.has(state)
    entries.set(task.id, {
      ...next,
      sequence: ++sequence,
      terminalOrder: terminal ? (previous?.terminalOrder ?? ++terminalSequence) : undefined,
    })
    trimTerminal()
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
