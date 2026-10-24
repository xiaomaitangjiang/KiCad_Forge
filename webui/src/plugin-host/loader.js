// Cordis-backed frontend plugin loader for the phase-1 prototype.
//
// A plugin bundle's default export is a Cordis plugin ({ inject, apply }).
// The loader decorates the Cordis context with the SDK's slot/view faces and
// binds every contribution to the plugin's own fiber.

import { Context } from '@deepseek-ai/cordis'
import { useSyncExternalStore } from 'react'
import { SDK_VERSION, createSnapshotStore } from '@kiforge/plugin-sdk'
import { createContractApi, createDiagnosticsApi, createSlotApi, createViewApi } from './sdk-api.js'
import { createContractRegistry } from './contracts.js'
import { createConfigApi } from './config.js'
import { createTasksApi, createImportsApi } from './tasks-api.js'
import {
  checkManifestCompatibility,
  inspectContractRequirements,
  orderPluginsByContracts,
} from './manifest-check.js'

function moduleDefault(module) {
  return module?.default ?? module?.plugin
}

export function createPluginLoader({ host, services = {}, stateApi = null, fetchImpl = globalThis.fetch }) {
  const root = new Context()
  let reconcileScheduled = false
  let reconciling = false
  let reconcileAgain = false
  let disposed = false
  const contracts = createContractRegistry({
    report: (item) => {
      host.diagnostics.push(item)
    },
    onChange: () => scheduleReconcile(),
  })

  // Host-owned contracts are available to manifest consumers independently of
  // any plugin provider. Per-plugin tasks/imports are injected directly.
  for (const name of ['symbols', 'libraries', 'footprints', 'commands', 'workspace']) {
    if (services[name]) {
      contracts.provide({ id: 'host' }, `kiforge.${name}`, '1.0.0', services[name])
    }
  }
  const plugins = []
  const availableProviders = new Map()
  const runtimeChecks = {
    reactSame: null,
    reactVersion: null,
    pluginReactVersion: null,
  }
  const listeners = new Set()
  let cachedSnapshot = []

  function project() {
    return plugins.map((plugin) => ({
      id: plugin.id,
      name: plugin.name,
      version: plugin.version,
      enabled: plugin.enabled,
      status: plugin.status,
      error: plugin.error,
    }))
  }

  function notify() {
    cachedSnapshot = project()
    for (const listener of listeners) listener()
  }

  function find(id) {
    return availableProviders.get(id)
  }

  function scheduleReconcile() {
    if (disposed || reconcileScheduled) return
    reconcileScheduled = true
    queueMicrotask(() => {
      reconcileScheduled = false
      void reconcileContracts()
    })
  }

  async function disposeRecord(record) {
    const epoch = invalidateActivation(record)
    const fiber = record.fiber
    record.fiber = null
    record.cordisCtx = null
    record.taskApis?.imports.dispose()
    record.taskApis?.tasks.dispose()
    record.taskApis = null
    const listeners = record.disposeListeners
    record.disposeListeners = null
    if (listeners) {
      for (const listener of [...listeners]) {
        try {
          listener()
        } catch {
          // one failing cleanup listener must not block the rest
        }
      }
      listeners.clear()
    }
    // Remove this generation's styles before yielding to asynchronous Cordis
    // cleanup, which may overlap a later enable request.
    removeStyles(record)
    if (fiber) {
      let disposal
      try {
        disposal = Promise.resolve(fiber.dispose())
        record.disposing = disposal
        await disposal
      } catch {
        // disposal failures are diagnostics; the record is already detached
      } finally {
        if (record.disposing === disposal) record.disposing = null
      }
    }
    return epoch
  }

  async function reconcileContracts() {
    if (disposed) return
    if (reconciling) {
      reconcileAgain = true
      return
    }
    reconciling = true
    try {
      let changed = true
      while (changed && !disposed) {
        changed = false
        for (const record of plugins) {
          if (disposed || !record.enabled || record.requestedEnabled === false) continue
          const inspection = inspectContractRequirements({
            descriptor: record.descriptor,
            contracts,
            identity: { id: record.id, version: record.version },
          })
          if ((inspection.incompatible.length || inspection.missing.length) && record.fiber) {
            const epoch = await disposeRecord(record)
            changed = true
            if (disposed || !record.enabled || record.requestedEnabled === false || record.activationEpoch !== epoch) continue
          }
          if (inspection.incompatible.length) {
            record.status = 'error'
            record.error = inspection.incompatible
              .map((item) => `${item.contract}@${item.range} needs ${item.provider.version}`)
              .join('; ')
            notify()
            continue
          }
          if (inspection.missing.some((item) => item.perPluginOnly)) {
            const item = inspection.missing.find((entry) => entry.perPluginOnly)
            record.status = 'error'
            record.error = `${item.contract} is per-plugin and cannot be declared in frontend.requires`
            host.diagnostics.push({
              code: 'contract/per-plugin-only',
              severity: 'error',
              message: `${record.id}: ${record.error}`,
              detail: { plugin: record.id, contract: item.contract },
            })
            notify()
            continue
          }
          if (inspection.missing.length) {
            if (record.status !== 'waiting') {
              record.status = 'waiting'
              record.error = null
              notify()
            }
            continue
          }
          if (!record.fiber && (record.status === 'waiting' || record.status === 'idle')) {
            await activate(record)
            changed = true
          }
        }
      }
    } finally {
      reconciling = false
      if (reconcileAgain) {
        reconcileAgain = false
        scheduleReconcile()
      }
    }
  }

  function configRemote(pluginId) {
    const url = `/api/plugin-config/${encodeURIComponent(pluginId)}`
    return {
      async get() {
        const response = await fetchImpl(url, { cache: 'no-store' })
        if (!response.ok) return null
        const body = await response.json()
        return body?.config ?? body ?? null
      },
      async set(_pluginId, value) {
        const response = await fetchImpl(url, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(value),
        })
        if (!response.ok) {
          let detail = ''
          try {
            detail = (await response.json())?.error ?? ''
          } catch {
            // ignore non-JSON bodies
          }
          throw new Error(detail || `config write failed: HTTP ${response.status}`)
        }
      },
    }
  }

  async function importPlugin(descriptor) {
    const frontend = descriptor.frontend
    if (!frontend?.entry_url) throw new Error('plugin has no frontend entry')
    const url = new URL(frontend.entry_url, globalThis.location.origin)
    url.searchParams.set('v', frontend.version || descriptor.version || '0')
    return import(/* @vite-ignore */ url.href)
  }

  function resolveImportProvider(id) {
    const record = find(id)
    if (!record) throw new Error(`Unknown import provider: ${id}`)
    const descriptor = record.descriptor
    if (record.enabled === false) {
      throw new Error(`Import provider is disabled: ${id}`)
    }
    const actions = descriptor.actions ?? []
    const staged = actions.filter((action) => action.protocol === 'staged-import-v1')
    if (staged.length !== 1) {
      throw new Error(`Import provider ${id} must declare exactly one staged-import-v1 action`)
    }
    const action = staged[0]
    if (!action.analyze_action || !actions.some((candidate) => candidate.id === action.analyze_action)) {
      throw new Error(`Import provider ${id} must declare its analyze_action`)
    }
    return { pluginId: id, importAction: action.id, analyzeAction: action.analyze_action }
  }

  async function activate(record) {
    if (disposed || !record.enabled || record.requestedEnabled === false || record.fiber || record.activating) return
    const epoch = (record.activationEpoch ?? 0) + 1
    record.activationEpoch = epoch
    record.activating = true
    const isCurrent = () => !disposed && record.enabled && record.requestedEnabled !== false && record.activationEpoch === epoch
    record.status = 'loading'
    record.error = null
    notify()
    try {
      if (record.disposing) {
        await record.disposing.catch(() => {})
        if (!isCurrent()) return
      }
      const module = await importPlugin(record.descriptor)
      if (!isCurrent()) return
      const definition = moduleDefault(module)
      if (!definition || (typeof definition !== 'function' && typeof definition.apply !== 'function')) {
        throw new Error('plugin bundle default export is not a Cordis plugin')
      }
      const identity = { id: record.id, version: record.version }
      const compatibility = checkManifestCompatibility({
        descriptor: record.descriptor,
        sdkVersion: SDK_VERSION,
        identity,
      })
      if (!compatibility.ok) {
        host.diagnostics.push({
          code: compatibility.code,
          severity: 'error',
          message: compatibility.message,
          detail: compatibility.detail,
        })
        throw new Error(compatibility.message)
      }
      const inspection = inspectContractRequirements({
        descriptor: record.descriptor,
        contracts,
        identity,
      })
      if (inspection.incompatible.length) {
        const item = inspection.incompatible[0]
        const message = `${record.id} requires ${item.contract}@${item.range}, but ${item.provider.pluginId} provides ${item.provider.version}`
        host.diagnostics.push({
          code: 'contract/version-incompatible',
          severity: 'error',
          message,
          detail: {
            plugin: record.id,
            contract: item.contract,
            range: item.range,
            provider: item.provider.pluginId,
            version: item.provider.version,
          },
        })
        throw new Error(message)
      }
      const perPluginOnly = inspection.missing.find((item) => item.perPluginOnly)
      if (perPluginOnly) {
        const message = `${perPluginOnly.contract} is per-plugin and cannot be declared in frontend.requires`
        host.diagnostics.push({
          code: 'contract/per-plugin-only',
          severity: 'error',
          message: `${record.id}: ${message}`,
          detail: { plugin: record.id, contract: perPluginOnly.contract },
        })
        throw new Error(message)
      }
      if (inspection.missing.length) {
        record.status = 'waiting'
        record.error = null
        record.activating = false
        notify()
        return
      }

      const inject = definition.inject
      const configApi =
        record.configApi ??
        (record.configApi = createConfigApi({
          pluginId: record.id,
          schema: record.descriptor.config?.schema ?? record.descriptor.config ?? {},
          remote: configRemote(record.id),
        }))
      await configApi.load().catch(() => undefined)
      if (!isCurrent()) return
      insertStyles(record)
      const wrapped = {
        name: definition.name || record.id,
        inject,
        apply(cordisCtx) {
          if (!isCurrent()) return
          const slots = createSlotApi(host, cordisCtx, identity)
          const views = createViewApi(host, cordisCtx, identity)
          const contractApi = createContractApi(contracts, identity, cordisCtx)
          const diagnostics = createDiagnosticsApi(host, identity)
          const tasks = createTasksApi(identity.id, fetchImpl)
          const imports = createImportsApi(tasks, fetchImpl, { resolveProvider: resolveImportProvider })
          const apis = { tasks, imports }
          record.taskApis = apis
          cordisCtx.effect(() => () => {
            imports.dispose()
            tasks.dispose()
            if (record.taskApis === apis) record.taskApis = null
          })
          const model = {
            createStore(initial) {
              const store = createSnapshotStore(initial)
              cordisCtx.effect(() => () => store.dispose())
              return store
            },
            useSnapshot: (store) =>
              useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot),
          }
          const decorated = cordisCtx.extend({
            plugin: identity,
            slots,
            views,
            contracts: contractApi,
            config: configApi,
            diagnostics,
            tasks,
            imports,
            model,
          })
          // Cordis 4 events are global; intercept `dispose` so a plugin's
          // cleanup listener is owned by its own record, not every plugin.
          const disposeListeners = new Set()
          const parentOn = decorated.on.bind(decorated)
          Object.defineProperty(decorated, 'on', {
            configurable: true,
            value(name, listener, options) {
              if (name === 'dispose') {
                disposeListeners.add(listener)
                return () => disposeListeners.delete(listener)
              }
              return parentOn(name, listener, options)
            },
          })
          record.disposeListeners = disposeListeners
          // Host services are plain values, not Cordis services: define them as
          // own properties so plugin components can capture/use them after apply.
          for (const [name, value] of Object.entries(services)) {
            Object.defineProperty(decorated, name, {
              value,
              configurable: true,
              enumerable: true,
            })
          }
          // Dependency-driven restarts create a new fiber; keep the record
          // pointing at the live one so a later disable disposes the right one.
          record.fiber = cordisCtx.fiber
          record.cordisCtx = cordisCtx
          record.status = 'active'
          notify()
          cordisCtx.on('dispose', () => {
            if (record.fiber !== cordisCtx.fiber || record.activationEpoch !== epoch || disposed) return
            record.fiber = null
            if (record.enabled) record.status = 'waiting'
            else record.status = 'disabled'
            notify()
          })
          if (module.__reactUseState) {
            runtimeChecks.pluginReactVersion = module.__reactVersion
            runtimeChecks.reactSame =
              module.__reactUseState === globalThis.__KF_RUNTIME__?.React.useState
          }
          return definition.apply(decorated)
        },
      }
      const fiber = root.plugin(wrapped)
      if (isCurrent() && !record.fiber) record.fiber = fiber
      await fiber
      if (!isCurrent()) return
      if (record.fiber === fiber) record.status = 'active'
      notify()
    } catch (error) {
      if (!isCurrent()) return
      record.status = 'error'
      record.error = error?.message || String(error)
      record.fiber = null
      record.cordisCtx = null
      removeStyles(record)
      notify()
    } finally {
      if (record.activationEpoch === epoch) record.activating = false
    }
  }

  function invalidateActivation(record) {
    record.activationEpoch = (record.activationEpoch ?? 0) + 1
    record.activating = false
    return record.activationEpoch
  }

  async function loadHostDiagnostics() {
    try {
      const response = await fetchImpl('/api/plugin-diagnostics', { cache: 'no-store' })
      if (!response.ok) return
      const items = await response.json()
      for (const item of Array.isArray(items) ? items : []) {
        host.diagnostics.push({
          code: item.code || 'manifest/error',
          severity: item.severity || 'error',
          message: item.message || '',
          detail: item.detail ?? { pluginId: item.plugin_id },
        })
      }
    } catch {
      // Diagnostics are best-effort; a missing route must not block loading.
    }
  }

  async function loadAll({ filter = null } = {}) {
    if (disposed) return cachedSnapshot
    await loadHostDiagnostics()
    if (disposed) return cachedSnapshot
    const response = await fetchImpl('/api/plugins', { cache: 'no-store' })
    if (!response.ok) throw new Error(`GET /api/plugins failed: HTTP ${response.status}`)
    const manifests = await response.json()
    if (disposed) return cachedSnapshot
    availableProviders.clear()
    plugins.length = 0
    for (const manifest of Array.isArray(manifests) ? manifests : []) {
      if (!manifest?.id) continue
      // The manifest is the single source of truth for a plugin icon. The slot
      // host resolves it for contributions that do not pass a React icon, so
      // built-in and third-party plugins use the same mechanism.
      host.setPluginIcon?.(manifest.id, manifest.icon_url ?? null)
      const record = {
        id: manifest.id,
        name: manifest.name,
        version: manifest.version,
        descriptor: manifest,
        enabled: manifest.enabled !== false,
        revision: Number.isInteger(manifest.revision) ? manifest.revision : 0,
        status: 'idle',
        error: null,
        fiber: null,
      }
      // Python-only providers participate in the same persisted state queue;
      // only frontend records participate in Cordis activation/snapshots.
      availableProviders.set(manifest.id, record)
      if (!manifest.frontend?.entry_url || (filter && !filter(manifest))) continue
      plugins.push(record)
    }
    notify()
    // Sequential so a page plugin declares its child slots before an extender
    // registers into them, and contract providers run before their consumers.
    const ordered = orderPluginsByContracts(plugins, (item) => host.diagnostics.push(item))
    for (const record of ordered) {
      if (disposed) break
      if (!record.enabled) {
        record.status = 'disabled'
        notify()
        continue
      }
      await activate(record)
    }
    await reconcileContracts()
    return cachedSnapshot
  }

  async function setEnabled(id, enabled) {
    const record = find(id)
    if (disposed || !record) return
    const request = (record.stateEpoch ?? 0) + 1
    record.stateEpoch = request
    record.requestedEnabled = enabled
    if (!enabled && !record.fiber) invalidateActivation(record)
    const isCurrent = () => !disposed && record.stateEpoch === request
    let remote = null
    if (stateApi) {
      try {
        // Persist in invocation order: a later toggle must use the preceding
        // reply's revision even when that reply no longer owns the UI state.
        const write = (record.statePending ?? Promise.resolve()).then(async () => {
          const reply = await stateApi.set(id, enabled, record.revision)
          if (Number.isInteger(reply?.revision)) record.revision = reply.revision
          return reply
        })
        record.statePending = write.catch(() => {})
        remote = await write
        if (!isCurrent()) return remote
        if (typeof remote?.enabled === 'boolean' && remote.enabled !== enabled) {
          enabled = remote.enabled
        }
      } catch (error) {
        if (!isCurrent()) return null
        host.diagnostics.push({
          code: 'plugin/state-failed',
          severity: 'error',
          message: `Cannot ${enabled ? 'enable' : 'disable'} ${id}: ${error?.message || String(error)}`,
          detail: { plugin: id, enabled },
        })
        // 404 means the prototype/mock host has no state API; keep local toggle.
        if (error?.status !== 404) {
          record.requestedEnabled = record.enabled
          // A rejected disable may have invalidated an activation in flight;
          // resume it using a fresh generation without replacing a live fiber.
          if (record.descriptor.frontend?.entry_url && record.enabled && !record.fiber) await activate(record)
          throw error
        }
      }
    }
    if (!isCurrent()) return remote
    record.enabled = enabled
    record.requestedEnabled = enabled
    if (!record.descriptor.frontend?.entry_url) {
      record.status = enabled ? 'idle' : 'disabled'
      notify()
      return remote
    }
    if (enabled) {
      await activate(record)
    } else if (record.fiber) {
      await disposeRecord(record)
      if (!isCurrent()) return remote
      record.status = 'disabled'
      notify()
    } else {
      record.status = 'disabled'
      notify()
    }
    return remote
  }

  async function dispose() {
    disposed = true
    for (const record of [...plugins].reverse()) {
      await disposeRecord(record)
    }
    plugins.length = 0
    notify()
  }

  function insertStyles(record) {
    removeStyles(record)
    record.styleNodes = []
    for (const href of record.descriptor.frontend?.styles ?? []) {
      const link = document.createElement('link')
      link.rel = 'stylesheet'
      link.href = href
      link.dataset.kforgePlugin = record.id
      document.head.append(link)
      record.styleNodes.push(link)
    }
  }

  function removeStyles(record) {
    for (const node of record.styleNodes ?? []) node.remove()
    record.styleNodes = []
  }

  return {
    loadAll,
    setEnabled,
    dispose,
    subscribe(listener) {
      listeners.add(listener)
      return () => listeners.delete(listener)
    },
    getSnapshot: () => cachedSnapshot,
    runtimeChecks,
    contracts,
    root,
  }
}
