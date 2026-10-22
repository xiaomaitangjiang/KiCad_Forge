import test from 'node:test'
import assert from 'node:assert/strict'
import {
  WORKBENCH_DEFAULT_SIZES,
  WORKBENCH_LAYOUT_VERSION,
  WORKBENCH_WORKSPACE_VIEW,
  createDefaultWorkbenchLayout,
  parseWorkbenchLayout,
  resolveWorkbenchLayout,
  serializeWorkbenchLayout,
} from '../src/workbench/layout.js'

const defaultLayout = () => createDefaultWorkbenchLayout()

test('default workbench layout keeps all panels in documented ranges', () => {
  assert.deepEqual(defaultLayout(), {
    version: WORKBENCH_LAYOUT_VERSION,
    primarySidebar: { visible: true, width: WORKBENCH_DEFAULT_SIZES.primary, activeViewId: WORKBENCH_WORKSPACE_VIEW },
    auxiliarySidebar: { visible: true, width: WORKBENCH_DEFAULT_SIZES.auxiliary, activeViewId: null },
    bottomPanel: { visible: false, height: WORKBENCH_DEFAULT_SIZES.bottom, activeViewId: 'tasks' },
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [{ id: 'g1', pages: ['kf:symbols'], active: 'kf:symbols', size: 100 }],
    },
  })
})

test('parseWorkbenchLayout treats absent or blank snapshots as fresh defaults', () => {
  for (const raw of [null, undefined, '', '   ']) {
    const parsed = parseWorkbenchLayout(raw)
    assert.deepEqual(parsed.layout, defaultLayout())
    assert.equal(parsed.recovered, false)
  }
})

test('parseWorkbenchLayout falls back wholly for broken or unknown snapshots', () => {
  const broken = ['{', 'null', '[]', '{"version":2}', '{"version":3}', 'x'.repeat(64 * 1024 + 1)]
  for (const raw of broken) {
    const parsed = parseWorkbenchLayout(raw)
    assert.deepEqual(parsed.layout, defaultLayout(), raw)
    assert.equal(parsed.recovered, true, raw)
  }
})

test('parseWorkbenchLayout accepts a complete version 1 snapshot', () => {
  const raw = JSON.stringify({
    version: 1,
    primarySidebar: { visible: false, width: 333, activeViewId: 'kf:github' },
    auxiliarySidebar: { visible: false, width: 444, activeViewId: 'inspector:detail' },
    bottomPanel: { visible: true, height: 260, activeViewId: 'output' },
  })
  const parsed = parseWorkbenchLayout(raw)
  assert.deepEqual(parsed.layout, {
    version: 2,
    primarySidebar: { visible: false, width: 333, activeViewId: 'kf:github' },
    auxiliarySidebar: { visible: false, width: 444, activeViewId: 'inspector:detail' },
    bottomPanel: { visible: true, height: 260, activeViewId: 'tasks' },
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [{ id: 'g1', pages: ['kf:symbols'], active: 'kf:symbols', size: 100 }],
    },
  })
  assert.equal(parsed.recovered, false)
})

test('parse and serialize migrate the legacy output bottom view to tasks', () => {
  const parsed = parseWorkbenchLayout(JSON.stringify({
    version: 2,
    primarySidebar: { visible: true, width: 300, activeViewId: 'kf:github' },
    auxiliarySidebar: { visible: true, width: 320, activeViewId: 'inspector:detail' },
    bottomPanel: { visible: true, height: 260, activeViewId: ' output ' },
    center: defaultLayout().center,
  }))
  assert.equal(parsed.recovered, false)
  assert.deepEqual(parsed.layout.bottomPanel, { visible: true, height: 260, activeViewId: 'tasks' })
  assert.equal(parsed.layout.primarySidebar.activeViewId, 'kf:github')
  assert.equal(parsed.layout.auxiliarySidebar.activeViewId, 'inspector:detail')
  assert.deepEqual(parsed.layout.center, defaultLayout().center)

  const saved = serializeWorkbenchLayout(parsed.layout)
  assert.equal(saved.includes('output'), false)
  const savedLayout = JSON.parse(saved)
  assert.equal(savedLayout.bottomPanel.activeViewId, 'tasks')
  const reopened = parseWorkbenchLayout(saved)
  assert.equal(reopened.recovered, false)
  assert.deepEqual(reopened.layout, parsed.layout)
})

test('parseWorkbenchLayout round-trips a version 2 center split and rejects a broken tree', () => {
  const center = {
    activeGroupId: 'g2',
    nextId: 3,
    tree: {
      type: 'split',
      direction: 'row',
      size: 100,
      children: [
        { type: 'group', groupId: 'g1', size: 60 },
        { type: 'group', groupId: 'g2', size: 40 },
      ],
    },
    groups: [
      { id: 'g1', pages: ['kf:symbols', 'kf:settings'], active: 'kf:settings', size: 60 },
      { id: 'g2', pages: ['view:demo'], active: 'view:demo', size: 40 },
    ],
  }
  const base = {
    primarySidebar: { visible: true, width: 280, activeViewId: WORKBENCH_WORKSPACE_VIEW },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
  }
  const parsed = parseWorkbenchLayout(JSON.stringify({ version: 2, ...base, center }))
  assert.equal(parsed.recovered, false)
  assert.deepEqual(parsed.layout.center, center)

  const brokenCenter = JSON.parse(JSON.stringify(center))
  brokenCenter.tree.children[1].groupId = 'g9'
  const broken = parseWorkbenchLayout(JSON.stringify({ version: 2, ...base, center: brokenCenter }))
  assert.equal(broken.recovered, true)
  assert.deepEqual(broken.layout.center, defaultLayout().center)
})

test('parseWorkbenchLayout preserves a closed home in an otherwise valid center', () => {
  const center = {
    activeGroupId: 'g1',
    nextId: 2,
    tree: { type: 'group', groupId: 'g1', size: 100 },
    groups: [{ id: 'g1', pages: [], active: null, size: 100 }],
  }
  const base = {
    primarySidebar: { visible: true, width: 280, activeViewId: WORKBENCH_WORKSPACE_VIEW },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
  }
  const parsed = parseWorkbenchLayout(JSON.stringify({ version: 2, ...base, center }))
  assert.equal(parsed.recovered, false)
  assert.deepEqual(parsed.layout.center, center)
  assert.deepEqual(JSON.parse(serializeWorkbenchLayout(parsed.layout)).center, center)

  const reopened = parseWorkbenchLayout(serializeWorkbenchLayout(createDefaultWorkbenchLayout()))
  assert.equal(reopened.recovered, false)
  assert.deepEqual(reopened.layout.center, defaultLayout().center)
})

test('parseWorkbenchLayout clamps only the broken fields and reports recovery', () => {
  const parsed = parseWorkbenchLayout(JSON.stringify({
    version: 1,
    primarySidebar: { visible: 'yes', width: 900, activeViewId: '  ' },
    auxiliarySidebar: { visible: true, width: 10, activeViewId: 'a'.repeat(129) },
    bottomPanel: { visible: 'true', height: 999, activeViewId: 7 },
  }))
  assert.deepEqual(parsed.layout.primarySidebar, {
    visible: true,
    width: 480,
    activeViewId: WORKBENCH_WORKSPACE_VIEW,
  })
  assert.deepEqual(parsed.layout.auxiliarySidebar, {
    visible: true,
    width: 220,
    activeViewId: null,
  })
  assert.deepEqual(parsed.layout.bottomPanel, {
    visible: false,
    height: 420,
    activeViewId: 'tasks',
  })
  assert.equal(parsed.recovered, true)
})

test('parseWorkbenchLayout accepts null auxiliary view and trims ids', () => {
  const parsed = parseWorkbenchLayout(JSON.stringify({
    version: 1,
    primarySidebar: { activeViewId: '  kf:git  ' },
    auxiliarySidebar: { activeViewId: null },
    bottomPanel: {},
  }))
  assert.equal(parsed.layout.primarySidebar.activeViewId, 'kf:git')
  assert.equal(parsed.layout.auxiliarySidebar.activeViewId, null)
  assert.equal(parsed.recovered, false)
})

test('serializeWorkbenchLayout clamps every dimension and coerces flags', () => {
  const parsed = JSON.parse(serializeWorkbenchLayout({
    primarySidebar: { visible: 0, width: 1, activeViewId: '   ' },
    auxiliarySidebar: { visible: undefined, width: 9999, activeViewId: null },
    bottomPanel: { visible: 'yes', width: 12, height: -1, activeViewId: ' output ' },
  }))
  assert.deepEqual(parsed, {
    version: 2,
    primarySidebar: { visible: true, width: 200, activeViewId: WORKBENCH_WORKSPACE_VIEW },
    auxiliarySidebar: { visible: true, width: 520, activeViewId: null },
    bottomPanel: { visible: false, height: 120, activeViewId: 'tasks' },
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [{ id: 'g1', pages: ['kf:symbols'], active: 'kf:symbols', size: 100 }],
    },
  })
  assert.equal(serializeWorkbenchLayout(null).includes('"version":2'), true)
  assert.ok(serializeWorkbenchLayout(null).length <= 64 * 1024)
})

test('resolveWorkbenchLayout keeps 280px sidebars and a 360px center at desktop sizes', () => {
  for (const [width, height] of [[1920, 1080], [1440, 900], [1280, 900]]) {
    const resolved = resolveWorkbenchLayout(defaultLayout(), width, height)
    assert.equal(resolved.primaryVisible, true, String(width))
    assert.equal(resolved.auxiliaryVisible, true, String(width))
    assert.equal(resolved.primaryWidth, 280)
    assert.equal(resolved.auxiliaryWidth, 280)
    assert.equal(resolved.bottomVisible, false)
    assert.ok(resolved.centerMinWidth >= 360)
    assert.deepEqual(resolved.autoCollapsed, { primary: false, auxiliary: false, bottom: false })
  }
})

test('resolveWorkbenchLayout auto-collapses auxiliary before touching primary', () => {
  const resolved = resolveWorkbenchLayout(defaultLayout(), 960, 640)
  assert.equal(resolved.primaryVisible, true)
  assert.equal(resolved.primaryWidth, 280)
  assert.equal(resolved.auxiliaryVisible, false)
  assert.equal(resolved.bottomVisible, false)
  assert.equal(resolved.autoCollapsed.auxiliary, true)
  assert.equal(resolved.autoCollapsed.primary, false)
})

test('resolveWorkbenchLayout shrinks primary width before hiding it', () => {
  const resolved = resolveWorkbenchLayout(defaultLayout(), 640, 800)
  assert.equal(resolved.primaryVisible, true)
  assert.equal(resolved.primaryWidth, 228)
  assert.equal(resolved.auxiliaryVisible, false)
  assert.equal(resolved.autoCollapsed.primary, false)
  assert.equal(resolved.autoCollapsed.auxiliary, true)
})

test('resolveWorkbenchLayout hides primary only when activity bar plus minimum center cannot fit', () => {
  const resolved = resolveWorkbenchLayout(defaultLayout(), 600, 800)
  assert.equal(resolved.primaryVisible, false)
  assert.equal(resolved.auxiliaryVisible, false)
  assert.equal(resolved.autoCollapsed.primary, true)
  assert.equal(resolved.autoCollapsed.auxiliary, true)
})

test('resolveWorkbenchLayout does not mark user-hidden panels as auto-collapsed', () => {
  const layout = defaultLayout()
  layout.primarySidebar.visible = false
  layout.auxiliarySidebar.visible = false
  const resolved = resolveWorkbenchLayout(layout, 1920, 1080)
  assert.equal(resolved.primaryVisible, false)
  assert.equal(resolved.auxiliaryVisible, false)
  assert.deepEqual(resolved.autoCollapsed, { primary: false, auxiliary: false, bottom: false })
})

test('resolveWorkbenchLayout hides bottom panel below the height threshold without mutating layout', () => {
  const layout = defaultLayout()
  layout.bottomPanel.visible = true
  const short = resolveWorkbenchLayout(layout, 1280, 439)
  assert.equal(short.bottomVisible, false)
  assert.equal(short.autoCollapsed.bottom, true)
  assert.equal(layout.bottomPanel.visible, true)
  const tall = resolveWorkbenchLayout(layout, 1280, 900)
  assert.equal(tall.bottomVisible, true)
  assert.equal(tall.bottomHeight, 220)
  assert.equal(tall.autoCollapsed.bottom, false)
})

test('resolveWorkbenchLayout clamps dimensions before fitting them to the viewport', () => {
  const layout = defaultLayout()
  layout.primarySidebar.width = 999
  layout.auxiliarySidebar.width = 1
  layout.bottomPanel.visible = true
  layout.bottomPanel.height = 999
  const resolved = resolveWorkbenchLayout(layout, 1920, 1080)
  assert.equal(resolved.primaryWidth, 480)
  assert.equal(resolved.auxiliaryWidth, 220)
  assert.equal(resolved.bottomHeight, 420)
})
