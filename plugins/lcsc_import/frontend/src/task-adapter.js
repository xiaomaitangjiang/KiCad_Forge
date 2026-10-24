// Bridge between the migrated LCSC modal's legacy `api.executePlugin` calls and
// the host's per-plugin task service. Pure functions so the mapping is testable
// without a browser or a real LCSC download.
export async function waitTask(tasks, task) {
  let current = task
  while (!['succeeded', 'failed', 'cancelled'].includes(current?.state)) {
    await new Promise((resolve) => setTimeout(resolve, 10))
    const next = await tasks.get(current.id ?? current.task_id)
    if (!next) break
    current = next
  }
  return current
}

export function createTaskApi({ tasks, libraries, footprints }) {
  const pending = new Set()
  let generation = 0

  function waitHandle(handle, started) {
    const terminal = () => ['succeeded', 'failed', 'cancelled'].includes(handle.getSnapshot().state)
    if (started !== generation || terminal()) return Promise.resolve(handle.getSnapshot())
    return new Promise((resolve) => {
      let off = () => {}
      let settled = false
      const release = () => {
        if (settled) return
        settled = true
        off()
        pending.delete(release)
        resolve(handle.getSnapshot())
      }
      const update = () => { if (terminal()) release() }
      pending.add(release)
      off = handle.subscribe(update)
      // Also tolerate stores that notify synchronously when subscribing.
      if (settled) off()
      else update()
    })
  }

  return {
    dispose() {
      // Effects may be replayed by React; invalidate earlier work while allowing
      // the same memoized bridge to serve a subsequent mounted generation.
      generation += 1
      for (const release of pending) release()
    },
    async executePlugin(_pluginId, body, onTask) {
      const started = generation
      const { action, args, ...rest } = body ?? {}
      const taskArgs = args ?? rest
      try {
        const handle = await tasks.run(action, taskArgs)
        if (started !== generation) throw new Error('Plugin view is closed')
        onTask?.(handle)
        const final = await waitHandle(handle, started)
        if (!final || final.state !== 'succeeded') {
          return { ok: false, error: final?.error || `Task ${final?.state || 'failed'}` }
        }
        return { ok: true, plugin_result: final.result }
      } catch (error) {
        return { ok: false, error: error?.message || String(error) }
      }
    },
    async compLibraries() {
      const values = await libraries.componentLibraries()
      return values.map(({ symbolPath, footprintPath, isProtected, ...value }) => ({
        ...value,
        symbol_path: symbolPath,
        footprint_path: footprintPath,
        is_protected: isProtected,
      }))
    },
    async componentSymbolLibraries(id) {
      const value = await libraries.componentSymbolLibraries(id)
      if (value && typeof value === 'object' && Array.isArray(value.libraries)) {
        return {
          ok: true,
          libraries: value.libraries,
          can_create: value.canCreate,
          case_sensitive: value.caseSensitive,
        }
      }
      return value
    },
    async searchFootprints(query, near) {
      const values = await footprints.search(query, near)
      return values.map(({ footprintId, padCount, footprintStatus, reason, ...value }) => ({
        ...value,
        id: footprintId,
        pad_count: padCount,
        footprint_status: footprintStatus,
        footprint_reason: reason,
      }))
    },
  }
}
