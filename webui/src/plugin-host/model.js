// React-free snapshot model used by the plugin host. Business components
// subscribe through the host's useSyncExternalStore adapter; nothing here
// imports React, so the model is testable in node.

import { createSnapshotStore } from '@kiforge/plugin-sdk'
export { createSnapshotStore } from '@kiforge/plugin-sdk'

/**
 * A keyed async resource model. Every request carries a per-key epoch and an
 * AbortController; only the newest epoch may publish, so a late response from
 * a previously selected symbol can never overwrite the current one.
 */
export function createResourceModel({ load, validate, retainErrorWhileLoading = false }) {
  const stores = new Map()
  const epochs = new Map()
  const active = new Map()

  function get(key) {
    let store = stores.get(key)
    if (!store) {
      store = createSnapshotStore({
        key,
        epoch: 0,
        status: 'idle',
        data: null,
        error: null,
      })
      stores.set(key, store)
    }
    return store
  }

  async function request(key, params, { signal } = {}) {
    const store = get(key)
    const epoch = (epochs.get(key) ?? 0) + 1
    epochs.set(key, epoch)
    active.get(key)?.abort()
    const controller = new AbortController()
    active.set(key, controller)
    const previousData = store.getSnapshot().data
    const onExternalAbort = () => controller.abort()
    const detachExternal = () => signal?.removeEventListener('abort', onExternalAbort)
    // A transport may ignore abort forever. Detach on local cancellation as
    // well as in finally, so destroying the model does not retain its signal.
    controller.signal.addEventListener('abort', detachExternal, { once: true })
    if (signal) {
      if (signal.aborted) controller.abort()
      else signal.addEventListener('abort', onExternalAbort, { once: true })
    }

    store.set({
      key,
      epoch,
      status: 'loading',
      data: previousData,
      error: retainErrorWhileLoading ? store.getSnapshot().error : null,
    })

    const abortOutcome = () => {
      // Only the current request may change this store; cancellation keeps the
      // last published data but must not leave a permanent loading state.
      if (store.disposed || epoch !== epochs.get(key)) return { status: 'superseded' }
      store.set({ key, epoch, status: 'idle', data: store.getSnapshot().data, error: null })
      return { status: 'aborted' }
    }

    try {
      const data = await load(params, { signal: controller.signal })
      if (store.disposed || epoch !== epochs.get(key)) return { status: 'superseded', data }
      if (controller.signal.aborted) return abortOutcome()
      validate?.(data, params)
      store.set({ key, epoch, status: 'ready', data, error: null })
      return { status: 'published', data }
    } catch (error) {
      if (store.disposed || epoch !== epochs.get(key)) return { status: 'superseded', error }
      if (controller.signal.aborted) return abortOutcome()
      store.set({ key, epoch, status: 'error', data: store.getSnapshot().data, error })
      return { status: 'failed', error }
    } finally {
      detachExternal()
      controller.signal.removeEventListener('abort', detachExternal)
      if (active.get(key) === controller) active.delete(key)
    }
  }

  function dispose(key) {
    active.get(key)?.abort()
    active.delete(key)
    stores.get(key)?.dispose()
    stores.delete(key)
    epochs.delete(key)
  }

  // Reusable lifecycle cleanup for StrictMode: invalidate work while retaining
  // this store and its last data. Terminal disposal remains a separate action.
  function invalidate(key) {
    const store = stores.get(key)
    if (!store || store.disposed) return
    epochs.set(key, (epochs.get(key) ?? 0) + 1)
    active.get(key)?.abort()
    active.delete(key)
    const previous = store.getSnapshot()
    store.set({ ...previous, epoch: epochs.get(key), status: 'idle' })
  }

  function disposeAll() {
    for (const controller of active.values()) controller.abort()
    for (const store of stores.values()) store.dispose()
    active.clear()
    stores.clear()
    epochs.clear()
  }

  return { get, request, invalidate, dispose, disposeAll, has: (key) => stores.has(key) }
}

/** Host-side selector hook adapter; the SDK only exposes its type. */
export function bindUseSnapshot(useSyncExternalStore) {
  return function useSnapshot(store) {
    return useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot)
  }
}
