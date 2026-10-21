import { MAX_CLIPBOARD_ITEMS, checkCapacity } from './limits.js'
import { utf8Bytes } from './clipboard-model.js'

// 对象粘贴的纯数据部分（ED-031/032）：把剪贴板 envelope 收敛成宿主请求，
// 冲突名与改名解析保持可单测，不依赖 DOM。
export function conflictNames(plan) {
  const list = Array.isArray(plan?.conflicts) ? plan.conflicts : []
  return list
    .map(item => (typeof item === 'string' ? item : item?.name))
    .map(name => String(name || '').trim())
    .filter(Boolean)
}

export function unresolvedConflicts(conflicts, renames) {
  return (conflicts || []).filter(name => !String(renames?.[name] || '').trim())
}

/** 只提交宿主需要的字段；源路径/UUID/摘要不得影响目标写权限。 */
export function pasteItems(envelope) {
  const items = Array.isArray(envelope?.items) ? envelope.items : []
  return items.map(item => ({
    name: String(item?.name || '').trim(),
    sexpr: String(item?.sexpr || ''),
    dependency: Boolean(item?.dependency),
  })).filter(item => item.name && item.sexpr)
}

export function pasteBudgetViolation(items, renames) {
  const payload = JSON.stringify({ items, renames: renames || {} })
  const violation = checkCapacity(items.length, utf8Bytes(payload), MAX_CLIPBOARD_ITEMS, 4 * 1024 * 1024)
  return violation
}

export function pasteRequest({ targetId, operationId, dryRun, renames, items, targetDigest }) {
  return {
    target_library_id: String(targetId || ''),
    operation_id: String(operationId || ''),
    dry_run: Boolean(dryRun),
    renames: renames || {},
    items: items || [],
    ...(targetDigest ? { target_digest: String(targetDigest) } : {}),
  }
}
