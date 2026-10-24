// 剪切/移动的纯数据规则（ED-040..045）。
// 剪贴板里的对象内容只用于「与本次剪切意图匹配」；真正移动时源对象与摘要
// 由宿主按 token 重新读取，前端不把剪贴板内容当授权。
import { pasteBudgetViolation, pasteItems } from './paste-model.js'

const DIRECT = (items) => (Array.isArray(items) ? items.filter(item => item && !item.dependency) : [])

/// 直接选中项的身份签名：UUID + 源摘要 + 名称。依赖项不参与，因为父节点
/// 可能同时被直接选中，UI 展示顺序也可能不同。
export function cutSignature(items) {
  return DIRECT(items)
    .map(item => [item.uuid || '', item.source_digest || '', item.name || ''].join('\n'))
    .join('\n')
}

/// 系统剪贴板内容是否仍是这次剪切时写入的那份。改了内容就不再当移动，
/// 只保留源对象（ED-044）；token 由调用方决定是否释放。
export function envelopeMatchesCut(envelope, cut) {
  if (!envelope || !cut || !Array.isArray(envelope.items)) return false
  const direct = DIRECT(envelope.items)
  if (direct.length === 0) return false
  return cutSignature(direct) === cutSignature(cut.items)
}

/// 移动预检里必须由用户改名的冲突；其余原因（缺父、未移动派生、已在目标）
/// 不能靠改名解决，必须阻止提交并说明。
export function moveNameConflicts(plan) {
  const conflicts = Array.isArray(plan?.conflicts) ? plan.conflicts : []
  const names = []
  for (const conflict of conflicts) {
    const name = typeof conflict === 'string' ? conflict : conflict?.name
    const reason = typeof conflict === 'string' ? 'name_conflict' : (conflict?.reason || 'name_conflict')
    if (!name || reason !== 'name_conflict') continue
    if (!names.includes(name)) names.push(name)
  }
  return names
}

export function blockingMoveConflicts(plan) {
  const conflicts = Array.isArray(plan?.conflicts) ? plan.conflicts : []
  return conflicts.filter((conflict) => {
    if (typeof conflict === 'string') return false
    return conflict?.name && conflict.reason && conflict.reason !== 'name_conflict'
  })
}

export function moveDirectItems(items) {
  return DIRECT(items)
}

/// 移动请求：dry_run 不带摘要；提交必须带预检拿到的目标摘要（ED-042），
/// 新建目标时为空，不发送该字段。
export function moveRequest({ token, targetId, operationId, dryRun, renames, targetDigest }) {
  const body = {
    token,
    target_library_id: targetId,
    operation_id: operationId,
    dry_run: dryRun !== false,
    renames: renames || {},
  }
  if (!body.dry_run && targetDigest) body.target_digest = targetDigest
  return body
}

export { pasteBudgetViolation, pasteItems }
