import type { ModelApi, SnapshotStore, TaskSnapshot } from '@kiforge/plugin-sdk'
import type { TaskApi } from './task-adapter.js'

export interface LcscModelSnapshot {
  step: 'lookup' | 'target' | 'symbol' | 'footprint'
  toast: string
  task: TaskSnapshot | null
  completed: boolean
  refreshSucceeded: boolean
  canBeginNewIntent: boolean
  busy: boolean
}
export interface LcscModel extends SnapshotStore<LcscModelSnapshot> {
  start(): Promise<unknown>
  beginNewIntent(): boolean
  activate(): void
  suspend(): void
  dispose(): void
}
export function createLcscModel(options: {
  api: TaskApi
  createStore: ModelApi['createStore']
  pluginAction: { plugin: { id: string }; action: { name: string; analyze_action: string } }
  onRefresh: () => Promise<void>
}): LcscModel
