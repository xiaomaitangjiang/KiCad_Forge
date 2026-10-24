import type { ReactNode } from 'react'
import type { TaskHandle, TaskSnapshot } from '@kiforge/plugin-sdk'
import type { LcscModel } from './model.js'

export interface LcscImportModalProps {
  inline?: boolean
  pluginAction: { plugin: { id: string }; action: { name: string; analyze_action: string } }
  api: unknown
  compLibraries: Array<Record<string, unknown>>
  loadCompLibraries: () => Promise<Array<Record<string, unknown>> | null>
  onRefresh: () => void
  toastMsg: (message: string) => void
  onClose: () => void
  onTask: (handle: TaskHandle) => void
  task: TaskSnapshot | null
  onCancelTask: () => void
}

export default function LcscImportModal(props: LcscImportModalProps | { inline?: boolean; model: LcscModel }): ReactNode
