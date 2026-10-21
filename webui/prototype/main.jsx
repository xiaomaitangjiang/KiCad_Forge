import React, { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react'
import { createRoot } from 'react-dom/client'
import * as ReactDOM from 'react-dom'
import * as SDK from '@kiforge/plugin-sdk'
import { SlotCore } from '@deepseek-ai/dsh-client-ui-slots'
import { createSlotHost, SlotOutlet } from '../src/plugin-host/slot-host.js'
import { createPluginLoader } from '../src/plugin-host/loader.js'
import { bindUseSnapshot, createResourceModel } from '../src/plugin-host/model.js'
import { PluginTabs } from '../src/components/PluginSlot.jsx'
import './style.css'

// The identity probes are deliberately installed before any plugin module is
// imported: the QA script compares the plugin's React import with this object.
window.__KF_RUNTIME__ = {
  React,
  ReactDOM,
  SDK,
  SlotCore,
  version: React.version,
}

const host = createSlotHost({
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
const loader = createPluginLoader({ host })

function RapidSwitchProbe() {
  const [symbolId, setSymbolId] = useState('A')
  const model = useMemo(
    () =>
      createResourceModel({
        load: async ({ id, delay }) => {
          await new Promise((resolve) => setTimeout(resolve, delay))
          return id
        },
      }),
    [],
  )
  const useSnapshot = useMemo(() => bindUseSnapshot(useSyncExternalStore), [])
  useEffect(() => {
    model.request(symbolId, { id: symbolId, delay: symbolId === 'A' ? 400 : 25 })
    return () => model.dispose(symbolId)
  }, [model, symbolId])
  const snapshot = useSnapshot(model.get(symbolId))
  return (
    <div data-testid="rapid-probe">
      <button data-testid="rapid-switch" onClick={() => setSymbolId((value) => (value === 'A' ? 'B' : 'A'))}>
        切换符号
      </button>
      <span data-testid="rapid-result">{symbolId}:{snapshot.status}:{snapshot.data ?? ''}</span>
    </div>
  )
}
loader.runtimeChecks.reactVersion = React.version

function useSlotVersion(key) {
  const subscribe = useCallback((callback) => host.subscribe(key, callback), [key])
  const getSnapshot = useCallback(() => host.getVersion(key), [key])
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot)
}

function PluginRow({ plugin, onToggle, busy }) {
  const active = plugin.enabled && plugin.status === 'active'
  return (
    <div className="plugin-row">
      <div>
        <strong>{plugin.name}</strong>
        <span className="muted"> · {plugin.id} · {plugin.status}</span>
        {plugin.error ? <div className="error">{plugin.error}</div> : null}
      </div>
      <button disabled={busy} onClick={() => onToggle(plugin.id, !plugin.enabled)}>
        {plugin.enabled ? '停用' : '启用'}
      </button>
      <span className={active ? 'dot ok' : 'dot'} />
    </div>
  )
}

function App() {
  const plugins = useSyncExternalStore(loader.subscribe, loader.getSnapshot, loader.getSnapshot)
  const pageVersion = useSlotVersion('demo.prototype.page')
  const auxVersion = useSlotVersion('demo.prototype.aux')
  const tabsVersion = useSlotVersion('demo.supply.tabs')
  const [busy, setBusy] = React.useState('')

  const counts = useMemo(
    () => ({
      root: host.count('root'),
      page: host.count('demo.prototype.page'),
      aux: host.count('demo.prototype.aux'),
      tabs: host.count('demo.supply.tabs'),
    }),
    [pageVersion, auxVersion, tabsVersion],
  )

  async function toggle(id, enabled) {
    setBusy(id)
    try {
      await loader.setEnabled(id, enabled)
    } finally {
      setBusy('')
    }
  }

  return (
    <div className="page">
      <header>
        <h1>KiCad Forge · 插件原型</h1>
        <div className="muted">
          React {React.version} · root {counts.root} · page {counts.page} · aux {counts.aux} · tabs {counts.tabs}
        </div>
      </header>
      <aside>
        <h2>插件</h2>
        {plugins.map((plugin) => (
          <PluginRow key={plugin.id} plugin={plugin} busy={busy === plugin.id} onToggle={toggle} />
        ))}
        <h2>诊断</h2>
        <pre className="diagnostics">
          {host.diagnostics.length
            ? host.diagnostics.map((item) => `${item.code}: ${item.message}`).join('\n')
            : '无'}
        </pre>
      </aside>
      <main>
        <section className="kf-card" data-testid="slot-demo">
          <h3>六插槽演示</h3>
          <div data-testid="slot-sidebar">
            <SlotOutlet host={host} slot="sidebar.section" ownerProps={{ workspaceId: 'ws-1' }} />
          </div>
          <div data-testid="slot-toolbar">
            <SlotOutlet host={host} slot="toolbar.action" ownerProps={{ workspaceId: 'ws-1' }} />
          </div>
          <div data-testid="slot-card">
            <SlotOutlet host={host} slot="card.accessory"
              ownerProps={{ workspaceId: 'ws-1', symbolId: 'A', selected: true }} />
          </div>
          <PluginTabs host={host} slot="inspector.tab"
            ownerProps={{ workspaceId: 'ws-1', symbolId: 'A' }} defaultLabel="详情">
            <div data-testid="inspector-default">默认检查器内容</div>
          </PluginTabs>
          <div data-testid="slot-contextmenu">
            <SlotOutlet host={host} slot="contextmenu.item"
              ownerProps={{ workspaceId: 'ws-1', targetKind: 'symbol', symbolId: 'A' }} />
          </div>
          <div data-testid="slot-settings">
            <SlotOutlet host={host} slot="settings.section" ownerProps={{ workspaceId: 'ws-1' }} />
          </div>
          <RapidSwitchProbe />
        </section>
        <SlotOutlet host={host} slot="root" />
      </main>
    </div>
  )
}

const ready = loader.loadAll({
  filter: (manifest) => !String(manifest.id).startsWith('com.kicad_forge.'),
}).catch((error) => {
  host.diagnostics.push({ code: 'prototype/load-failed', message: error?.message || String(error) })
  return []
})

window.__KF_PROTOTYPE__ = {
  ready,
  host,
  loader,
  errors: host.diagnostics,
  runtimeChecks: loader.runtimeChecks,
  enable: (id) => loader.setEnabled(id, true),
  disable: (id) => loader.setEnabled(id, false),
  counts: () => ({
    root: host.count('root'),
    page: host.count('demo.prototype.page'),
    aux: host.count('demo.prototype.aux'),
    tabs: host.count('demo.supply.tabs'),
  }),
  plugins: () => loader.getSnapshot(),
}

createRoot(document.getElementById('root')).render(React.createElement(App))
