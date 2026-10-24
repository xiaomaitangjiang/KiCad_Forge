import type { ReactNode } from 'react'
import type { SlotDefinition } from '@kiforge/plugin-sdk'
import {
  cardAccessory,
  contextMenuItem,
  createSnapshotStore,
  declareSlot,
  definePlugin,
  defineSlot,
  inspectorTab,
  settingsSection,
  sidebarPanel,
  sidebarSection,
  toolbarAction,
} from '@kiforge/plugin-sdk'

// Community slots are declared through module augmentation + the strict API.
declare module '@kiforge/plugin-sdk' {
  interface SlotMap {
    'demo.supply.extra': { label: string }
  }
}

// Positive case: a community plugin consumes only the published SDK surface.
const tabs = defineSlot<{ label: string }>('demo.supply.tabs', 'keyed')
const extra = declareSlot('demo.supply.extra', 'list')
const standaloneStore = createSnapshotStore({ count: 0 }, { freeze: true })
const disposed: boolean = standaloneStore.disposed
void disposed
// Structural handles with a widened name remain a 1.x compatibility surface.
const legacyHandle: SlotDefinition<{ label: string }> = { name: 'demo.legacy', kind: 'keyed' }
const inferredInspector = declareSlot('inspector.tab')
const inspectorKind: 'keyed' = inferredInspector.kind
void inspectorKind

export default definePlugin({
  name: 'type-positive',
  apply(ctx) {
    ctx.slots.register(extra, { id: 'extra', render: (props) => props.label })
    ctx.slots.register(legacyHandle, { id: 'legacy', render: (props) => props.label })
    ctx.slots.register<{ label: string }>(legacyHandle, { id: 'legacy-generic', render: (props) => props.label })
    ctx.slots.register({ name: 'sidebar.panel', kind: 'list' }, {
      id: 'literal-panel', render: (props) => props.counts.symbols,
    })

    ctx.slots.register(tabs, {
      id: 'inventory',
      key: 'inventory',
      title: '库存',
      order: 10,
      render: (props): ReactNode => props.label,
    })

    ctx.slots.register(sidebarSection, { id: 'sidebar', render: () => null })
    // Legacy section props stay workspace-only; counts is optional.
    ctx.slots.register(sidebarSection, { id: 'sidebar-counts', render: (props) => props.counts?.symbols ?? props.workspaceId })
    // The activity panel contract always supplies counts.
    ctx.slots.register(sidebarPanel, { id: 'activity', render: (props) => `${props.counts.symbols}:${props.workspaceId}` })
    // A render callback may ignore fields supplied by the host.
    ctx.slots.register(sidebarPanel, { id: 'workspace-only', render: (props: { workspaceId: string | null }) => props.workspaceId })
    ctx.slots.register(toolbarAction, { id: 'toolbar', render: () => null })
    ctx.slots.register(toolbarAction, { id: 'toolbar-counts', render: props => props.status?.symbols ?? props.workspaceId })
    ctx.slots.register(cardAccessory, {
      id: 'card',
      render: (props) => `${props.symbolId}:${props.selected}`,
    })
    ctx.slots.register(inspectorTab, {
      id: 'details',
      key: 'details',
      title: '详情',
      render: (props) => props.symbolId,
    })
    ctx.slots.register(contextMenuItem, { id: 'copy', render: () => null })
    ctx.slots.register(settingsSection, { id: 'settings', render: () => null })

    const workspace = ctx.workspace.getSnapshot()
    void workspace.workspaceId
    void ctx.contracts.get('demo.supplier')
    void ctx.tasks.run('info')
    void ctx.commands.run('refreshAll')
    void ctx.imports.operation('op-1')

    const model = ctx.model.createStore({ count: 0 })
    model.set({ count: 1 })
    model.update((current) => ({ count: current.count + 1 }))
    const snapshot: { count: number } = ctx.model.useSnapshot(model)
    void snapshot.count
    const off = model.subscribe(() => {})
    off()
    model.dispose()

    ctx.views.register({
      id: 'demo.supply',
      title: '供应信息',
      slots: [tabs],
      render: ({ renderSlot }) => {
        renderSlot<{ label: string }>(legacyHandle, { label: 'legacy' })
        renderSlot(sidebarPanel, { workspaceId: null, counts: { symbols: 1, issues: 0, matches: 0 } })
        return renderSlot(tabs)
      },
    })

    ctx.slots.onDeclared(tabs, (declared) => {
      if (declared) ctx.logger.info('declared')
    })
  },
})
