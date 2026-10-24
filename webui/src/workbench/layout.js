// 工作台布局是纯数据：只描述区域尺寸/显隐/活动视图/中心分组，不持有 ReactNode 或业务状态。
// 读写都经过这里，业务组件不直接拼 JSON，避免坏配置扩散到渲染层。

import { createCenterNavigation, parseCenterState, serializeCenterState } from './useCenterNavigation.js'

export const WORKBENCH_LAYOUT_KEY = 'workbench.layout.v1'
export const WORKBENCH_LAYOUT_VERSION = 2
export const WORKBENCH_LAYOUT_LEGACY_VERSIONS = Object.freeze([1])
export const WORKBENCH_WORKSPACE_VIEW = 'kf:workspace'
export const WORKBENCH_ACTIVITY_WIDTH = 48
export const WORKBENCH_SEPARATOR_WIDTH = 4
export const WORKBENCH_CENTER_MIN_WIDTH = 360
export const WORKBENCH_CENTER_MIN_HEIGHT = 240
export const WORKBENCH_BOTTOM_MIN_HEIGHT = 440

export const WORKBENCH_DEFAULT_SIZES = Object.freeze({ primary: 280, auxiliary: 280, bottom: 220 })

export const WORKBENCH_LIMITS = Object.freeze({
  primary: Object.freeze({ min: 200, max: 480 }),
  auxiliary: Object.freeze({ min: 220, max: 520 }),
  bottom: Object.freeze({ min: 120, max: 420 }),
})

const MAX_SERIALIZED_LENGTH = 64 * 1024
const MAX_VIEW_ID_LENGTH = 128

function isRecord(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value))
}

function clampWidth(panel, value, fallback) {
  const limit = WORKBENCH_LIMITS[panel]
  return Number.isFinite(value) ? Math.round(clamp(value, limit.min, limit.max)) : fallback
}

function sanitizeViewId(value, fallback, allowNull = false) {
  if (allowNull && value === null) return null
  if (typeof value === 'string') {
    const trimmed = value.trim()
    if (trimmed && trimmed.length <= MAX_VIEW_ID_LENGTH) return trimmed
  }
  return fallback
}

// O1: the bottom panel no longer has an output tab. Old snapshots are migrated
// on read and can never be written back, while other unknown ids stay untouched.
function normalizeBottomViewId(value, fallback) {
  const id = sanitizeViewId(value, fallback)
  return id === 'output' ? 'tasks' : id
}

export function createDefaultWorkbenchLayout() {
  return {
    version: WORKBENCH_LAYOUT_VERSION,
    primarySidebar: { visible: true, width: 280, activeViewId: WORKBENCH_WORKSPACE_VIEW },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
    center: serializeCenterState(createCenterNavigation()),
  }
}

function readBoolean(source, key, fallback, path, issues) {
  if (typeof source[key] === 'boolean') return source[key]
  if (source[key] !== undefined) issues.push(path)
  return fallback
}

function readDimension(source, key, panel, fallback, path, issues) {
  const raw = source[key]
  if (Number.isFinite(raw)) return clampWidth(panel, raw, fallback)
  if (raw !== undefined) issues.push(path)
  return fallback
}

function readViewId(source, key, fallback, path, issues, allowNull = false) {
  const raw = source[key]
  if (raw === null && allowNull) return null
  if (typeof raw === 'string') {
    const trimmed = raw.trim()
    if (trimmed && trimmed.length <= MAX_VIEW_ID_LENGTH) return trimmed
  }
  if (raw !== undefined) issues.push(path)
  return fallback
}

/**
 * 解析宿主配置里的布局快照。
 * - 空值/空串：还没保存过，按默认布局，且不算损坏。
 * - 未知版本/坏 JSON/超长：整份回退，标记 recovered，交给 UI 提示与重试。
 * - 已知版本里个别字段坏：该字段回退，其余保留，也标记 recovered。
 */
export function parseWorkbenchLayout(raw) {
  const fallback = createDefaultWorkbenchLayout()
  if (raw === null || raw === undefined || raw === '') {
    return { layout: fallback, recovered: false }
  }

  let value = raw
  if (typeof raw === 'string') {
    if (!raw.trim()) return { layout: fallback, recovered: false }
    if (raw.length > MAX_SERIALIZED_LENGTH) return { layout: fallback, recovered: true }
    try {
      value = JSON.parse(raw)
    } catch {
      return { layout: fallback, recovered: true }
    }
  }

  if (!isRecord(value)) return { layout: fallback, recovered: true }
  const version = value.version
  const knownVersion = version === WORKBENCH_LAYOUT_VERSION || WORKBENCH_LAYOUT_LEGACY_VERSIONS.includes(version)
  if (!knownVersion) return { layout: fallback, recovered: true }

  const issues = []
  const primarySource = isRecord(value.primarySidebar) ? value.primarySidebar : {}
  const auxiliarySource = isRecord(value.auxiliarySidebar) ? value.auxiliarySidebar : {}
  const bottomSource = isRecord(value.bottomPanel) ? value.bottomPanel : {}
  if (!isRecord(value.primarySidebar)) issues.push('primarySidebar')
  if (!isRecord(value.auxiliarySidebar)) issues.push('auxiliarySidebar')
  if (!isRecord(value.bottomPanel)) issues.push('bottomPanel')

  const primarySidebar = {
    visible: readBoolean(primarySource, 'visible', fallback.primarySidebar.visible, 'primarySidebar.visible', issues),
    width: readDimension(primarySource, 'width', 'primary', fallback.primarySidebar.width, 'primarySidebar.width', issues),
    activeViewId: readViewId(primarySource, 'activeViewId', fallback.primarySidebar.activeViewId, 'primarySidebar.activeViewId', issues),
  }
  const auxiliarySidebar = {
    visible: readBoolean(auxiliarySource, 'visible', fallback.auxiliarySidebar.visible, 'auxiliarySidebar.visible', issues),
    width: readDimension(auxiliarySource, 'width', 'auxiliary', fallback.auxiliarySidebar.width, 'auxiliarySidebar.width', issues),
    activeViewId: readViewId(auxiliarySource, 'activeViewId', fallback.auxiliarySidebar.activeViewId, 'auxiliarySidebar.activeViewId', issues, true),
  }
  const bottomViewId = readViewId(bottomSource, 'activeViewId', fallback.bottomPanel.activeViewId, 'bottomPanel.activeViewId', issues)
  const bottomPanel = {
    visible: readBoolean(bottomSource, 'visible', fallback.bottomPanel.visible, 'bottomPanel.visible', issues),
    height: readDimension(bottomSource, 'height', 'bottom', fallback.bottomPanel.height, 'bottomPanel.height', issues),
    activeViewId: normalizeBottomViewId(bottomViewId, fallback.bottomPanel.activeViewId),
  }

  // v1 快照没有 center：迁移为默认中心分组，不算损坏；v2 起 center 是完整子快照。
  let center = fallback.center
  if (version >= 2) {
    if (value.center === undefined) {
      issues.push('center')
    } else {
      const parsedCenter = parseCenterState(value.center)
      center = serializeCenterState(parsedCenter.state)
      if (parsedCenter.recovered) issues.push('center')
    }
  }

  return {
    layout: { version: WORKBENCH_LAYOUT_VERSION, primarySidebar, auxiliarySidebar, bottomPanel, center },
    recovered: issues.length > 0,
  }
}

/** 写回配置前再夹一次，保证任何调用路径都写不出越界尺寸。 */
export function serializeWorkbenchLayout(layout) {
  const value = isRecord(layout) ? layout : {}
  const primary = isRecord(value.primarySidebar) ? value.primarySidebar : {}
  const auxiliary = isRecord(value.auxiliarySidebar) ? value.auxiliarySidebar : {}
  const bottom = isRecord(value.bottomPanel) ? value.bottomPanel : {}
  return JSON.stringify({
    version: WORKBENCH_LAYOUT_VERSION,
    primarySidebar: {
      visible: primary.visible !== false,
      width: clampWidth('primary', primary.width, 280),
      activeViewId: sanitizeViewId(primary.activeViewId, WORKBENCH_WORKSPACE_VIEW),
    },
    auxiliarySidebar: {
      visible: auxiliary.visible !== false,
      width: clampWidth('auxiliary', auxiliary.width, 280),
      activeViewId: sanitizeViewId(auxiliary.activeViewId, null, true),
    },
    bottomPanel: {
      visible: bottom.visible === true,
      height: clampWidth('bottom', bottom.height, 220),
      activeViewId: normalizeBottomViewId(bottom.activeViewId, 'tasks'),
    },
    center: serializeCenterState(parseCenterState(value.center).state),
  })
}

/**
 * 视口不足时只做临时收敛，不写回用户布局。
 * 收缩顺序：辅助侧栏 -> 主侧栏宽度 -> 主侧栏；活动栏与中心区最小宽度始终保留。
 * 高度不足时临时收起底部面板。
 */
export function resolveWorkbenchLayout(layout, viewportWidth, viewportHeight) {
  const source = isRecord(layout) ? layout : createDefaultWorkbenchLayout()
  const primarySource = isRecord(source.primarySidebar) ? source.primarySidebar : {}
  const auxiliarySource = isRecord(source.auxiliarySidebar) ? source.auxiliarySidebar : {}
  const bottomSource = isRecord(source.bottomPanel) ? source.bottomPanel : {}
  const width = Number.isFinite(viewportWidth) && viewportWidth > 0 ? viewportWidth : 1280
  const height = Number.isFinite(viewportHeight) && viewportHeight > 0 ? viewportHeight : 900
  const available = Math.max(0, width - WORKBENCH_ACTIVITY_WIDTH)

  let primaryVisible = primarySource.visible !== false
  let auxiliaryVisible = auxiliarySource.visible !== false
  let primaryWidth = clampWidth('primary', primarySource.width, 280)
  let auxiliaryWidth = clampWidth('auxiliary', auxiliarySource.width, 280)
  const bottomHeight = clampWidth('bottom', bottomSource.height, 220)
  const bottomVisible = bottomSource.visible === true && height >= WORKBENCH_BOTTOM_MIN_HEIGHT

  const sum = () => (primaryVisible ? primaryWidth : 0)
    + (auxiliaryVisible ? auxiliaryWidth : 0)
    + WORKBENCH_CENTER_MIN_WIDTH
    + (primaryVisible ? WORKBENCH_SEPARATOR_WIDTH : 0)
    + (auxiliaryVisible ? WORKBENCH_SEPARATOR_WIDTH : 0)

  if (primaryVisible && auxiliaryVisible && sum() > available) auxiliaryVisible = false

  if (primaryVisible && auxiliaryVisible === false && sum() > available) {
    const primaryRoom = available - WORKBENCH_CENTER_MIN_WIDTH - WORKBENCH_SEPARATOR_WIDTH
    if (primaryRoom < WORKBENCH_LIMITS.primary.min) primaryVisible = false
    else primaryWidth = Math.min(primaryWidth, primaryRoom)
  }

  if (primaryVisible === false && auxiliaryVisible && sum() > available) auxiliaryVisible = false

  return {
    primaryVisible,
    primaryWidth,
    auxiliaryVisible,
    auxiliaryWidth,
    bottomVisible,
    bottomHeight,
    centerMinWidth: WORKBENCH_CENTER_MIN_WIDTH,
    autoCollapsed: {
      primary: primarySource.visible !== false && primaryVisible === false,
      auxiliary: auxiliarySource.visible !== false && auxiliaryVisible === false,
      bottom: bottomSource.visible === true && bottomVisible === false,
    },
  }
}
