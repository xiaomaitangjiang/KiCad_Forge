/**
 * 编辑命令容量预算（U0 实测定稿；稳定契约见 docs/specs/EDITING_COMMANDS.md，测量证据见 docs/AgentDocs/FRONTEND_EDITING_FINAL_2026-10-20.md）。
 *
 * 实测依据（2026-10-20，本机 KiCad 官方符号库 + LCSC 拉取符号 + 合成 64 引脚符号）：
 * 单个符号节点 p50 1794 B / p90 1991 B / p99 4066 B / max 5001 B（多单元），
 * 合成 64 引脚 9678 B；结构化 envelope 元数据约 300 B/项。
 * C++ 宿主服务另存一份常量，由测试断言两边一致。
 */

export const MAX_CLIPBOARD_ITEMS = 200
export const MAX_CLIPBOARD_BYTES = 4 * 1024 * 1024
export const MAX_HISTORY_ENTRIES = 20
export const MAX_HISTORY_BYTES = 32 * 1024 * 1024
export const MAX_HISTORY_FILE_BYTES = 16 * 1024 * 1024

export const LIMITS = Object.freeze({
  maxClipboardItems: MAX_CLIPBOARD_ITEMS,
  maxClipboardBytes: MAX_CLIPBOARD_BYTES,
  maxHistoryEntries: MAX_HISTORY_ENTRIES,
  maxHistoryBytes: MAX_HISTORY_BYTES,
  maxHistoryFileBytes: MAX_HISTORY_FILE_BYTES,
})

function toFiniteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value : 0
}

/**
 * 校验一次剪贴板/历史负载是否超限。
 * 返回 null 表示可用；返回 'items' | 'bytes' 表示超限原因。
 */
export function checkCapacity(count, bytes, maxCount = MAX_CLIPBOARD_ITEMS, maxBytes = MAX_CLIPBOARD_BYTES) {
  if (toFiniteNumber(count) > maxCount) return 'items'
  if (toFiniteNumber(bytes) > maxBytes) return 'bytes'
  return null
}

export function clipboardBudgetExceeded(draft) {
  return checkCapacity(draft?.items, draft?.bytes)
}

export function historyBudgetExceeded(draft) {
  return checkCapacity(draft?.entries, draft?.bytes, MAX_HISTORY_ENTRIES, MAX_HISTORY_BYTES)
}
