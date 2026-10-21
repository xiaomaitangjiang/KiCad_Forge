// Plugin-scoped configuration contract. The remote store is the host's
// /api/plugin-config/<id> (backed by ConfigStore's plugins.<id>.config); this
// module owns defaults, a small JSON-Schema subset and subscription semantics.
import { createSnapshotStore } from './model.js'

const MAX_CONFIG_BYTES = 64 * 1024
const utf8 = new TextEncoder()

export function validateConfig(schema, value, path = '') {
  const errors = []
  if (!schema || typeof schema !== 'object') return errors
  const label = path || 'config'

  if (schema.type === 'object') {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      errors.push({ path: label, message: 'must be an object' })
      return errors
    }
    for (const key of schema.required ?? []) {
      if (!(key in value)) errors.push({ path: `${label}.${key}`, message: 'is required' })
    }
    for (const [key, child] of Object.entries(schema.properties ?? {})) {
      if (key in value) errors.push(...validateConfig(child, value[key], `${label}.${key}`))
    }
    return errors
  }

  if (schema.type === 'string' && typeof value !== 'string') {
    errors.push({ path: label, message: 'must be a string' })
  } else if (schema.type === 'number' && typeof value !== 'number') {
    errors.push({ path: label, message: 'must be a number' })
  } else if (schema.type === 'integer' && !Number.isInteger(value)) {
    errors.push({ path: label, message: 'must be an integer' })
  } else if (schema.type === 'boolean' && typeof value !== 'boolean') {
    errors.push({ path: label, message: 'must be a boolean' })
  }
  if (schema.enum && !schema.enum.some((candidate) => Object.is(candidate, value))) {
    errors.push({ path: label, message: `must be one of ${schema.enum.join(', ')}` })
  }
  return errors
}

export function configDefaults(schema) {
  if (!schema || schema.type !== 'object') return {}
  const result = {}
  for (const [key, child] of Object.entries(schema.properties ?? {})) {
    if (child && Object.prototype.hasOwnProperty.call(child, 'default')) {
      result[key] = child.default
    } else if (child?.type === 'object') {
      result[key] = configDefaults(child)
    }
  }
  return result
}

export function mergeConfig(schema, current, patch) {
  const next = { ...current }
  for (const [key, value] of Object.entries(patch ?? {})) {
    const child = schema?.properties?.[key]
    if (child?.type === 'object' && value && typeof value === 'object') {
      next[key] = mergeConfig(child, current?.[key] ?? {}, value)
    } else {
      next[key] = value
    }
  }
  return next
}

export function createConfigApi({ pluginId, schema, remote, initial = {} }) {
  let current = mergeConfig(schema, configDefaults(schema), initial)
  const store = createSnapshotStore(current, { freeze: true })
  let loaded = false
  let pending = Promise.resolve()

  // Merge patches when their turn starts, so concurrent writes cannot both
  // replace a newer snapshot using the same old configuration. Reads share the
  // queue; a failed operation rejects its caller without poisoning later work.
  function enqueue(operation) {
    const result = pending.then(operation)
    pending = result.catch(() => {})
    return result
  }

  function load() {
    return enqueue(async () => {
      if (remote?.get) {
        const stored = await remote.get(pluginId)
        if (stored && typeof stored === 'object') {
          current = mergeConfig(schema, configDefaults(schema), stored)
          store.set(current)
        }
      }
      loaded = true
      return current
    })
  }

  function set(patch) {
    return enqueue(async () => {
      if (!patch || typeof patch !== 'object' || Array.isArray(patch)) {
        throw new Error('config patch must be an object')
      }
      const candidate = mergeConfig(schema, current, patch)
      const errors = validateConfig(schema, candidate)
      if (errors.length) {
        const error = new Error(`Invalid plugin config: ${errors[0].path} ${errors[0].message}`)
        error.code = 'config/invalid'
        error.fields = errors
        throw error
      }
      const encoded = JSON.stringify(candidate)
      if (utf8.encode(encoded).byteLength > MAX_CONFIG_BYTES) {
        const error = new Error(`Plugin config exceeds ${MAX_CONFIG_BYTES} bytes`)
        error.code = 'config/too-large'
        throw error
      }
      if (remote?.set) await remote.set(pluginId, candidate)
      current = candidate
      store.set(current)
      return current
    })
  }

  return {
    get: () => current,
    set,
    load,
    subscribe: (listener) => store.subscribe(() => listener(store.getSnapshot())),
    getSnapshot: store.getSnapshot,
    get loaded() {
      return loaded
    },
  }
}
