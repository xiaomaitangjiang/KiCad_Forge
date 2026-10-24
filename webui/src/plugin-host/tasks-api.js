// Task identities are bound by the host. Imports select a provider through a
// separate client; they never change the calling plugin's tasks identity.
import { createSnapshotStore } from './model.js'
import { taskOverview } from './task-overview.js'

function encode(value) {
  return encodeURIComponent(String(value))
}

function normalizeTask(dto, mapResult = (value) => value) {
  return {
    id: dto?.task_id ?? dto?.id ?? '',
    action: dto?.action ?? '',
    state: dto?.state ?? 'queued',
    phase: dto?.phase,
    cancelable: dto?.cancelable === true,
    cancelRequested: (dto?.cancel_requested ?? dto?.cancelRequested) === true,
    result: dto?.result === undefined ? undefined : mapResult(dto.result),
    error: dto?.error ?? '',
    uncertain: dto?.uncertain === true,
    revision: dto?.revision,
  }
}

const TERMINAL = new Set(['succeeded', 'failed', 'cancelled'])

function createTaskHandle(initial, fetchTask, cancelTask, { mapResult, onRelease } = {}) {
  const store = createSnapshotStore(normalizeTask(initial, mapResult), { freeze: true })
  const id = store.getSnapshot().id
  const controller = new AbortController()
  let stopped = TERMINAL.has(store.getSnapshot().state)
  let timer = null
  let resolveDone
  const done = new Promise((resolve) => {
    resolveDone = resolve
  })
  if (stopped) resolveDone(store.getSnapshot())

  function publish(dto) {
    if (stopped) return
    const next = normalizeTask(dto, mapResult)
    const current = store.getSnapshot()
    if (JSON.stringify(current) !== JSON.stringify(next)) store.set(next)
    if (TERMINAL.has(next.state)) {
      stopped = true
      if (timer) clearTimeout(timer)
      timer = null
      resolveDone(next)
      onRelease?.()
    }
  }

  async function poll() {
    if (stopped) return
    try {
      const next = await fetchTask(id, controller.signal)
      if (next) publish({ ...next, task_id: next.task_id ?? id })
    } catch {
      // transient read errors keep the last snapshot; the host remains owner
    }
    if (!stopped) {
      timer = setTimeout(poll, 250)
      timer.unref?.()
    }
  }
  if (!stopped) {
    timer = setTimeout(poll, 0)
    timer.unref?.()
  }

  return {
    id,
    getSnapshot: store.getSnapshot,
    subscribe: store.subscribe,
    async cancel() {
      const reply = await cancelTask(id)
      if (reply?.task) publish({ ...reply.task, task_id: id })
      // The native cancel endpoint returns the complete task record directly.
      else if (reply?.task_id) publish(reply)
      return reply
    },
    /** Internal: terminal snapshot, or the last snapshot on frontend release. */
    done,
    dispose() {
      stopped = true
      if (timer) clearTimeout(timer)
      timer = null
      controller.abort()
      store.dispose()
      resolveDone(store.getSnapshot())
      onRelease?.()
    },
  }
}

function createRequest(fetchImpl) {
  return async function request(path, options = {}) {
    const response = await fetchImpl(path, {
      cache: 'no-store',
      ...options,
      headers: {
        'content-type': 'application/json',
        ...(options.headers ?? {}),
      },
    })
    let body = null
    try {
      body = await response.json()
    } catch {
      body = null
    }
    if (!response.ok) {
      const error = new Error(body?.error || `task request failed: HTTP ${response.status}`)
      error.status = response.status
      error.code = body?.error_code ?? body?.code
      error.body = body
      throw error
    }
    return body
  }
}

export function createTasksApi(pluginId, fetchImpl = globalThis.fetch) {
  const request = createRequest(fetchImpl)
  const handles = new Set()
  let disposed = false

  return {
    async run(action, args = {}, options = {}) {
      if (disposed) throw new Error('Task client is disposed')
      const created = await request('/api/tasks', {
        method: 'POST',
        body: JSON.stringify({
          plugin_id: pluginId,
          action,
          args,
          ...(options.clientRequestId ? { client_request_id: options.clientRequestId } : {}),
        }),
      })
      const handle = createTaskHandle(
        { ...created, action },
        (taskId, signal) => request(`/api/tasks/${encode(taskId)}`, { signal }),
        (taskId) => request(`/api/tasks/${encode(taskId)}/cancel`, { method: 'POST' }),
        { mapResult: options.mapResult, onRelease: () => handles.delete(handle) },
      )
      // The overview observes this handle; it does not own polling or cancellation.
      taskOverview.track(pluginId, handle)
      if (disposed) handle.dispose()
      else if (!TERMINAL.has(handle.getSnapshot().state)) handles.add(handle)
      return handle
    },
    async get(taskId) {
      try {
        return normalizeTask(await request(`/api/tasks/${encode(taskId)}`))
      } catch (error) {
        if (error.status === 404) return null
        throw error
      }
    },
    cancel(taskId) {
      return request(`/api/tasks/${encode(taskId)}/cancel`, { method: 'POST' })
    },
    async list() {
      const reply = await request(`/api/plugins/${encode(pluginId)}/tasks`)
      return (reply?.tasks ?? []).map((task) => normalizeTask(task))
    },
    operation(operationId) {
      // Internal compatibility helper; ImportsContract exposes the mapped DTO.
      return request(`/api/imports/operations/${encode(operationId)}`)
    },
    dispose() {
      if (disposed) return
      disposed = true
      for (const handle of handles) handle.dispose()
      handles.clear()
    },
  }
}

/**
 * Attach a read-only observer to a task the backend already owns, e.g. after
 * a frontend reload. It reuses the same poll/cancel adapter as a normal task
 * handle; callers must check overview.hasTask() first to avoid a second poller.
 */
export function createRecoveredTask(pluginId, dto, { fetchImpl = globalThis.fetch, overview = taskOverview } = {}) {
  if (!dto || (!dto.task_id && !dto.id)) return null
  const request = createRequest(fetchImpl)
  const handle = createTaskHandle(
    dto,
    (taskId, signal) => request(`/api/tasks/${encode(taskId)}`, { signal }),
    (taskId) => request(`/api/tasks/${encode(taskId)}/cancel`, { method: 'POST' }),
  )
  overview.track(pluginId, handle)
  return handle
}

function mapFields(dto, fields) {
  const result = {}
  for (const [publicName, wireName] of Object.entries(fields)) {
    const value = dto?.[wireName] ?? dto?.[publicName]
    if (value !== undefined) result[publicName] = value
  }
  return result
}

function normalizeImportResult(dto) {
  return mapFields(dto, {
    ok: 'ok', noop: 'noop', replayed: 'replayed', operationId: 'operation_id', externalId: 'lcsc_id',
    symbolId: 'symbol_id', symbolName: 'symbol_name', footprintId: 'footprint_id',
    footprintReference: 'footprint_reference', mode: 'mode', createdLibrary: 'created_library',
    warnings: 'warnings', error: 'error',
  })
}

function normalizeAnalysis(dto) {
  const analysis = mapFields(dto, {
    externalId: 'lcsc_id', symbolNames: 'symbol_names', footprintName: 'footprint_name',
    modelName: 'model_name', warning: 'warning',
  })
  if (Array.isArray(dto?.candidates)) {
    analysis.candidates = dto.candidates.map((candidate) => mapFields({
      ...candidate, footprint_id: candidate.footprint_id ?? candidate.id,
    }, {
      footprintId: 'footprint_id', name: 'name', library: 'library', score: 'score',
      padCount: 'pad_count', footprintStatus: 'footprint_status', reason: 'footprint_reason',
    }))
  }
  return analysis
}

export function createImportsApi(tasks, fetchImpl = globalThis.fetch, { resolveProvider } = {}) {
  const request = createRequest(fetchImpl)
  const clients = new Map()
  let disposed = false

  function provider(input) {
    if (disposed) throw new Error('Import client is disposed')
    if (typeof input?.provider !== 'string' || !input.provider.trim()) {
      throw new Error('Import provider must be a plugin ID')
    }
    const aliases = { lcsc: 'com.kicad_forge.lcsc_import', lcsc_import: 'com.kicad_forge.lcsc_import' }
    const id = aliases[input.provider.trim()] ?? input.provider.trim()
    const descriptor = resolveProvider?.(id) ?? {
      pluginId: id, analyzeAction: 'analyze', importAction: 'import',
    }
    if (!clients.has(descriptor.pluginId)) {
      clients.set(descriptor.pluginId, createTasksApi(descriptor.pluginId, fetchImpl))
    }
    return { ...descriptor, tasks: clients.get(descriptor.pluginId) }
  }

  return {
    async analyze(input) {
      const service = provider(input)
      const handle = await service.tasks.run(service.analyzeAction, { lcsc_id: input.externalId })
      const terminal = await handle.done
      if (terminal.state !== 'succeeded') {
        throw new Error(terminal.error || `analyze ${terminal.state}`)
      }
      return normalizeAnalysis(terminal.result)
    },
    commit(selection) {
      const service = provider(selection)
      return service.tasks.run(service.importAction, {
        lcsc_id: selection.externalId,
        target_component_library_id: selection.targetComponentLibraryId,
        symbol_library_choice: selection.symbolLibraryChoice,
        footprint_choice: selection.footprintChoice?.mode === 'reuse'
          ? { mode: 'reuse', footprint_id: selection.footprintChoice.footprintId }
          : { mode: selection.footprintChoice?.mode },
        ...(selection.operationId !== undefined ? { operation_id: selection.operationId } : {}),
      }, { mapResult: normalizeImportResult })
    },
    cancel(taskId) {
      return tasks.cancel(taskId)
    },
    async operation(operationId) {
      try {
        const reply = await request(`/api/imports/operations/${encode(operationId)}`)
        return normalizeImportResult(reply?.result ?? reply)
      } catch (error) {
        if (error.status === 404 && error.code === 'operation_not_found') return null
        throw error
      }
    },
    dispose() {
      if (disposed) return
      disposed = true
      for (const client of clients.values()) client.dispose()
      clients.clear()
    },
  }
}
