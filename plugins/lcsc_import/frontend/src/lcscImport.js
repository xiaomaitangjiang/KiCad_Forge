// 立创导入专用流程的纯逻辑：编号规范化、请求会话/版本、目标元件库校验、
// 候选身份、提交体构造。组件只做渲染与事件，判定逻辑放在这里以便直接用
// Node 测试驱动（乱序、防重、重试、快照失效都不依赖 DOM）。
//
// 复用 footprintChoice 的 UUID 语义：复用身份是封装 UUID，库名/名称只用于显示。

import { reuseChoice, withFootprintChoice } from './footprintChoice.js'
import { footprintIsAvailable } from './footprintStatus.js'

// ---- 编号规范化：与插件/宿主的 normalize_lcsc_id 同规则 ----
// 去首尾空白、统一大写、纯数字补 C；只接受 C+数字。
// reason: 'empty' | 'format' —— 文案由界面本地化，helper 不产出用户可见文本。
export function normalizeLcscId(value) {
  if (typeof value !== 'string') return { ok: false, reason: 'format' }
  const text = value.trim().toUpperCase()
  if (!text) return { ok: false, reason: 'empty' }
  if (/^[0-9]+$/.test(text)) return { ok: true, value: `C${text}` }
  if (/^C[0-9]+$/.test(text)) return { ok: true, value: text }
  return { ok: false, reason: 'format' }
}

// ---- 请求会话/版本校验 ----
// begin() 每次都会推进版本：新查找自动作废上一次的响应与 finally。
// invalidate() 换会话：关闭弹窗、修改编号后，所有在途响应都不再属于当前状态。
export function createRequestGuard() {
  let session = 0
  let version = 0
  return {
    begin() {
      version += 1
      return { session, version }
    },
    invalidate() {
      session += 1
      version = 0
    },
    isCurrent(token) {
      return !!token && token.session === session && token.version === version
    },
  }
}

// ---- 宿主插件返回的包装形状 ----
// api.executePlugin 返回 {ok, error?, plugin_result:{ok, ...}}：外层与内层
// 任一失败都算失败，具体原因优先用内层的。
export function pluginOutcome(wrapper) {
  if (!wrapper || typeof wrapper !== 'object')
    return { ok: false, error: 'Invalid plugin response' }
  const inner = wrapper.plugin_result && typeof wrapper.plugin_result === 'object'
    ? wrapper.plugin_result
    : null
  // 转换失败的真实包装是 {ok:false, plugin_result:{ok:false, error:"..."}}：
  // 外层只有 ok=false，没有顶层 error —— 先取内层的具体原因，别退化成一个
  // 通用的 “Plugin request failed”。
  const innerError = inner && typeof inner.error === 'string' && inner.error.trim()
    ? inner.error
    : ''
  if (!wrapper.ok) return { ok: false, error: innerError || wrapper.error || 'Plugin request failed' }
  if (inner && inner.ok === false) return { ok: false, error: innerError || wrapper.error || 'Plugin failed' }
  return { ok: true, data: inner || wrapper }
}

/// 候选/搜索结果清洗：宿主返回的数组里可能混入 null、数字等损坏元素。
/// 合法的全部保留，损坏的计数返回给界面提示，绝不让一个坏元素把弹窗打崩。
export function sanitizeCandidates(value) {
  if (!Array.isArray(value)) return { ok: false, items: [], dropped: 0 }
  const items = value.filter(c => !!c && typeof c === 'object' && !Array.isArray(c))
  return { ok: true, items, dropped: value.length - items.length }
}

/// analyze 响应 → 界面用的器件信息（candidates 全部保留，不做筛选/排序）
export function parseAnalyzeResult(wrapper) {
  const outcome = pluginOutcome(wrapper)
  if (!outcome.ok) return outcome
  const d = outcome.data
  if (!d || typeof d !== 'object') return { ok: false, error: 'Invalid analyze response' }
  const sanitized = sanitizeCandidates(d.candidates)
  if (!sanitized.ok && typeof d.footprint_name !== 'string')
    return { ok: false, error: 'Invalid analyze response' }
  if (!sanitized.ok && !sanitized.items.length && sanitized.dropped === 0 && !Array.isArray(d.candidates))
    return { ok: false, error: 'Invalid analyze response' }
  return {
    ok: true,
    data: {
      id: typeof d.lcsc_id === 'string' ? d.lcsc_id : '',
      symbolNames: Array.isArray(d.symbol_names)
        ? d.symbol_names.filter(s => typeof s === 'string' && s)
        : [],
      footprintName: typeof d.footprint_name === 'string' ? d.footprint_name : '',
      modelName: typeof d.model_name === 'string' ? d.model_name : '',
      candidates: sanitized.items,
      droppedCandidates: sanitized.dropped,
      warning: typeof d.warning === 'string' ? d.warning : '',
    },
  }
}

// ---- 目标元件库（包）校验 ----
// 锁定/保护/停用/缺符号或封装路径的包不能写：前端先禁用并给出原因，
// 宿主提交时还会再复核一次。
// 字段与 GET /api/component-libraries 的实际返回一一对应（id/name/symbol_path/
// footprint_path/enabled/locked/is_protected），没有额外的 kind/type 概念：
// 宿主只认 package id，前端不猜。空字符串路径按缺失处理。
export function validateTargetLibrary(lib) {
  if (!lib || typeof lib !== 'object' || !lib.id) return { ok: false, reason: 'missing' }
  if (lib.enabled === false) return { ok: false, reason: 'disabled' }
  if (lib.locked) return { ok: false, reason: 'locked' }
  if (lib.is_protected) return { ok: false, reason: 'protected' }
  const symbolPath = typeof lib.symbol_path === 'string' ? lib.symbol_path.trim() : ''
  const footprintPath = typeof lib.footprint_path === 'string' ? lib.footprint_path.trim() : ''
  if (!symbolPath || !footprintPath) return { ok: false, reason: 'paths' }
  return { ok: true, reason: '' }
}

/// 提交摘要用的目标快照 —— 提交前拿它跟刷新结果比，任何一个字段变了都必须
/// 停下重新确认（不能拿旧摘要去写新路径）。
export function targetSnapshot(lib) {
  return {
    id: lib.id,
    name: typeof lib.name === 'string' ? lib.name : '',
    symbolPath: typeof lib.symbol_path === 'string' ? lib.symbol_path : '',
    footprintPath: typeof lib.footprint_path === 'string' ? lib.footprint_path : '',
    enabled: lib.enabled !== false,
    locked: !!lib.locked,
    isProtected: !!lib.is_protected,
  }
}

export function sameTarget(a, b) {
  if (!a || !b) return false
  return a.id === b.id && a.name === b.name &&
    a.symbolPath === b.symbolPath && a.footprintPath === b.footprintPath &&
    a.enabled === b.enabled && a.locked === b.locked && a.isProtected === b.isProtected
}

// 符号子库以相对文件路径定位；数据库中尚无元件的文件同样是可选落点。
function validSymbolLibraryPath(value) {
  return typeof value === 'string' && value.length > 0 &&
    !/[\\:]/.test(value) && ![...value].some(c => c.codePointAt(0) < 32 || c.codePointAt(0) === 127) && !value.startsWith('/') &&
    value.split('/').every(part => part && part !== '.' && part !== '..') &&
    /\.kicad_sym$/i.test(value)
}

export function parseSymbolLibraries(value) {
  if (!value || value.ok !== true || !Array.isArray(value.libraries) ||
    typeof value.can_create !== 'boolean') return { ok: false, reason: 'badResponse' }
  const seen = new Set()
  for (const library of value.libraries) {
    if (!library || typeof library.name !== 'string' || !library.name ||
      !validSymbolLibraryPath(library.path) || seen.has(library.path))
      return { ok: false, reason: 'badResponse' }
    seen.add(library.path)
  }
  return { ok: true, libraries: value.libraries, canCreate: value.can_create,
    ...(typeof value.case_sensitive === 'boolean' ? { caseSensitive: value.case_sensitive } : {}) }
}

export function normalizeSymbolLibraryName(value) {
  if (typeof value !== 'string') return { ok: false, reason: 'invalidName' }
  let name = value.trim()
  if (/\.kicad_sym$/i.test(name)) name = name.slice(0, -10)
  if (!name || name === '.' || name === '..' || /[<>:"/\\|?*]/.test(name) ||
    [...name].some(c => c.codePointAt(0) < 32 || c.codePointAt(0) === 127) ||
    /[. ]$/.test(name) || /\.kicad_sym$/i.test(name) || new TextEncoder().encode(name).length + 10 > 255 ||
    /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name))
    return { ok: false, reason: 'invalidName' }
  return { ok: true, name }
}

export function symbolLibraryPath(choice) {
  return choice?.mode === 'existing' ? choice.path
    : choice?.mode === 'new' ? `${choice.name}.kicad_sym` : ''
}

export function validateSymbolLibraryChoice(choice, catalog, { replay = false } = {}) {
  if (!catalog?.ok) return { ok: false, reason: 'unread' }
  if (choice?.mode === 'existing') return catalog.libraries.some(lib => lib.path === choice.path)
    ? { ok: true, choice: { mode: 'existing', path: choice.path } } : { ok: false, reason: 'gone' }
  if (choice?.mode !== 'new') return { ok: false, reason: 'pick' }
  if (!catalog.canCreate) return { ok: false, reason: 'cannotCreate' }
  const normalized = normalizeSymbolLibraryName(choice.name)
  if (!normalized.ok) return normalized
  const normalizedChoice = { mode: 'new', name: normalized.name }
  const desiredPath = symbolLibraryPath(normalizedChoice)
  if (!replay && catalog.libraries.some(lib => catalog.caseSensitive === false
    ? lib.path.toLowerCase() === desiredPath.toLowerCase() : lib.path === desiredPath))
    return { ok: false, reason: 'exists' }
  return { ok: true, choice: normalizedChoice }
}

// ---- 候选身份 ----
// UUID 才是身份：没有 UUID 的候选不能被选中（界面上禁用），也绝不回退到
// library/name —— 名字不是身份，宿主也会拒绝。
export function candidateIdentity(candidate) {
  if (!candidate || typeof candidate !== 'object') return ''
  if (typeof candidate.id === 'string' && candidate.id) return candidate.id
  if (typeof candidate.footprint_id === 'string' && candidate.footprint_id) return candidate.footprint_id
  return ''
}

/// 候选的封装是否真的还在（服务端状态说了算）：未知/缺失状态=未检查，不算可用
export function candidateAvailable(candidate) {
  return footprintIsAvailable(candidate)
}

export function candidateSelectable(candidate) {
  // 身份 + 存在性：UUID 缺失或封装文件失效/未检查都不能选
  return candidateIdentity(candidate) !== '' && candidateAvailable(candidate)
}

/// 候选 → 复用选择；缺 UUID 返回 null（调用方据此禁用该项）
/// 身份统一走 candidateIdentity：宿主/analyze 可能把 UUID 放在 footprint_id，
/// 只读 candidate.id 会让同一候选「显示可选、生成的选择却没有 id」。
export function candidateChoice(candidate) {
  const id = candidateIdentity(candidate)
  if (!id) return null
  const choice = reuseChoice({ ...candidate, id })
  return choice.id === id ? choice : null
}

// ---- 提交体 ----
// 元件库包用独立字段 target_component_library_id；绝不带 target_library(_id)
// —— 旧字段是符号库语义，混进来会被宿主按「两种模式同时给」拒绝。
// reuse 带顶层 footprint_id，new 会清掉任何旧 UUID（withFootprintChoice）。
export function buildImportBody({ lcscId, targetComponentLibraryId, symbolLibraryChoice, choice, operationId }) {
  const body = {
    action: 'import',
    lcsc_id: lcscId,
    target_component_library_id: targetComponentLibraryId,
  }
  if (operationId) body.operation_id = operationId
  if (symbolLibraryChoice) body.symbol_library_choice = { ...symbolLibraryChoice }
  return withFootprintChoice(body, choice)
}

/// 一次确认意图的幂等键：字符集与长度符合宿主的 is_safe_operation_id
export function newOperationId() {
  const raw = globalThis.crypto?.randomUUID?.() ||
    `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`
  return `lcsc-${raw}`.slice(0, 64)
}

// ---- 导入后的刷新结果校验 ----
// App 里三个刷新回调就是 useRequest(...).load（符号走 createSymbolRefresher 包一层，
// 原样返回 load 的结果）：成功返回本次原始结果，失败返回 null（useData.js 的契约）。
// 因此 null / 形状不符都必须算刷新失败 —— 把 null 当成功会让弹窗在列表仍旧陈旧时
// 关闭。kind: 'symbols' | 'libraries' | 'status'，界面据此显示对应原因。
export function refreshOutcome(kind, value) {
  if (kind === 'status') {
    const ok = !!value && typeof value === 'object' && !Array.isArray(value)
    return { ok, reason: ok ? '' : 'status' }
  }
  const ok = Array.isArray(value)
  return { ok, reason: ok ? '' : kind }
}

/// 相似度：0-1 小数 → 百分比；已是百分数或非数字原样处理
export function formatScore(score) {
  const n = Number(score)
  if (!Number.isFinite(n)) return ''
  return `${Math.round(n > 1 ? n : n * 100)}%`
}
