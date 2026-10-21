import { createResourceModel } from '../plugin-host/model.js'

const key = 'request'

// One reusable resource per hook instance. React only subscribes and supplies
// the dependency-bound request function; results, errors and epochs live here.
export function createRequestModel() {
  const resources = createResourceModel({
    load: ({ read, args }) => read(...args),
    validate: (data, { validate }) => {
      if (validate && !validate(data)) throw new Error('读取结果格式无效')
    },
    retainErrorWhileLoading: true,
  })
  const store = resources.get(key)
  let disposed = false
  let active = true

  function loadTracked(read, args = [], validate) {
    if (disposed || !active) return Promise.resolve({ status: 'superseded' })
    return resources.request(key, { read, args, validate })
  }
  async function load(...request) {
    const outcome = await loadTracked(...request)
    return Object.hasOwn(outcome, 'data') ? outcome.data : null
  }
  function setField(field, value) {
    store.update(previous => {
      const next = typeof value === 'function' ? value(previous[field]) : value
      return Object.is(next, previous[field]) ? previous : { ...previous, [field]: next }
    })
  }

  return {
    getSnapshot: store.getSnapshot,
    subscribe: store.subscribe,
    loadTracked,
    load,
    setData: (value) => setField('data', value),
    setError: (value) => setField('error', value),
    activate: () => { if (!disposed) active = true },
    invalidate: () => { active = false; if (!disposed) resources.invalidate(key) },
    dispose() { disposed = true; resources.disposeAll() },
  }
}
