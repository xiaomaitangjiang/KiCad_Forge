// 只校验传输结构与输入；匹配和正则编译始终交给宿主。
export const MATCH_FIELDS = ['name', 'value', 'model', 'reference']
export const FIELD_LABELS = { name: '名称', value: '值', model: '型号 / MPN', reference: 'Reference' }
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value)
const revision = value => Number.isSafeInteger(value) && value >= 0
const bytes = value => new TextEncoder().encode(value).length

// 消息只存稳定键与插值；渲染时翻译，不把语言绑定到请求回调或草稿会话。
const ERROR_TEXT = {
  invalidConfig: '匹配配置格式无效', tooManyEntries: '每类规则最多 500 条',
  invalidEntry: '匹配条目格式无效或 ID 重复', invalidRule: '正则模式或优先级无效（优先级须为 int32 整数）',
  invalidException: '精确特例原文须为 1–4096 字节', invalidRead: '匹配配置读取结果无效',
  invalidHealth: '匹配配置健康状态无效', noConfig: '尚无可用配置，请修复配置后重新加载',
}
export const matchingMessage = (key, params = {}) => ({ key: `matching.${key}`, params })
export function translateMatchingMessage(value, t) {
  return value && typeof value === 'object' && typeof value.key === 'string'
    ? t(value.key, value.params) : value || ''
}
export function matchingError(key, params = {}) {
  const error = new Error(ERROR_TEXT[key] || key)
  error.matchingMessage = matchingMessage(`errors.${key}`, params)
  return error
}
export const matchingErrorMessage = cause => cause?.matchingMessage
  || (cause?.kind === 'network' ? matchingMessage('errors.network')
    : cause?.kind === 'parse' ? matchingMessage('errors.invalidJson') : cause?.message || String(cause))

export function configFrom(value) {
  if (!object(value) || value.schema_version !== 1 || !revision(value.revision)
      || !Array.isArray(value.rules) || !Array.isArray(value.exceptions)) throw matchingError('invalidConfig')
  const ids = new Set()
  for (const [kind, rows] of [['rules', value.rules], ['exceptions', value.exceptions]]) {
    if (rows.length > 500) throw matchingError('tooManyEntries')
    for (const row of rows) {
      if (!object(row) || typeof row.id !== 'string' || !row.id || ids.has(row.id)
          || !MATCH_FIELDS.includes(row.field) || typeof row.enabled !== 'boolean'
          || typeof row.type_id !== 'string' || !row.type_id) throw matchingError('invalidEntry')
      ids.add(row.id)
      if (kind === 'rules') {
        if (typeof row.pattern !== 'string' || bytes(row.pattern) > 512
            || typeof row.case_sensitive !== 'boolean' || !Number.isInteger(row.priority)
            || row.priority < -2147483648 || row.priority > 2147483647) throw matchingError('invalidRule')
      } else if (typeof row.key !== 'string' || !row.key || bytes(row.key) > 4096) {
        throw matchingError('invalidException')
      }
    }
  }
  // 仅传正式配置字段，GET 的状态和能力字段不混入 POST。
  return { schema_version: 1, revision: value.revision,
    rules: value.rules.map(({ id, field, pattern, priority, case_sensitive, enabled, type_id }) =>
      ({ id, field, pattern, priority, case_sensitive, enabled, type_id })),
    exceptions: value.exceptions.map(({ id, field, key, enabled, type_id }) => ({ id, field, key, enabled, type_id })) }
}

export function parseMatchingRead(value) {
  if (!object(value) || typeof value.ok !== 'boolean' || typeof value.has_snapshot !== 'boolean'
      || typeof value.load_error !== 'string') throw (value?.error ? new Error(value.error) : matchingError('invalidRead'))
  const config = value.has_snapshot ? configFrom(value) : null
  if (value.ok && (!config || value.load_error)) throw matchingError('invalidHealth')
  return { config, healthy: value.ok && !!config && !value.load_error,
    error: value.load_error || value.error || (!config ? '尚无可用配置，请修复配置后重新加载' : ''),
    errorMessage: !value.load_error && !value.error && !config ? matchingMessage('errors.noConfig') : null,
    engine: value.engine, limits: value.limits }
}

export function sameMatchingContent(a, b) {
  const content = value => ({
    rules: value.rules.map(({ id, field, pattern, priority, case_sensitive, enabled, type_id }) =>
      [id, field, pattern, priority, case_sensitive, enabled, type_id]),
    exceptions: value.exceptions.map(({ id, field, key, enabled, type_id }) => [id, field, key, enabled, type_id]),
  })
  return !!a && !!b && JSON.stringify(content(a)) === JSON.stringify(content(b))
}

export function emptySample() {
  return Object.fromEntries(MATCH_FIELDS.map(field => [field, { provided: false, text: '' }]))
}

export function sampleBody(sample) {
  return Object.fromEntries(MATCH_FIELDS.filter(field => sample[field]?.provided)
    .map(field => [field, sample[field].text]))
}

export function newMatchingEntry(kind) {
  const common = { id: crypto.randomUUID(), field: 'name', type_id: '', enabled: true }
  return kind === 'rules' ? { ...common, pattern: '', priority: 0, case_sensitive: false } : { ...common, key: '' }
}

export function validateMatchingDraft(draft, typeMap, messages = false) {
  try {
    const config = configFrom(draft)
    if (typeMap) {
      const missing = [...config.rules, ...config.exceptions].find(entry => !typeMap.has(entry.type_id))
      if (missing) return messages ? matchingMessage('errors.missingType', { id: missing.type_id }) : `目标类型不存在：${missing.type_id}`
    }
    return ''
  } catch (cause) { return messages ? matchingErrorMessage(cause) : cause.message }
}

const COUNTS = ['total', 'protected_count', 'matched', 'unmatched', 'conflicts', 'input_too_long',
  'evaluation_limited', 'evaluation_errors', 'would_update', 'unchanged', 'type_updated', 'source_changed',
  'cas_skipped', 'deleted', 'rolled_back', 'not_attempted']
function matchingEvidence(value) {
  return Array.isArray(value) && value.every(item => object(item)
    && ['rule', 'exception'].includes(item.source) && typeof item.id === 'string' && !!item.id
    && MATCH_FIELDS.includes(item.field) && typeof item.text === 'string'
    && Number.isInteger(item.priority) && item.priority >= -2147483648 && item.priority <= 2147483647
    && typeof item.type_id === 'string' && !!item.type_id)
}

export function isMatchingSamplePreview(value) {
  return object(value) && value.ok === true && value.mode === 'sample'
    && ['draft', 'saved'].includes(value.source) && revision(value.revision)
    && object(value.sample) && Object.entries(value.sample).every(([field, text]) => MATCH_FIELDS.includes(field) && typeof text === 'string')
    && Object.prototype.hasOwnProperty.call(MATCH_STATUS_LABELS, value.status)
    && typeof value.type_id === 'string' && typeof value.reason === 'string'
    && matchingEvidence(value.structured_evidence) && typeof value.load_error === 'string'
    && typeof value.saved_healthy === 'boolean'
}

export function isMatchingReport(value) {
  return object(value) && typeof value.ok === 'boolean' && object(value.summary)
    && revision(value.summary.revision) && COUNTS.every(key => revision(value.summary[key]))
    && Array.isArray(value.details) && value.details.length <= 100
    && value.details.every(item => object(item) && typeof item.status === 'string'
      && typeof item.reason === 'string' && typeof item.name === 'string'
      && matchingEvidence(item.structured_evidence))
}

// 所有必要读取都实际发布，才可宣布界面同步完成；拒绝/坏回报不能视为成功。
export function committedRefreshOutcome(results, sources) {
  const details = results.map((result, index) => {
    const value = result.status === 'fulfilled' ? result.value : null
    const status = ['published', 'superseded', 'failed'].includes(value?.status) ? value.status : 'failed'
    return { source: sources[index], status,
      error: result.status === 'rejected' ? String(result.reason?.message || result.reason)
        : String(value?.error?.message || value?.error || (status === 'failed' ? '读取未能发布' : '')),
      ...(status === 'failed' && result.status !== 'rejected' && !value?.error
        ? { errorMessage: matchingMessage('errors.unpublished') } : {}) }
  })
  return { status: details.every(item => item.status === 'published') ? 'published'
    : details.some(item => item.status === 'failed') ? 'failed' : 'superseded', details }
}

export const MATCH_STATUS_LABELS = { matched: '命中', unmatched: '未匹配', conflict: '类型冲突',
  input_too_long: '源字段超长', evaluation_limit: '匹配工作量超限', evaluation_error: '匹配引擎错误',
  source_changed: '源数据已变化', protected: '已分类，受保护' }
