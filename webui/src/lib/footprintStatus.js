// 封装/3D 存在性状态 → 界面语义的唯一映射点。
//
// 硬规则（对应用户示例 SS-0323PG-14N5WW：引用写着 My_Library:SW-SMD_... 但本地没有）：
//   * 名称非空、UUID 非空、has_footprint/has_3d_model 这些旧字段都不能当成「存在」
//   * 只有服务端 status === 'available' 才是正常
//   * 未知/缺失状态 = 未检查：不显示正常，也不允许选中
//   * 加载中、请求失败与「未检查」是三种不同显示，不能互相顶替

export const FOOTPRINT_STATUSES = [
  'unassigned', 'not_found', 'ambiguous', 'stale', 'available', 'check_failed',
]
export const MODEL_STATUSES = ['unassigned', 'stale', 'available', 'check_failed']

/// 只在服务端明确给了合法状态时才算已知
export function footprintStatusOf(entity) {
  const raw = entity && typeof entity === 'object' ? entity.footprint_status : undefined
  return typeof raw === 'string' && FOOTPRINT_STATUSES.includes(raw) ? raw : 'unknown'
}

export function modelStatusOf(entity) {
  const raw = entity && typeof entity === 'object' ? entity.model_status : undefined
  return typeof raw === 'string' && MODEL_STATUSES.includes(raw) ? raw : 'unknown'
}

export function footprintReasonOf(entity) {
  const raw = entity && typeof entity === 'object' ? entity.footprint_reason : undefined
  return typeof raw === 'string' ? raw : ''
}

export function modelReasonOf(entity) {
  const raw = entity && typeof entity === 'object' ? entity.model_reason : undefined
  return typeof raw === 'string' ? raw : ''
}

/// 符号原始引用：服务端字段优先，退回列表里的 footprint 字段（永远保留显示）
export function footprintReferenceOf(entity) {
  if (!entity || typeof entity !== 'object') return ''
  if (typeof entity.footprint_reference === 'string' && entity.footprint_reference) {
    return entity.footprint_reference
  }
  return typeof entity.footprint === 'string' ? entity.footprint : ''
}

export function footprintIsAvailable(entity) {
  return footprintStatusOf(entity) === 'available'
}

/// 3D 正常 = 服务端说封装可用且模型可用（不看旧 has_3d_model）
export function modelIsUsable(entity) {
  return footprintIsAvailable(entity) && modelStatusOf(entity) === 'available'
}

const TONES = {
  available: 'ok',
  unassigned: 'warn',
  not_found: 'bad',
  ambiguous: 'bad',
  stale: 'bad',
  check_failed: 'warn',
  unknown: 'unknown',
  loading: 'unknown',
  error: 'unknown',
}

const LABEL_KEYS = {
  available: 'footprintStatus.available',
  unassigned: 'footprintStatus.unassigned',
  not_found: 'footprintStatus.notFound',
  ambiguous: 'footprintStatus.ambiguous',
  stale: 'footprintStatus.stale',
  check_failed: 'footprintStatus.checkFailed',
  unknown: 'footprintStatus.unknown',
  loading: 'footprintStatus.loading',
  error: 'footprintStatus.error',
}

/// 原因码 → i18n key（未知原因码只显示状态文案，不显示原始码）
export function footprintReasonKey(reason) {
  return reason ? `footprintStatus.reason.${reason}` : ''
}

/// 缺失/失效/歧义/检查失败都要能换一个封装：只有「没写引用」走新建流程
export function canReplaceFootprint(status) {
  return ['not_found', 'stale', 'ambiguous', 'check_failed'].includes(status)
}

/// 封装视图。loading / failed 由调用方传入（对应请求生命周期），
/// 两者都不会被当成正常，也不会被当成「未检查」。
export function footprintView(entity, { loading = false, failed = false } = {}) {
  if (loading) {
    return {
      status: 'loading', reason: '', known: false, available: false, problem: false,
      tone: TONES.loading, labelKey: LABEL_KEYS.loading, reasonKey: '',
      reference: footprintReferenceOf(entity), canReplace: false, loading: true, failed: false,
    }
  }
  if (failed) {
    return {
      status: 'error', reason: '', known: false, available: false, problem: true,
      tone: TONES.error, labelKey: LABEL_KEYS.error, reasonKey: '',
      reference: footprintReferenceOf(entity), canReplace: false, loading: false, failed: true,
    }
  }
  const status = footprintStatusOf(entity)
  const reason = footprintReasonOf(entity)
  return {
    status,
    reason,
    known: status !== 'unknown',
    available: status === 'available',
    problem: status !== 'available',
    tone: TONES[status],
    labelKey: LABEL_KEYS[status],
    reasonKey: footprintReasonKey(reason),
    reference: footprintReferenceOf(entity),
    canReplace: canReplaceFootprint(status),
    loading: false,
    failed: false,
  }
}

/// 3D 模型视图：模型状态只在封装可用的前提下才有意义
export function modelView(entity, { loading = false, failed = false } = {}) {
  if (loading) {
    return {
      status: 'loading', reason: '', available: false, usable: false,
      tone: TONES.loading, labelKey: LABEL_KEYS.loading, reasonKey: '',
    }
  }
  if (failed) {
    return {
      status: 'error', reason: '', available: false, usable: false,
      tone: TONES.error, labelKey: LABEL_KEYS.error, reasonKey: '',
    }
  }
  const footprintOk = footprintIsAvailable(entity)
  const status = modelStatusOf(entity)
  const reason = modelReasonOf(entity)
  if (!footprintOk) {
    // 封装本身不可用（或还没检查过）：3D 状态无法确认 —— 连文案也不能像「模型正常」
    // （只看 usable/tone 不够：Inspector/Settings 会拼 labelKey 给用户看）
    return {
      status,
      reason,
      available: false,
      usable: false,
      tone: footprintStatusOf(entity) === 'unknown' ? 'unknown' : 'bad',
      labelKey: 'footprintStatus.model.footprintUnavailable',
      reasonKey: '',
    }
  }
  const available = status === 'available'
  return {
    status,
    reason,
    available,
    usable: available,
    tone: available ? 'ok' : (status === 'unknown' ? 'unknown' : 'warn'),
    labelKey: status === 'unknown' ? LABEL_KEYS.unknown : `footprintStatus.model.${status}`,
    reasonKey: reason ? `footprintStatus.modelReason.${reason}` : '',
  }
}

/// 问题筛选：封装不可用（含未检查）或没有可用 3D —— 只看状态，不看旧字段
export function isProblemSymbol(entity) {
  return !footprintIsAvailable(entity) || !modelIsUsable(entity)
}
