// Host-side query contracts and refresh commands. Task/import contracts have
// separate clients because they carry plugin-bound identities and lifecycles.
import { api as hostApi } from '../api.js'
import { createSnapshotStore } from './model.js'

// Fixed, shallow HTTP-to-SDK mappings. Raw API consumers keep their existing DTOs.
const SYMBOL_FIELDS = {
  id: 'id', name: 'name', library_id: 'libraryId', library_name: 'libraryName',
  type_id: 'typeId', type: 'typeName', mpn: 'mpn', value: 'value', footprint: 'footprint',
  pins: 'pins', locked: 'locked',
}
const SYMBOL_LIBRARY_FIELDS = {
  id: 'id', name: 'name', file_path: 'path', path: 'path', symbol_count: 'symbolCount', locked: 'locked',
}
const COMPONENT_LIBRARY_FIELDS = {
  id: 'id', name: 'name', symbol_path: 'symbolPath', footprint_path: 'footprintPath',
  enabled: 'enabled', locked: 'locked', is_protected: 'isProtected',
}
const BINDING_FIELDS = {
  footprint_id: 'footprintId', footprint_name: 'footprintName', footprint_status: 'footprintStatus',
  model_id: 'modelId', model_name: 'modelName',
}
const MATCH_FIELDS = {
  footprint_id: 'footprintId', footprint: 'name', library: 'library', score: 'score',
  footprint_status: 'footprintStatus', footprint_reason: 'reason',
}
const FOOTPRINT_FIELDS = {
  id: 'footprintId', name: 'name', library: 'library', score: 'score', pad_count: 'padCount',
  footprint_status: 'footprintStatus', footprint_reason: 'reason',
}
const COMMAND_REFRESHES = {
  refreshSymbols: ['symbols'],
  refreshLibraries: ['libraries'],
  refreshStatus: ['status'],
  refreshAll: ['symbols', 'libraries', 'status'],
}

function assertSuccess(value) {
  if (value?.ok === false) {
    const error = new Error(value.error || 'Host operation failed')
    error.kind = 'business'
    error.code = value.error_code ?? value.code
    error.payload = value
    throw error
  }
  return value
}

function invalidResponse() {
  const error = new TypeError('Invalid host API response')
  error.kind = 'parse'
  return error
}

function record(value) {
  assertSuccess(value)
  if (value === null || typeof value !== 'object' || Array.isArray(value)) throw invalidResponse()
  return value
}

function mapDto(value, fields) {
  record(value)
  const dto = {}
  for (const source in fields) {
    if (value[source] !== undefined) dto[fields[source]] = value[source]
  }
  return dto
}

function mapRows(value, fields) {
  assertSuccess(value)
  if (!Array.isArray(value)) throw invalidResponse()
  if (fields) return value.map((row) => mapDto(row, fields))
  value.forEach(record)
  return value
}

function assertWriteSuccess(value) {
  if (record(value).ok !== true) throw invalidResponse()
}

async function readOptional(read, map) {
  try {
    return map(await read())
  } catch (error) {
    if (error?.kind === 'http' && error.status === 404) return null
    throw error
  }
}

export function createHostServices({ getRefresh = () => ({}), api = hostApi } = {}) {
  const workspaceStore = createSnapshotStore(
    { workspaceId: null, selectedSymbolId: null, libraryFilterId: null },
    { freeze: true },
  )

  function setScope(scope) {
    const current = workspaceStore.getSnapshot()
    workspaceStore.set({
      workspaceId: scope?.workspaceId ?? null,
      selectedSymbolId: scope?.selectedSymbolId ?? null,
      libraryFilterId: scope?.libraryFilterId ?? null,
    })
    return current
  }

  const symbols = {
    async list(query = {}) {
      return mapRows(await api.symbols(query.q ?? '', query.libraryId ?? ''), SYMBOL_FIELDS)
    },
    get: (id) => readOptional(() => api.symbol(id), (value) => mapDto(value, SYMBOL_FIELDS)),
    binding: (symbolId) => readOptional(() => api.getBinding(symbolId), (value) => ({
      symbolId, ...mapDto(value, BINDING_FIELDS),
    })),
    async bindFootprint(symbolId, footprintId) {
      assertWriteSuccess(await api.bindFootprint(symbolId, footprintId))
    },
    async bindModel(footprintId, modelId) {
      assertWriteSuccess(await api.bindModel(footprintId, modelId))
    },
    async matches(symbolId) {
      return mapRows(await api.symbolMatches(symbolId), MATCH_FIELDS)
    },
  }

  const libraries = {
    async symbolLibraries() {
      return mapRows(await api.libraries(), SYMBOL_LIBRARY_FIELDS)
    },
    async componentLibraries() {
      return mapRows(await api.compLibraries(), COMPONENT_LIBRARY_FIELDS)
    },
    async componentSymbolLibraries(componentLibraryId) {
      const value = record(await api.componentSymbolLibraries(componentLibraryId))
      if (typeof value.can_create !== 'boolean' ||
          (value.case_sensitive !== undefined && typeof value.case_sensitive !== 'boolean')) {
        throw invalidResponse()
      }
      return {
        libraries: mapRows(value.libraries, SYMBOL_LIBRARY_FIELDS),
        canCreate: value.can_create === true,
        ...(value.case_sensitive === undefined ? {} : { caseSensitive: value.case_sensitive }),
      }
    },
  }

  const footprints = {
    async search(q, near) {
      return mapRows(await api.searchFootprints(q, near), FOOTPRINT_FIELDS)
    },
    async searchModels(q, near) {
      return mapRows(await api.searchModels(q, near))
    },
  }

  const commands = {
    async run(command) {
      if (!Object.hasOwn(COMMAND_REFRESHES, command)) throw new Error(`Unknown command: ${String(command)}`)
      const sources = COMMAND_REFRESHES[command]
      const refresh = getRefresh() ?? {}
      for (const source of sources) {
        if (typeof refresh[source] !== 'function') throw new Error(`Refresh ${source} is not available`)
      }
      for (const source of sources) await refresh[source]()
    },
  }

  return {
    workspace: {
      getSnapshot: workspaceStore.getSnapshot,
      subscribe: workspaceStore.subscribe,
      setScope,
    },
    symbols,
    libraries,
    footprints,
    commands,
  }
}
