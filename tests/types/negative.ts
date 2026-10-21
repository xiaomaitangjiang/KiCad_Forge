import type { PluginContext, RenderSlot, SidebarPanelProps } from '@kiforge/plugin-sdk'
import { createSnapshotStore, declareSlot, sidebarPanel, sidebarSection, toolbarAction } from '@kiforge/plugin-sdk'

export default function compileNegative(ctx: PluginContext, renderSlot: RenderSlot) {
  ctx.slots.register(toolbarAction, { id: 'toolbar-readonly', render: props => {
    if (props.status) {
      // @ts-expect-error toolbar status counts are read-only
      props.status.symbols = 1
      // @ts-expect-error only public counts are exposed
      void props.status.privateDatabase
    }
    return null
  } })
  const store = ctx.model.createStore({ count: 0 })
  // @ts-expect-error store replacement must preserve its snapshot shape
  store.set({ count: 'wrong' })
  // @ts-expect-error store updates must return a complete snapshot
  store.update(() => ({}))
  // @ts-expect-error store listeners must be callable
  store.subscribe(42)
  // @ts-expect-error the store lifecycle flag is read-only
  store.disposed = false
  // @ts-expect-error standalone store replacements preserve their value type
  createSnapshotStore<number>(0).set('wrong')

  // @ts-expect-error a raw string is not a declared SlotDefinition
  ctx.slots.register('sidebar.section', { render: () => null })

  // @ts-expect-error the entry props do not match the sidebar owner contract
  ctx.slots.register(sidebarSection, { render: (props: { missing: number }) => props.missing })

  // @ts-expect-error the command name is a closed union
  ctx.commands.run('deleteEverything')

  // @ts-expect-error task actions are strings, not numbers
  ctx.tasks.run(42)

  // @ts-expect-error workspace snapshots have no such field
  void ctx.workspace.getSnapshot().nope

  // @ts-expect-error import operations require a string id
  void ctx.imports.operation({ id: 'op-1' })
  // @ts-expect-error sidebar.panel requires the full counts object
  const missingCounts: SidebarPanelProps = { workspaceId: null }
  void missingCounts

  // @ts-expect-error panel counts expose symbols/issues/matches only
  ctx.slots.register(sidebarPanel, { id: 'panel-counts', render: (props) => props.counts.missing })

  // @ts-expect-error legacy section props cannot require counts
  ctx.slots.register(sidebarSection, { id: 'section-counts', render: (props: { counts: number }) => props.counts })
  // @ts-expect-error typos are not in SlotMap; use defineSlot only for legacy names
  declareSlot('sidebar.secton')

  // @ts-expect-error the official inspector slot is keyed
  declareSlot('inspector.tab', 'list')

  // @ts-expect-error a known slot's literal name cannot claim another kind
  ctx.slots.register({ name: 'sidebar.panel', kind: 'keyed' }, { render: () => null })

  // @ts-expect-error a known slot's literal name cannot carry unrelated props
  ctx.slots.register({ name: 'sidebar.panel', kind: 'list', __props: { wrong: 1 } }, { render: () => null })

  // @ts-expect-error rendering a panel requires complete counts when props are passed
  renderSlot(sidebarPanel, { workspaceId: null })

  // @ts-expect-error a partial owner cannot substitute a made-up counts shape
  renderSlot(sidebarPanel, { workspaceId: null, counts: { symbols: 1 } })

  // @ts-expect-error community augmentation also checks owner props
  renderSlot(declareSlot('demo.supply.extra'), { label: 42 })

  ctx.slots.register(sidebarPanel, { id: 'read-only', render: (props) => {
    // @ts-expect-error panel owner props are readonly snapshots
    props.workspaceId = 'another-workspace'
    // @ts-expect-error counts cannot be replaced by a plugin
    props.counts = { symbols: 0, issues: 0, matches: 0 }
    // @ts-expect-error nested counts cannot be changed by a plugin
    props.counts.symbols = 0
    return null
  } })

  ctx.slots.register(sidebarSection, { id: 'read-only-legacy', render: (props) => {
    // @ts-expect-error the legacy owner props are readonly too
    props.workspaceId = 'another-workspace'
    if (props.counts) {
      // @ts-expect-error optional legacy counts are also readonly
      props.counts.issues = 0
    }
    return null
  } })
}
