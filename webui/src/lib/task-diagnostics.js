// 任务诊断文本只服务剪贴板：字段固定、长度有界，不携带 result 或其他任务。
export const MAX_TASK_DIAGNOSTIC_LENGTH = 1024
export const MAX_TASK_DIAGNOSTIC_ERROR_LENGTH = 320
const MAX_TASK_DIAGNOSTIC_FIELD_LENGTH = 160

function bounded(value, max) {
  const text = typeof value === 'string' ? value : value === null || value === undefined ? '' : String(value)
  if (!text) return ''
  if (text.length <= max) return text
  return text.slice(0, Math.max(0, max - 1)) + '…'
}

/** 终端记录里的 updatedAt 是 epoch 毫秒；无效值不伪造时间。 */
export function formatTaskDiagnosticTime(value) {
  const time = typeof value === 'number' ? value : Number(value)
  if (!Number.isFinite(time)) return ''
  const date = new Date(time)
  if (Number.isNaN(date.getTime())) return ''
  return date.toISOString()
}

/** 只暴露诊断需要的标量字段；调用方只能拿到当前任务自己的数据。 */
export function taskDiagnosticFields(task) {
  const source = task && typeof task === 'object' ? task : {}
  return {
    id: bounded(source.id, MAX_TASK_DIAGNOSTIC_FIELD_LENGTH),
    pluginId: bounded(source.pluginId, MAX_TASK_DIAGNOSTIC_FIELD_LENGTH),
    action: bounded(source.action, MAX_TASK_DIAGNOSTIC_FIELD_LENGTH),
    state: bounded(source.state, 40),
    // M3: uncertain must always be explicit so true/false diagnostics differ.
    uncertain: source.uncertain === true,
    phase: bounded(source.phase, MAX_TASK_DIAGNOSTIC_FIELD_LENGTH),
    time: formatTaskDiagnosticTime(source.updatedAt),
    error: bounded(source.error, MAX_TASK_DIAGNOSTIC_ERROR_LENGTH),
  }
}

/**
 * 单行任务行复制的内容：身份、插件、动作、状态、阶段、时间、错误摘要。
 * 不读取 result；总长度超过 1024 时整体截断，保证剪贴板有界。
 */
export function formatTaskDiagnostics(task) {
  const fields = taskDiagnosticFields(task)
  const keyLines = [
    `task: ${fields.id || '-'}`,
    `plugin: ${fields.pluginId || '-'}`,
    `action: ${fields.action || '-'}`,
    `state: ${fields.state || 'unknown'}`,
    `uncertain: ${fields.uncertain ? 'true' : 'false'}`,
    `phase: ${fields.phase || '-'}`,
    `time: ${fields.time || '-'}`,
  ]
  // M3: never drop identity/state/uncertain/phase/time just because the text is
  // long. Only the trailing error line is shortened to fit the total bound.
  const errorPrefix = 'error: '
  const errorValue = fields.error || '-'
  const fixed = keyLines.join('\n') + '\n' + errorPrefix
  const room = MAX_TASK_DIAGNOSTIC_LENGTH - fixed.length
  if (room <= 0) {
    // Defensive only: current per-field caps keep the fixed part under the
    // total bound, but never return text over the advertised limit.
    return fixed.slice(0, MAX_TASK_DIAGNOSTIC_LENGTH)
  }
  if (errorValue.length <= room) return fixed + errorValue
  return fixed + bounded(errorValue, room)
}
