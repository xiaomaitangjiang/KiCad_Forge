// The LCSC flow is a first-class main-sidebar panel (WP-001/002/004). The
// plugin no longer registers a center view or an "open details" summary; the
// complete lookup → target → symbol → footprint flow lives here.
import { useEffect, useSyncExternalStore } from 'react'
import { definePlugin, sidebarPanel } from '@kiforge/plugin-sdk'
import LcscImportModal from './LcscImportModal.jsx'
import { createTaskApi } from './task-adapter.js'
import { createLcscModel } from './model.js'
import type { LcscModel } from './model.js'
import './style.css'

const STEPS = [
  { id: 'lookup', label: '查找' },
  { id: 'target', label: '目标' },
  { id: 'symbol', label: '子库' },
  { id: 'footprint', label: '封装' },
]

function useLcscSnapshot(model: LcscModel) {
  return useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)
}

function stepIndex(step: string | undefined, completed: boolean) {
  if (completed) return STEPS.length
  const index = STEPS.findIndex(item => item.id === step)
  return index < 0 ? 0 : index
}

/** Complete LCSC import panel rendered inside the primary sidebar. */
function LcscSidebarPanel({ model }: { model: LcscModel }) {
  const snapshot = useLcscSnapshot(model)
  useEffect(() => { void model.start() }, [model])
  const { step, completed, toast } = snapshot
  const current = stepIndex(step, completed)
  return (
    <section className="lcsc-sidebar-panel" data-testid="lcsc-panel" aria-label="从 LCSC 导入">
      <ol className="lcsc-stepbar" data-testid="lcsc-stepbar" aria-label="导入步骤">
        {STEPS.map((item, index) => (
          <li key={item.id}
            data-step={item.id}
            data-state={index < current ? 'done' : index === current ? 'active' : 'todo'}
            className={`lcsc-step${index < current ? ' done' : ''}${index === current ? ' active' : ''}`}
            aria-current={index === current ? 'step' : undefined}>
            <span className="lcsc-step-index" aria-hidden="true">{index < current ? '✓' : index + 1}</span>
            <span className="lcsc-step-label">{item.label}</span>
          </li>
        ))}
      </ol>
      <LcscImportModal inline model={model} />
      {toast && <div className="plugin-toast" data-testid="lcsc-toast">{toast}</div>}
    </section>
  )
}

export default definePlugin({
  name: 'com.kicad_forge.lcsc_import.frontend',
  apply(ctx) {
    const services = {
      libraries: ctx.libraries,
      footprints: ctx.footprints,
      commands: ctx.commands,
      tasks: ctx.tasks,
    }
    const api = createTaskApi(services)
    const model = createLcscModel({ api, createStore: ctx.model.createStore,
      pluginAction: { plugin: { id: 'com.kicad_forge.lcsc_import' },
        action: { name: 'Import LCSC', analyze_action: 'analyze' } },
      onRefresh: () => services.commands.run('refreshAll'),
    })
    ctx.on('dispose', model.dispose)
    ctx.slots.register(sidebarPanel, {
      id: 'lcsc-import',
      title: '从 LCSC 导入',
      // The manifest declares icon.svg; the host resolves it to an asset URL.
      // Do not inline a second copy of the icon here.
      render: () => <LcscSidebarPanel model={model} />,
    })
  },
})
