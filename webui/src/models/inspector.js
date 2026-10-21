import { createResourceModel, createSnapshotStore } from '../plugin-host/model.js'
import { footprintView } from '../lib/footprintStatus.js'

const idle = () => ({ status: 'idle', data: null, error: null })
const initial = () => ({
  selected: null, picker: null, query: '', busy: false, matchesOpen: false,
  binding: idle(), search: idle(), matches: idle(),
})

// Only the current selection owns resources. Query snapshots are referenced by
// the public snapshot; React does not keep a second set of query state/epochs.
export function createInspectorModel({ service, refresh = async () => {} }) {
  const store = createSnapshotStore(initial())
  const resources = createResourceModel({
    load: ({ method, args }, options) => service[method](...args, options),
  })
  const subscriptions = new Map()
  let session = 0
  let interaction = 0
  let submission = null
  let disposed = false

  function patch(value) { store.update(previous => ({ ...previous, ...value })) }
  function release(key) {
    subscriptions.get(key)?.()
    subscriptions.delete(key)
    resources.dispose(key)
  }
  function read(key, method, args) {
    if (!subscriptions.has(key)) {
      const resource = resources.get(key)
      subscriptions.set(key, resource.subscribe(() => patch({ [key]: resource.getSnapshot() })))
    }
    return resources.request(key, { method, args })
  }
  function clearResources() {
    for (const off of subscriptions.values()) off()
    subscriptions.clear()
    resources.disposeAll()
  }
  function select(selected) {
    if (disposed) return Promise.resolve({ status: 'superseded' })
    session++
    interaction++
    submission = null
    clearResources()
    store.set({ ...initial(), selected })
    return selected ? reloadBinding() : Promise.resolve({ status: 'published' })
  }
  function reloadBinding() {
    const selected = store.getSnapshot().selected
    return selected ? read('binding', 'getBinding', [selected.id]) : Promise.resolve({ status: 'superseded' })
  }
  function resetPicker() {
    release('search')
    patch({ picker: null, query: '', search: idle() })
  }
  function closePicker() { interaction++; resetPicker() }
  function openPicker(kind) {
    if (!store.getSnapshot().selected || disposed) return Promise.resolve({ status: 'superseded' })
    interaction++
    release('search')
    patch({ picker: kind, query: '', search: idle() })
    return search('')
  }
  function search(query) {
    const { selected, picker, binding } = store.getSnapshot()
    if (!selected || !picker || disposed) return Promise.resolve({ status: 'superseded' })
    patch({ query })
    const near = picker === 'footprint'
      ? binding.data?.footprint_name || selected.footprint || ''
      : binding.data?.model_name || ''
    return read('search', picker === 'footprint' ? 'searchFootprints' : 'searchModels', [query, near])
  }
  function closeMatches() {
    release('matches')
    patch({ matchesOpen: false, matches: idle() })
  }
  function findMatches() {
    const selected = store.getSnapshot().selected
    if (!selected || disposed) return Promise.resolve({ status: 'superseded' })
    patch({ matchesOpen: true })
    return read('matches', 'getMatches', [selected.id])
  }
  async function refreshCurrent() {
    const ownSession = session
    const [binding, refreshed] = await Promise.all([
      reloadBinding(),
      Promise.resolve().then(refresh).then(() => null, error => error),
    ])
    if (ownSession !== session || disposed) return { status: 'superseded' }
    const error = refreshed || (binding.status === 'failed' ? binding.error : null)
    return error ? { status: 'refresh-failed', error } : { status: 'published' }
  }
  async function write(kind, item, fromMatches = false) {
    const current = store.getSnapshot()
    if (!current.selected || disposed) return { status: 'superseded' }
    if (kind === 'footprint' && !fromMatches && !footprintView(item).available) return { status: 'unavailable' }
    if (submission) return { status: 'busy' }
    const request = { session, interaction }
    submission = request
    patch({ busy: true })
    const isCurrent = () => submission === request && session === request.session && interaction === request.interaction && !disposed
    try {
      let result
      if (kind === 'footprint') {
        result = await service.bindFootprint(current.selected.id, fromMatches ? item.footprint_id : item.id)
      } else {
        const binding = current.binding.data || await service.getBinding(current.selected.id)
        if (!isCurrent()) return { status: 'superseded' }
        if (!binding?.footprint_id) return { status: 'missing-footprint' }
        result = await service.bindModel(binding.footprint_id, item.id)
      }
      if (!isCurrent()) return { status: 'superseded' }
      if (!result?.ok) return { status: 'failed', error: new Error(result?.error || '') }
      if (fromMatches) closeMatches()
      else resetPicker()
      return await refreshCurrent()
    } catch (error) {
      return isCurrent() ? { status: 'failed', error } : { status: 'superseded' }
    } finally {
      if (submission === request) {
        submission = null
        patch({ busy: false })
      }
    }
  }
  function dispose() {
    if (disposed) return
    select(null)
    disposed = true
    store.dispose()
  }
  return {
    getSnapshot: store.getSnapshot, subscribe: store.subscribe,
    select, openPicker, closePicker, search, findMatches, closeMatches,
    selectCandidate: item => write(store.getSnapshot().picker, item),
    pickMatch: item => write('footprint', item, true),
    refresh: refreshCurrent, dispose,
  }
}
