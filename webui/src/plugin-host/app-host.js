// Main-window plugin runtime. The prototype page builds its own runtime; the
// real app shares this singleton so slots/views live outside React state.
import { createSlotHost } from './slot-host.js'
import { createPluginLoader } from './loader.js'
import { createHostServices } from './host-services.js'
import { createPluginStateApi } from './plugin-state-api.js'

const scope = { workspaceId: null, selectedSymbolId: null, libraryFilterId: null }
let refresh = {}

const services = createHostServices({
  getScope: () => scope,
  getRefresh: () => refresh,
})

const host = createSlotHost({
  viewSlot: 'app.view',
  auxSlot: 'app.aux',
  additionalSlots: [
    { name: 'sidebar.section', kind: 'list' },
    { name: 'sidebar.panel', kind: 'list' },
    { name: 'toolbar.action', kind: 'list' },
    { name: 'card.accessory', kind: 'list' },
    { name: 'inspector.tab', kind: 'keyed' },
    { name: 'contextmenu.item', kind: 'list' },
    { name: 'settings.section', kind: 'list' },
  ],
})
host.registerShell()

const loader = createPluginLoader({
  host,
  services,
  stateApi: createPluginStateApi(),
})

let started = null

export const appPluginRuntime = {
  host,
  loader,
  services,
  setScope(next) {
    Object.assign(scope, next)
    services.workspace.setScope(scope)
  },
  setRefresh(next) {
    refresh = { ...refresh, ...next }
  },
  start() {
    if (!started) started = loader.loadAll()
    return started
  },
  async dispose() {
    started = null
    await loader.dispose()
  },
}

// Read-only diagnostic handle for the acceptance harness; the app itself never
// depends on it.
globalThis.__KF_APP_RUNTIME__ = appPluginRuntime
