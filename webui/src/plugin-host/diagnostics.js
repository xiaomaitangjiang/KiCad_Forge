// Page-local diagnostics store. Host-side manifest errors arrive through the
// C++ /api/plugin-diagnostics route; frontend activation/slot errors land here.
import { createSnapshotStore } from './model.js'

export function createDiagnostics({ limit = 200 } = {}) {
  const records = []
  const store = createSnapshotStore(records, { freeze: false })

  function report(report) {
    const item = {
      code: String(report.code || 'plugin/error'),
      severity: report.severity || 'error',
      message: String(report.message || ''),
      detail: report.detail,
      at: new Date().toISOString(),
    }
    records.push(item)
    while (records.length > limit) records.shift()
    store.set([...records])
    return item
  }

  return {
    report,
    list: () => records.slice(),
    subscribe: store.subscribe,
    getSnapshot: store.getSnapshot,
    clear() {
      records.length = 0
      store.set([])
    },
  }
}
