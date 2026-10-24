// KiCad Forge plugin SDK — phase 2 public surface.
//
// This package is the only import a community plugin needs. It deliberately
// does not re-export Cordis, SlotCore or any DSH package type: the public
// declarations below are self-contained apart from React's ReactNode type.

import type { ReactNode } from 'react'

export const SDK_VERSION = '1.1.0'
export const CONTRACT_VERSION = '1.0.0'

export type SlotKind = 'single' | 'list' | 'keyed' | 'chain'

/**
 * A typed slot handle. A literal name is checked against SlotMap at use sites.
 * The default string name preserves the structural 1.x compatibility surface;
 * use declareSlot() to retain name/kind/props checking throughout the API.
 */
export interface SlotDefinition<
  P = Record<string, unknown>,
  N extends string = string,
  K extends SlotKind = SlotKind,
> {
  readonly name: N
  readonly kind: K
  /** Phantom marker used only to carry P through register/onDeclared. */
  readonly __props?: P
}

/** Compatibility declaration for community/legacy usage: the name is not
 * compile-checked. New code should augment SlotMap and use declareSlot(). */
export function defineSlot<P = Record<string, unknown>>(
  name: string,
  kind: SlotKind = 'list',
): SlotDefinition<P> {
  if (typeof name !== 'string' || !/^[a-z0-9]+(?:[._-][a-z0-9]+)*$/i.test(name)) {
    throw new Error(`Invalid slot name: ${String(name)}`)
  }
  return Object.freeze({ name, kind })
}

/** Official and community slot declarations. Communities add an entry through
 * module augmentation, then get name/props/kind checked by declareSlot(). */
export interface SlotMap {
  'sidebar.section': SidebarSectionProps
  'sidebar.panel': SidebarPanelProps
  'toolbar.action': ToolbarActionProps
  'card.accessory': CardAccessoryProps
  'inspector.tab': InspectorTabProps
  'contextmenu.item': ContextMenuItemProps
  'settings.section': SettingsSectionProps
  root: Record<string, unknown>
}

const officialSlotKinds = {
  'sidebar.section': 'list',
  'sidebar.panel': 'list',
  'toolbar.action': 'list',
  'card.accessory': 'list',
  'inspector.tab': 'keyed',
  'contextmenu.item': 'list',
  'settings.section': 'list',
  root: 'list',
} as const

/** Official slot kinds are fixed. Community slots may choose their own kind. */
export type SlotKindFor<K extends keyof SlotMap> =
  K extends keyof typeof officialSlotKinds ? (typeof officialSlotKinds)[K] : SlotKind

type SlotProps<S extends SlotDefinition<unknown>> =
  S['name'] extends keyof SlotMap ? SlotMap[S['name']]
    : S extends SlotDefinition<infer P> ? P : never

type CheckedSlot<S extends SlotDefinition<unknown>> =
  S['name'] extends keyof SlotMap
    ? SlotDefinition<SlotMap[S['name']], S['name'], SlotKindFor<S['name']>>
    : unknown

/** Strict declaration: only names present in SlotMap are accepted, and props
 * are inferred from that name. Official slots also have a fixed kind. */
export function declareSlot<K extends keyof SlotMap>(
  name: K,
  kind?: SlotKindFor<K>,
): SlotDefinition<SlotMap[K], K, SlotKindFor<K>> {
  const defaultKind = Object.hasOwn(officialSlotKinds, name)
    ? officialSlotKinds[name as keyof typeof officialSlotKinds] : 'list'
  return defineSlot<SlotMap[K]>(name, kind ?? defaultKind) as SlotDefinition<SlotMap[K], K, SlotKindFor<K>>
}

// ---------------------------------------------------------------------------
// Persistent public snapshots / model
// ---------------------------------------------------------------------------

export interface SnapshotStore<T> {
  /** Same reference until a commit; callers must treat it as readonly. */
  getSnapshot(): T
  /** Idempotent disposer; notifications stop after release. */
  subscribe(listener: () => void): () => void
}

/** Plugin-owned mutable snapshot; storage and subscriptions are host-provided. */
export interface MutableSnapshotStore<T> extends SnapshotStore<T> {
  set(next: T): void
  update(updater: (current: T) => T): void
  /** Idempotent. ctx.model.createStore also binds this to plugin disposal. */
  dispose(): void
  readonly disposed: boolean
}

/** Shared React-free implementation for host and standalone plugin models. */
export function createSnapshotStore<T>(
  initial: T, { freeze = false }: { freeze?: boolean } = {},
): MutableSnapshotStore<T> {
  let snapshot = freeze ? Object.freeze(initial) : initial
  let disposed = false
  const listeners = new Set<() => void>()

  function getSnapshot() {
    return snapshot
  }

  function subscribe(listener: () => void) {
    if (disposed) return () => {}
    listeners.add(listener)
    return () => { listeners.delete(listener) }
  }

  function set(next: T) {
    if (disposed || Object.is(next, snapshot)) return
    snapshot = freeze ? Object.freeze(next) : next
    for (const listener of [...listeners]) listener()
  }

  function update(updater: (current: T) => T) {
    set(updater(snapshot))
  }

  function dispose() {
    disposed = true
    listeners.clear()
  }

  return {
    getSnapshot, subscribe, set, update, dispose,
    get disposed() { return disposed },
  }
}

export type UseSnapshot = <T>(store: SnapshotStore<T>) => T

export interface ModelApi {
  /** Reuses the host snapshot implementation; no React or polling required. */
  createStore<T>(initial: T): MutableSnapshotStore<T>
  /** Host-bound useSyncExternalStore adapter; never fetched by the plugin. */
  useSnapshot: UseSnapshot
}

// ---------------------------------------------------------------------------
// Seven business contracts
// ---------------------------------------------------------------------------

export interface SymbolQuery {
  q?: string
  libraryId?: string
}

export interface SymbolSnapshot {
  id: string
  name: string
  libraryId: string
  libraryName?: string
  typeId?: string
  typeName?: string
  mpn?: string
  value?: string
  footprint?: string
  pins?: number
  locked?: boolean
}

export interface SymbolBinding {
  symbolId: string
  footprintId?: string
  footprintName?: string
  footprintStatus?: string
  modelId?: string
  modelName?: string
}

export interface SymbolMatch {
  footprintId: string
  name?: string
  library?: string
  score?: number
  footprintStatus?: string
  reason?: string
}

export interface SymbolsContract {
  list(query?: SymbolQuery): Promise<SymbolSnapshot[]>
  get(id: string): Promise<SymbolSnapshot | null>
  binding(symbolId: string): Promise<SymbolBinding | null>
  bindFootprint(symbolId: string, footprintId: string): Promise<void>
  bindModel(footprintId: string, modelId: string): Promise<void>
  matches(symbolId: string): Promise<SymbolMatch[]>
}

export interface SymbolLibrary {
  id: string
  name: string
  path?: string
  symbolCount?: number
  locked?: boolean
}

export interface ComponentLibrary {
  id: string
  name: string
  symbolPath?: string
  footprintPath?: string
  enabled?: boolean
  locked?: boolean
  isProtected?: boolean
}

export interface SymbolLibraryCatalog {
  libraries: SymbolLibrary[]
  canCreate: boolean
  caseSensitive?: boolean
}

export interface LibrariesContract {
  symbolLibraries(): Promise<SymbolLibrary[]>
  componentLibraries(): Promise<ComponentLibrary[]>
  componentSymbolLibraries(componentLibraryId: string): Promise<SymbolLibraryCatalog>
}

export interface FootprintSearchResult {
  footprintId: string
  name?: string
  library?: string
  score?: number
  padCount?: number
  footprintStatus?: string
  reason?: string
}

export interface FootprintsContract {
  search(q: string, near?: string): Promise<FootprintSearchResult[]>
  searchModels(q: string, near?: string): Promise<Array<Record<string, unknown>>>
}

export type ImportTargetComponentLibraryId = string
export type ImportOperationId = string

export interface ImportAnalyzeRequest {
  /** Manifest ID of the plugin declaring one staged-import-v1 action. */
  provider: string
  externalId: string
}

export interface ImportAnalysis {
  externalId: string
  symbolNames?: string[]
  footprintName?: string
  modelName?: string
  candidates?: FootprintSearchResult[]
  warning?: string
}

export type SymbolLibraryChoice =
  | { mode: 'existing'; path: string }
  | { mode: 'new'; name: string }

export type FootprintChoice =
  | { mode: 'new' }
  | { mode: 'reuse'; footprintId: string }

export interface ImportSelection {
  /** Provider plugin ID; separate from the host's import-source label. */
  provider: string
  externalId: string
  targetComponentLibraryId: ImportTargetComponentLibraryId
  symbolLibraryChoice: SymbolLibraryChoice
  footprintChoice: FootprintChoice
  /** Idempotency key for one confirmed intent; separate from task id. */
  operationId?: ImportOperationId
}

export interface ImportResult {
  ok: boolean
  noop?: boolean
  replayed?: boolean
  operationId?: ImportOperationId
  externalId?: string
  symbolId?: string
  symbolName?: string
  footprintId?: string
  footprintReference?: string
  mode?: 'new' | 'reuse'
  createdLibrary?: boolean
  warnings?: string[]
  error?: string
}

export type CancelStatus =
  | 'cancel_requested'
  | 'already_requested'
  | 'not_cancelable'
  | 'terminal'

export interface CancelResult {
  status: CancelStatus
  state?: TaskState
  phase?: string
}

export type TaskState = 'queued' | 'running' | 'succeeded' | 'failed' | 'cancelled'

export interface TaskSnapshot<T = unknown> {
  id: string
  action: string
  state: TaskState
  phase?: string
  cancelable: boolean
  result?: T
  error?: string
  uncertain?: boolean
}

export interface TaskHandle<T = unknown> extends SnapshotStore<TaskSnapshot<T>> {
  readonly id: string
  cancel(): Promise<CancelResult>
}

export interface ImportsContract {
  analyze(input: ImportAnalyzeRequest): Promise<ImportAnalysis>
  commit(selection: ImportSelection): Promise<TaskHandle<ImportResult>>
  operation(operationId: ImportOperationId): Promise<ImportResult | null>
  cancel(taskId: string): Promise<CancelResult>
}

export interface TasksContract {
  /** Current plugin identity is injected by the host; no pluginId argument. */
  run<T = unknown>(action: string, args?: Record<string, unknown>): Promise<TaskHandle<T>>
  get<T = unknown>(taskId: string): Promise<TaskSnapshot<T> | null>
  cancel(taskId: string): Promise<CancelResult>
  list(): Promise<TaskSnapshot[]>
}

export type CommandName =
  | 'refreshSymbols'
  | 'refreshLibraries'
  | 'refreshStatus'
  | 'refreshAll'

export interface CommandsContract {
  run(command: CommandName): Promise<void>
}

export interface WorkspaceSnapshot {
  workspaceId: string | null
  selectedSymbolId: string | null
  libraryFilterId: string | null
}

export interface WorkspaceContract extends SnapshotStore<WorkspaceSnapshot> {}

// ---------------------------------------------------------------------------
// Contracts registry (versions / duplicate providers)
// ---------------------------------------------------------------------------

export interface ContractApi {
  provide<T>(contract: string, version: string, implementation: T): void
  require<T>(contract: string, range?: string): T
  get<T>(contract: string): T | undefined
}

// ---------------------------------------------------------------------------
// Config / diagnostics
// ---------------------------------------------------------------------------

export interface ConfigApi<T extends Record<string, unknown> = Record<string, unknown>> {
  get(): T
  set(patch: Partial<T>): Promise<void>
  subscribe(listener: (value: T) => void): () => void
}

export type DiagnosticSeverity = 'info' | 'warning' | 'error'

export interface DiagnosticReport {
  code: string
  severity?: DiagnosticSeverity
  message: string
  detail?: unknown
}

export interface DiagnosticsApi {
  report(report: DiagnosticReport): void
}

// ---------------------------------------------------------------------------
// Seven public slots
// ---------------------------------------------------------------------------

export interface SidebarCounts {
  readonly symbols: number
  readonly issues: number
  readonly matches: number
}

/** Legacy embedded workspace section. counts is optional for old callers. */
export interface SidebarSectionProps {
  readonly workspaceId: string | null
  readonly counts?: SidebarCounts
}

/** Activity-bar panel; the host always supplies counts. */
export interface SidebarPanelProps {
  readonly workspaceId: string | null
  readonly counts: SidebarCounts
  /** Open one of this plugin's `app.view` pages in the center workspace. */
  readonly openView?: (localId: string) => void
}

export interface ToolbarActionProps {
  readonly workspaceId: string | null
  /** Supplied by the host; optional for existing workspace-only owners. */
  readonly status?: {
    readonly symbols: number
    readonly footprints: number
  }
}

export interface CardAccessoryProps {
  workspaceId: string | null
  symbolId: string
  selected: boolean
}

export interface InspectorTabProps {
  workspaceId: string | null
  symbolId: string
}

export interface ContextMenuItemProps {
  workspaceId: string | null
  targetKind: 'symbol' | 'selection'
  symbolId?: string
}

export interface SettingsSectionProps {
  workspaceId: string | null
}

export const sidebarSection = declareSlot('sidebar.section', 'list')
export const sidebarPanel = declareSlot('sidebar.panel', 'list')
export const toolbarAction = declareSlot('toolbar.action', 'list')
export const cardAccessory = declareSlot('card.accessory', 'list')
export const inspectorTab = declareSlot('inspector.tab', 'keyed')
export const contextMenuItem = declareSlot('contextmenu.item', 'list')
export const settingsSection = declareSlot('settings.section', 'list')

/** Root is seeded by the host; plugins may contribute root-level entries. */
export const rootSlot = declareSlot('root', 'list')

// ---------------------------------------------------------------------------
// Slots / views
// ---------------------------------------------------------------------------

export interface RenderSlot {
  <P, const S extends SlotDefinition<P> = SlotDefinition<P>>(
    slot: S & CheckedSlot<S>, props?: NoInfer<SlotProps<S>>,
  ): ReactNode
}

export interface SlotRenderProps {
  renderSlot: RenderSlot
}

export interface SlotEntry<P = Record<string, unknown>> {
  /** Stable identity inside a list slot. */
  id?: string
  /** Cell key inside a keyed slot. */
  key?: string
  /** Human-readable label used by diagnostics/tab headers later. */
  title?: string
  /** Optional icon rendered by activity-style hosts (React node supplied by the plugin). */
  icon?: ReactNode
  order?: number
  /** Child slots declared by this contribution; recursively disposed with it. */
  children?: Record<string, { kind?: SlotKind; scope?: 'root' }>
  render: (props: P & SlotRenderProps) => ReactNode
}

export interface SlotApi {
  register<P, const S extends SlotDefinition<P> = SlotDefinition<P>>(
    slot: S & CheckedSlot<S>, entry: SlotEntry<NoInfer<SlotProps<S>>>,
  ): () => void
  /** Fires when a slot's declaration appears or collapses. */
  onDeclared(slot: SlotDefinition<unknown>, listener: (declared: boolean) => void): () => void
}

export interface ViewDefinition {
  id: string
  title: string
  order?: number
  slots?: readonly SlotDefinition<unknown>[]
  render: (props: { renderSlot: RenderSlot }) => ReactNode
}

export interface ViewApi {
  register(view: ViewDefinition): () => void
}

// ---------------------------------------------------------------------------
// Plugin context
// ---------------------------------------------------------------------------

export interface Logger {
  info(...args: unknown[]): void
  warn(...args: unknown[]): void
  error(...args: unknown[]): void
}

/** The narrowed context a plugin's apply() receives. */
export interface PluginContext {
  readonly plugin: { id: string; version: string }
  /** Legacy Cordis service registration kept for phase-1 plugins. */
  provide(name: string, value: unknown): void
  effect(disposer: () => void): void
  on(event: 'dispose', listener: () => void): void
  logger: Logger

  slots: SlotApi
  views: ViewApi
  contracts: ContractApi
  config: ConfigApi
  diagnostics: DiagnosticsApi
  model: ModelApi

  symbols: SymbolsContract
  libraries: LibrariesContract
  footprints: FootprintsContract
  imports: ImportsContract
  tasks: TasksContract
  commands: CommandsContract
  workspace: WorkspaceContract
}

export interface PluginDefinition {
  name: string
  inject?: string[]
  apply(ctx: PluginContext): void | (() => void)
}

/** Identity helper; kept as a function so the plugin shape is explicit. */
export function definePlugin(definition: PluginDefinition): PluginDefinition {
  return definition
}
