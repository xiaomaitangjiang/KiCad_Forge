// 元件类型展示元数据（名称 / 图标 / 颜色）的唯一来源。
//
// 身份一律用 type_id（稳定 ID，落库的就是它）；展示名只用于显示 ——
// 重命名、重排、改图标都不改变筛选与已存数据的含义。
// 这里只做纯函数：索引、取值、输入校验、补丁构造，不碰网络与 React。

/// 没有图标时的共享通用占位（不是「某个内置类型的图标」）
export const TYPE_ICON_PLACEHOLDER = '○'
/// 图标是短文本：上限按 UTF-8 字节算（一个 emoji 可能占 4 字节以上）
export const TYPE_ICON_MAX_BYTES = 64

const HEX_COLOR = /^#[0-9a-fA-F]{6}$/

function text(value) {
  return typeof value === 'string' ? value : ''
}

/// 类型表 → id 索引。只认有 id 的条目：没有身份的行不能参与图标/名称查表。
export function buildTypeMap(list) {
  const map = new Map()
  for (const entry of Array.isArray(list) ? list : []) {
    if (!entry || typeof entry !== 'object') continue
    const id = text(entry.id)
    if (!id) continue
    map.set(id, {
      id,
      name: text(entry.name),
      icon: text(entry.icon),
      color: text(entry.color),
    })
  }
  return map
}

export function typeMetaOf(map, id) {
  if (!map || !id) return null
  return map.get(id) || null
}

/// 图标：没有配置就是通用占位，绝不按内置类型名字猜一张表
export function typeIconOf(map, id) {
  const meta = typeMetaOf(map, id)
  return (meta && meta.icon) || TYPE_ICON_PLACEHOLDER
}

/// 颜色：空串 = 沿用默认文字颜色（调用方负责不要写死颜色）
export function typeColorOf(map, id) {
  const meta = typeMetaOf(map, id)
  return (meta && meta.color) || ''
}

/// 展示名：登记的用登记名；没有登记的保留服务端给的展示名，
/// 再没有就原样显示 ID —— 认不出的历史 ID 不改成 Unknown。
export function typeLabelOf(map, id, display) {
  const meta = typeMetaOf(map, id)
  if (meta && meta.name) return meta.name
  return text(display) || text(id)
}

// ---------------- 输入校验（与后端同一套规则，前端只做即时提示） ----------------

/// UTF-8 字节数：图标限制按字节而不是 UTF-16 code unit 算
export function iconByteLength(value) {
  const raw = text(value)
  if (typeof TextEncoder === 'function') return new TextEncoder().encode(raw).length
  let bytes = 0
  for (const ch of raw) {
    const code = ch.codePointAt(0)
    bytes += code <= 0x7f ? 1 : code <= 0x7ff ? 2 : code <= 0xffff ? 3 : 4
  }
  return bytes
}

/// 返回错误文案（null = 合法）。空串合法 = 清空图标。
export function validateIconInput(value) {
  const bytes = iconByteLength(value)
  if (bytes > TYPE_ICON_MAX_BYTES) return `icon too long: ${bytes} bytes > ${TYPE_ICON_MAX_BYTES}`
  return null
}

/// 颜色规范形：'' = 无颜色；合法 #RRGGBB 统一小写；其它一律 null（不合法）。
/// 历史配置里可能存在非该格式的颜色，调用方要原样保留、不要改写。
export function normalizeColor(value) {
  const raw = text(value)
  if (!raw) return ''
  return HEX_COLOR.test(raw) ? raw.toLowerCase() : null
}

/// 返回错误文案（null = 合法）。空串合法 = 清除颜色。
export function validateColorInput(value) {
  return normalizeColor(value) === null ? `invalid color: ${text(value)}` : null
}

/// 编辑器草案相对条目**真正变化**的字段：按原文比较，不先归一化。
/// 只有出现在结果里的字段才需要按新写入策略校验 —— 历史配置里的旧颜色
/// 或超长图标没动，就不该挡住一次改名，也不该被顺手改写。
export function changedTypeFields(entry, draft) {
  const base = entry || {}
  const next = draft || {}
  const changed = {}
  const name = text(next.name).trim()
  if (name !== text(base.name)) changed.name = name
  const icon = text(next.icon)
  if (icon !== text(base.icon)) changed.icon = icon
  const color = text(next.color)
  if (color !== text(base.color)) changed.color = color
  return changed
}

/// 读取结果的整份校验：只有整份通过才能发布 ——
/// 坏行（null / id 不是非空字符串 / 展示字段类型不对 / 重复 id）不能顶掉
/// 上一份有效快照，也不能让列表渲染踩到 null。
/// 历史 icon/color 字符串（长或非 hex）属于合法旧配置，这里不套新写入策略；
/// 应用层 `{ ok:false, error }` 保留服务端给出的真实原因。
export function parseTypeList(data) {
  if (!Array.isArray(data)) {
    const reason = data && typeof data === 'object' && typeof data.error === 'string' ? data.error : ''
    return { ok: false, error: reason || 'Invalid component type list' }
  }
  const seen = new Set()
  const list = []
  for (let i = 0; i < data.length; i += 1) {
    const entry = data[i]
    if (!entry || typeof entry !== 'object' || Array.isArray(entry))
      return { ok: false, error: `Invalid component type entry at #${i}` }
    if (typeof entry.id !== 'string' || !entry.id)
      return { ok: false, error: `Invalid component type id at #${i}` }
    for (const key of ['name', 'icon', 'color']) {
      if (entry[key] !== undefined && entry[key] !== null && typeof entry[key] !== 'string')
        return { ok: false, error: `Invalid component type field "${key}" at #${i}` }
    }
    if (seen.has(entry.id))
      return { ok: false, error: `Duplicate component type id: ${entry.id}` }
    seen.add(entry.id)
    list.push(entry)
  }
  return { ok: true, list }
}

/// 编辑器草案 → 提交补丁：只带真正变化的字段（省略 = 服务端保持原值）。
/// 名称 trim 后比较；图标/颜色按原样比较，不参与 trim。
/// 历史颜色不是合法 #RRGGBB 时：没动就不提交（改名不能顺手改色），
/// 显式清空或换成合法颜色则照常提交。
export function buildTypePatch(entry, draft) {
  const patch = { action: 'update', id: text(entry && entry.id) }
  const base = entry || {}
  const next = draft || {}
  const name = text(next.name).trim()
  if (name !== text(base.name)) patch.name = name
  const icon = text(next.icon)
  if (icon !== text(base.icon)) patch.icon = icon
  const color = normalizeColor(next.color)
  if (color !== null) {
    const current = normalizeColor(base.color)
    const unchanged = current !== null
      ? color === current
      : color === text(base.color)   // 历史值按原文比：没动就不提交，显式清空/换色才算变
    if (!unchanged) patch.color = color
  }
  return patch
}

/// 补丁里除了身份/动作之外是否真的有字段变化
export function patchHasChanges(patch) {
  return !!patch && (patch.name !== undefined || patch.icon !== undefined || patch.color !== undefined)
}

/// 新增请求体：带 name/icon/color（空串 = 该字段为空），身份由后端生成
export function buildAddPayload(draft) {
  const next = draft || {}
  return {
    action: 'add',
    name: text(next.name).trim(),
    icon: text(next.icon),
    color: normalizeColor(next.color) || '',
  }
}
