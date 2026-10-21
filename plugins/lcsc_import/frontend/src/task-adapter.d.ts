import type { FootprintsContract, LibrariesContract, TasksContract, TaskHandle, TaskSnapshot } from '@kiforge/plugin-sdk'

export interface LcscServices {
  tasks: TasksContract
  libraries: LibrariesContract
  footprints: FootprintsContract
}

export interface LcscTaskApi {
  dispose(): void
  executePlugin(
    pluginId: string,
    body: Record<string, unknown>,
    onTask?: (handle: TaskHandle) => void,
  ): Promise<{ ok: boolean; error?: string; plugin_result?: unknown }>
  compLibraries(): Promise<Array<Record<string, unknown>>>
  componentSymbolLibraries(id: string): Promise<unknown>
  searchFootprints(query: string, near?: string): Promise<Array<Record<string, unknown>>>
}

export function createTaskApi(services: LcscServices): LcscTaskApi
export function waitTask(tasks: TasksContract, task: TaskSnapshot): Promise<TaskSnapshot>
