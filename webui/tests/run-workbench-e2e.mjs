// 真实 WebView2 工作台验收：尺寸、键盘、拖动、持久化、坏状态与重复操作开销。
// KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/run-workbench-e2e.mjs
import { writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, runAppFixture } from './plugin-e2e-harness.mjs'

const KEY = 'workbench.layout.v1'
const VIEWPORTS = [[1920, 1080], [1280, 900], [960, 640], [800, 600], [600, 800]]
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms))
const round = (value) => Math.round(value * 10) / 10
const MOD = { alt: 1, ctrl: 2 }

function defaultLayout() {
  return {
    version: 1,
    primarySidebar: { visible: true, width: 280, activeViewId: 'kf:workspace' },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
  }
}

async function setLayout(json, origin, layout) {
  await json(`${origin}/api/config`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ [KEY]: JSON.stringify(layout) }),
  })
}

async function readLayout(json, origin) {
  const config = await json(`${origin}/api/config`)
  const raw = config?.[KEY]
  check(typeof raw === 'string' && raw.length > 0, `config has no ${KEY}: ${JSON.stringify(config)}`)
  return JSON.parse(raw)
}

async function waitForWorkbench(cdp, waitFor) {
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
  await sleep(250)
}

async function reload(cdp, waitFor) {
  // Page.reload 只表示导航已发起；旧文档还在时 .workbench 仍能命中，
  // 会让后续断言在旧页面上跑。先等 timeOrigin 变化，再等新文档挂载。
  const before = Number(await cdp.eval('performance.timeOrigin'))
  await cdp.send('Page.reload')
  await waitFor(() => cdp.eval(`performance.timeOrigin !== ${before}`), 30000, 'document reload')
  await waitForWorkbench(cdp, waitFor)
}

async function setViewport(cdp, width, height) {
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width, height, deviceScaleFactor: 1, mobile: false,
  })
  await sleep(350)
  const inner = await cdp.eval('({ width: window.innerWidth, height: window.innerHeight })')
  check(inner?.width === width && inner?.height === height,
    `viewport ${width}x${height} did not apply: ${JSON.stringify(inner)}`)
}

async function measure(cdp) {
  return cdp.eval(`(() => {
    const box = (selector) => {
      const node = document.querySelector(selector)
      if (!node) return null
      const r = node.getBoundingClientRect()
      return { x: r.x, y: r.y, width: r.width, height: r.height, right: r.right, bottom: r.bottom }
    }
    return {
      inner: { width: window.innerWidth, height: window.innerHeight },
      workbench: box('.workbench'),
      activity: box('.activitybar'),
      primary: box('[data-testid=workbench-primary]'),
      auxiliary: box('[data-testid=workbench-auxiliary]'),
      center: box('[data-testid=workbench-center-column]'),
      centerMain: box('.workbench-main-slot'),
      primarySeparator: box('[data-testid=workbench-separator-primary]'),
      bottom: box('.bottom-panel'),
      bottomSeparator: box('[data-testid=workbench-separator-bottom]'),
      statusbar: box('.statusbar'),
      topbar: box('.topbar'),
      scrollWidth: document.documentElement.scrollWidth,
      bodyScrollWidth: document.body.scrollWidth,
      nodes: document.querySelectorAll('*').length,
    }
  })()`)
}

function assertViewport(result, width, height) {
  check(result.inner.width === width && result.inner.height === height, `inner size ${JSON.stringify(result.inner)}`)
  check(result.workbench && result.workbench.x === 0, `workbench missing or offset: ${JSON.stringify(result.workbench)}`)
  check(Math.abs(result.workbench.width - width) <= 1.5,
    `workbench width ${round(result.workbench.width)} != viewport ${width}`)
  check(Math.abs(result.activity.width - 48) <= 0.5, `activity bar width ${round(result.activity.width)} != 48`)
  check(result.topbar && Math.abs(result.topbar.height - 48) <= 0.5, `topbar height ${JSON.stringify(result.topbar)}`)
  check(result.statusbar && Math.abs(result.statusbar.bottom - result.workbench.bottom) <= 1,
    `statusbar must close the workbench: ${JSON.stringify({ statusbar: result.statusbar, workbench: result.workbench })}`)
  check(Math.abs(result.workbench.height - (height - 48)) <= 1.5,
    `workbench height ${round(result.workbench.height)} != viewport height - 48`)
  check(result.scrollWidth <= width + 1 && result.bodyScrollWidth <= width + 1,
    `horizontal overflow: ${result.scrollWidth}/${result.bodyScrollWidth} > ${width}`)
}

function assertCenter(result, expected) {
  check(Math.abs(result.center.width - expected) <= 2,
    `center width ${round(result.center.width)} != ${expected} (viewport ${result.inner.width})`)
}

async function key(cdp, { key: value, code, vk, modifiers = 0 }) {
  const base = {
    modifiers, key: value, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk,
  }
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base })
}

async function clickAt(cdp, x, y, clickCount = 1) {
  await cdp.send('Input.dispatchMouseEvent', {
    type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount,
  })
  await cdp.send('Input.dispatchMouseEvent', {
    type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount,
  })
}

async function drag(cdp, separator, deltaX, { pauseBeforeReleaseMs = 0 } = {}) {
  const x = separator.x + separator.width / 2
  const y = separator.y + separator.height / 2
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, button: 'none', buttons: 0 })
  await cdp.send('Input.dispatchMouseEvent', {
    type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1,
  })
  const steps = 8
  for (let index = 1; index <= steps; index += 1) {
    await cdp.send('Input.dispatchMouseEvent', {
      type: 'mouseMoved', x: x + (deltaX * index) / steps, y, button: 'left', buttons: 1,
    })
    await sleep(20)
  }
  if (pauseBeforeReleaseMs) await sleep(pauseBeforeReleaseMs)
  await cdp.send('Input.dispatchMouseEvent', {
    type: 'mouseReleased', x: x + deltaX, y, button: 'left', buttons: 0, clickCount: 1,
  })
}

async function capture(cdp, results, name) {
  try {
    const shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
    writeFileSync(path.join(results, name), Buffer.from(shot.data, 'base64'))
  } catch (error) {
    console.log(`screenshot ${name} skipped: ${error.message}`)
  }
}

await runAppFixture('workbench-e2e', async ({ launch, stop, json, waitFor, results }) => {
  let session = await launch()
  let { cdp, origin } = session
  await waitForWorkbench(cdp, waitFor)

  // 从一份已知布局开始，排除开发机历史配置。
  await setLayout(json, origin, defaultLayout())
  await reload(cdp, waitFor)

  const sizes = []
  for (const [width, height] of VIEWPORTS) {
    await setViewport(cdp, width, height)
    const measured = await measure(cdp)
    assertViewport(measured, width, height)
    if (width >= 1280) {
      check(measured.primary && Math.abs(measured.primary.width - 280) <= 1,
        `${width}px primary width ${JSON.stringify(measured.primary)}`)
      check(measured.auxiliary && Math.abs(measured.auxiliary.width - 280) <= 1,
        `${width}px auxiliary width ${JSON.stringify(measured.auxiliary)}`)
      assertCenter(measured, width - 48 - 280 - 4 - 280 - 4)
    } else if (width === 960 || width === 800) {
      check(measured.primary && Math.abs(measured.primary.width - 280) <= 1,
        `${width}px primary width ${JSON.stringify(measured.primary)}`)
      check(!measured.auxiliary, `${width}px auxiliary sidebar must auto-collapse`)
      assertCenter(measured, width - 48 - 280 - 4)
    } else if (width === 600) {
      check(!measured.primary, '600px primary sidebar must auto-collapse')
      check(!measured.auxiliary, '600px auxiliary sidebar must auto-collapse')
      check(measured.activity && measured.activity.width === 48, 'activity bar must stay reachable')
      assertCenter(measured, 600 - 48)
    }
    sizes.push({
      viewport: `${width}x${height}`,
      primary: measured.primary ? round(measured.primary.width) : 0,
      auxiliary: measured.auxiliary ? round(measured.auxiliary.width) : 0,
      center: round(measured.center.width),
    })
  }

  // 跨 DPI：CSS 像素尺寸不随缩放变化，避免高 DPI 下侧栏/中心漂移。
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 1280, height: 900, deviceScaleFactor: 2, mobile: false,
  })
  await sleep(350)
  const dpr = await cdp.eval('window.devicePixelRatio')
  check(Math.abs(dpr - 2) < 0.01, `devicePixelRatio did not apply: ${dpr}`)
  const hidpi = await measure(cdp)
  assertViewport(hidpi, 1280, 900)
  check(hidpi.primary && Math.abs(hidpi.primary.width - 280) <= 1,
    `hidpi primary width ${JSON.stringify(hidpi.primary)}`)
  check(hidpi.auxiliary && Math.abs(hidpi.auxiliary.width - 280) <= 1,
    `hidpi auxiliary width ${JSON.stringify(hidpi.auxiliary)}`)
  assertCenter(hidpi, 664)
  await cdp.send('Emulation.clearDeviceMetricsOverride')
  await setViewport(cdp, 1280, 900)

  // 窄窗下自动收起不能改写用户显隐。
  await setViewport(cdp, 600, 800)
  const savedAfterCollapse = await readLayout(json, origin)
  check(savedAfterCollapse.primarySidebar.visible === true && savedAfterCollapse.auxiliarySidebar.visible === true,
    `auto-collapse leaked into persisted layout: ${JSON.stringify(savedAfterCollapse)}`)

  // 键盘：Ctrl+B / Ctrl+Alt+B / Ctrl+J；输入框内不拦截。
  await setViewport(cdp, 1280, 900)
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector("[data-testid=workbench-primary]")'), 3000, 'Ctrl+B hide primary')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=workbench-primary]"))'), 3000, 'Ctrl+B show primary')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl | MOD.alt })
  await waitFor(() => cdp.eval('!document.querySelector("[data-testid=workbench-auxiliary]")'), 3000, 'Ctrl+Alt+B hide auxiliary')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl | MOD.alt })
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=workbench-auxiliary]"))'), 3000, 'Ctrl+Alt+B show auxiliary')
  await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 3000, 'Ctrl+J show bottom panel')
  const withBottom = await measure(cdp)
  check(withBottom.bottom && Math.abs(withBottom.bottom.height - 220) <= 2,
    `bottom panel height ${JSON.stringify(withBottom.bottom)}`)
  check(withBottom.centerMain && withBottom.centerMain.height < 700,
    `center did not shrink for bottom panel: ${JSON.stringify(withBottom.centerMain)}`)
  await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector(".bottom-panel")'), 3000, 'Ctrl+J hide bottom panel')
  await cdp.eval('document.querySelector(".toolbar input")?.focus()')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await sleep(250)
  check(await cdp.eval('Boolean(document.querySelector("[data-testid=workbench-primary]"))'),
    'Ctrl+B must not fire while a text input is focused')
  await cdp.eval('document.activeElement?.blur()')

  // 拖动只能在松手后保存；松手后宽度落盘。
  await setLayout(json, origin, defaultLayout())
  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  let measured = await measure(cdp)
  const beforeDrag = await readLayout(json, origin)
  check(beforeDrag.primarySidebar.width === 280, `baseline width ${beforeDrag.primarySidebar.width}`)
  await drag(cdp, measured.primarySeparator, 64, { pauseBeforeReleaseMs: 500 })
  const duringRelease = await readLayout(json, origin)
  check(duringRelease.primarySidebar.width === 280,
    `layout persisted before pointer release: width=${duringRelease.primarySidebar.width}`)
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.width > 300, 4000, 'drag width saved')
  measured = await measure(cdp)
  const draggedWidth = measured.primary.width
  check(draggedWidth >= 330 && draggedWidth <= 360, `dragged width ${round(draggedWidth)}`)

  // 双击分隔条复位到 280。
  const separator = measured.primarySeparator
  const sx = separator.x + separator.width / 2
  const sy = separator.y + separator.height / 2
  await clickAt(cdp, sx, sy, 1)
  await clickAt(cdp, sx, sy, 2)
  await waitFor(() => cdp.eval('Math.abs(document.querySelector("[data-testid=workbench-primary]").getBoundingClientRect().width - 280) <= 1'), 4000, 'double click reset width')
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.width === 280, 4000, 'reset width saved')

  // 分隔条可聚焦并用方向键调整；键盘调整同样只在动作结束后持久化。
  const separatorFocus = await cdp.eval(`(() => {
    const node = document.querySelector('[data-testid=workbench-separator-primary]')
    node?.focus()
    return { focused: document.activeElement === node, role: node?.getAttribute('role'), tabIndex: node?.tabIndex }
  })()`)
  check(separatorFocus.focused && separatorFocus.role === 'separator' && separatorFocus.tabIndex === 0,
    'separator must be keyboard focusable: ' + JSON.stringify(separatorFocus))
  await key(cdp, { key: 'ArrowRight', code: 'ArrowRight', vk: 39 })
  const keyboardWidth = await waitFor(() => cdp.eval(`(() => {
    const width = document.querySelector('[data-testid=workbench-primary]').getBoundingClientRect().width
    return Math.abs(width - 280) > 2 ? width : null
  })()`), 4000, 'keyboard resize width')
  check(keyboardWidth > 280 && keyboardWidth < 500, 'keyboard resize width: ' + round(keyboardWidth))
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.width > 280, 4000, 'keyboard resize saved')
  const keyboardSeparator = (await measure(cdp)).primarySeparator
  await clickAt(cdp, keyboardSeparator.x + keyboardSeparator.width / 2, keyboardSeparator.y + keyboardSeparator.height / 2, 1)
  await clickAt(cdp, keyboardSeparator.x + keyboardSeparator.width / 2, keyboardSeparator.y + keyboardSeparator.height / 2, 2)
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.width === 280, 4000, 'keyboard reset saved')

  // 隐藏再显示必须恢复拖动后的宽度。
  measured = await measure(cdp)
  await drag(cdp, measured.primarySeparator, 60)
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.width > 300, 4000, 'second drag saved')
  measured = await measure(cdp)
  const hiddenWidth = measured.primary.width
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector("[data-testid=workbench-primary]")'), 3000, 'hide primary')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=workbench-primary]"))'), 3000, 'show primary')
  measured = await measure(cdp)
  check(Math.abs(measured.primary.width - hiddenWidth) <= 1,
    `reopened width ${round(measured.primary.width)} != ${round(hiddenWidth)}`)

  // 中心分组：split、活动组、组间移动、单例页、空组折叠、窄窗只显示活动组、重启恢复。
  await setLayout(json, origin, defaultLayout())
  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  const groupSnapshot = () => cdp.eval(`(() => ({
    groups: [...document.querySelectorAll('[data-center-group]')].map((node) => ({
      id: node.dataset.centerGroup,
      active: node.dataset.centerGroupActive === 'true',
      closable: [...node.querySelectorAll('[data-center-tab-close]')].map((button) => button.dataset.centerTabClose),
    })),
    collapsed: document.querySelector('.center-groups-root')?.dataset.centerCollapsed || null,
  }))()`)
  const hasSettingsPage = '[...document.querySelectorAll("[data-center-tab-close]")].some((node) => node.dataset.centerTabClose === "kf:settings")'
  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  await waitFor(() => cdp.eval(hasSettingsPage), 4000, 'settings center page opens')
  await cdp.eval('document.querySelector(\'.center-group[data-center-group="g1"] [data-center-split="row"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 4000, 'group split creates second group')
  let groupState = await groupSnapshot()
  check(groupState.groups.length === 2, 'split should keep two groups: ' + JSON.stringify(groupState))
  check(groupState.groups[1].active && groupState.groups[1].closable.length === 0,
    'new group must be empty and active: ' + JSON.stringify(groupState))
  await cdp.eval(`(() => {
    const close = document.querySelector('.center-group[data-center-group="g1"] [data-center-tab-close="kf:settings"]')
    close?.closest('.center-tab')?.querySelector('.center-tab-label')?.click()
  })()`)
  await waitFor(() => cdp.eval('document.querySelector(\'.center-group[data-center-group="g1"]\')?.dataset.centerGroupActive === "true"'), 3000, 'first group activates')
  await cdp.eval('document.querySelector(\'.center-group[data-center-group="g1"] [data-center-move-next]\')?.click()')
  await waitFor(() => cdp.eval('[...document.querySelectorAll(\'.center-group[data-center-group="g2"] [data-center-tab-close]\')].some((node) => node.dataset.centerTabClose === "kf:settings")'), 4000, 'page moves to next group')
  groupState = await groupSnapshot()
  check(groupState.groups[1].closable.includes('kf:settings'), 'settings should move into g2: ' + JSON.stringify(groupState))
  check(groupState.groups[0].closable.length === 1 && groupState.groups[0].closable[0] === 'kf:symbols',
    'g1 should keep only home and expose its close button: ' + JSON.stringify(groupState))
  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  await sleep(200)
  groupState = await groupSnapshot()
  check(groupState.groups.length === 2 && groupState.groups[1].closable.filter((key) => key === 'kf:settings').length === 1,
    'singleton page must not duplicate across groups: ' + JSON.stringify(groupState))
  check(groupState.groups[1].active, 'selecting an open page should focus its group: ' + JSON.stringify(groupState))
  const centerSeparator = await cdp.eval(`(() => {
    const node = document.querySelector('.center-split-separator')
    if (!node) return null
    const r = node.getBoundingClientRect()
    return { x: r.x, y: r.y, width: r.width, height: r.height }
  })()`)
  check(centerSeparator, 'center split separator missing')
  await drag(cdp, centerSeparator, 60)
  await waitFor(async () => {
    const saved = await readLayout(json, origin)
    const first = saved.center?.groups?.find((group) => group.id === 'g1')
    return Number.isFinite(first?.size) && first.size > 52
  }, 4000, 'center group width saved')
  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 5000, 'groups after resize reload')
  const resizedLayout = await readLayout(json, origin)
  const firstGroupSize = resizedLayout.center.groups.find((group) => group.id === 'g1')?.size
  check(Number.isFinite(firstGroupSize) && firstGroupSize > 52,
    'center group width must survive reload: ' + JSON.stringify(resizedLayout.center))
  groupState = await groupSnapshot()
  check(groupState.groups.length === 2 && groupState.groups[1].closable.includes('kf:settings'),
    'settings must stay in g2 after resize reload: ' + JSON.stringify(groupState))
  await cdp.eval('document.querySelector(\'.center-group[data-center-group="g2"] [data-center-tab-close="kf:settings"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 1'), 4000, 'closing the only g2 page collapses the group')
  await cdp.eval('document.querySelector(\'.center-group[data-center-group="g1"] [data-center-split="row"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 4000, 'split again for persistence')
  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  const reopenedGroup = await cdp.eval(`document.querySelector('[data-center-group][data-center-group-active="true"]')?.dataset.centerGroup`)
  check(reopenedGroup && reopenedGroup !== 'g1', 'reopened page should target the new active group: ' + reopenedGroup)
  await waitFor(() => cdp.eval(`[...document.querySelectorAll('.center-group[data-center-group="${reopenedGroup}"] [data-center-tab-close]')].some((node) => node.dataset.centerTabClose === "kf:settings")`), 4000, 'settings opens in the active empty group')
  await waitFor(async () => {
    const saved = await readLayout(json, origin)
    const second = saved.center?.groups?.find((group) => group.id === reopenedGroup)
    return saved.version === 2 && saved.center.groups.length === 2 && second?.pages.includes('kf:settings')
  }, 5000, 'split layout persisted')
  const savedGroups = await readLayout(json, origin)
  check(savedGroups.center.tree.type === 'split', 'persisted tree should be a split: ' + JSON.stringify(savedGroups.center.tree))
  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 5000, 'center groups hydrate after reload')
  groupState = await groupSnapshot()
  check(groupState.groups.length === 2 && groupState.groups[1].closable.includes('kf:settings'),
    'center groups must survive reload: ' + JSON.stringify(groupState))
  await setViewport(cdp, 600, 800)
  await waitFor(() => cdp.eval('document.querySelector(".center-groups-root")?.dataset.centerCollapsed === "true"'), 3000, 'narrow center collapses to active group')
  groupState = await groupSnapshot()
  check(groupState.groups.length === 1 && groupState.groups[0].closable.includes('kf:settings'),
    'narrow center should render only the active group: ' + JSON.stringify(groupState))
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 3000, 'wide center restores both groups')

  // 关掉再启动（端口会变）：布局仍应从宿主配置恢复。
  const persistedBeforeRelaunch = await readLayout(json, origin)
  await sleep(700)
  await stop(session)
  session = await launch()
  ;({ cdp, origin } = session)
  await waitForWorkbench(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 5000, 'center groups hydrate after relaunch')
  measured = await measure(cdp)
  check(Math.abs(measured.primary.width - persistedBeforeRelaunch.primarySidebar.width) <= 1,
    `layout did not survive relaunch: ${round(measured.primary.width)} vs ${persistedBeforeRelaunch.primarySidebar.width}`)
  groupState = await groupSnapshot()
  check(groupState.groups.length === 2 && groupState.groups[1].closable.includes('kf:settings'),
    'center groups must survive relaunch: ' + JSON.stringify(groupState))

  // 坏快照：回退默认布局并提示一次，不把坏值渲染进界面。
  await setLayout(json, origin, { version: 1, primarySidebar: { width: 'broken' } })
  await reload(cdp, waitFor)
  await waitFor(async () => (await cdp.eval('document.getElementById("toast")?.innerText || ""')).includes('布局配置已损坏'), 4000, 'layout recovery toast')
  await setViewport(cdp, 1280, 900)
  measured = await measure(cdp)
  check(measured.primary && Math.abs(measured.primary.width - 280) <= 1, `bad layout primary ${JSON.stringify(measured.primary)}`)
  check(measured.auxiliary, 'bad layout must restore default auxiliary sidebar')
  check(!measured.bottom, 'bad layout must restore default hidden bottom panel')

  // 坏中心组快照：重复 group id 属于不可修复的结构错误，整份中心状态回退默认树。
  const badCenterLayout = {
    ...defaultLayout(),
    version: 2,
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [
        { id: 'g1', pages: ['kf:symbols'], active: 'kf:symbols', size: 100 },
        { id: 'g1', pages: ['kf:settings'], active: 'kf:settings', size: 100 },
      ],
    },
  }
  await setLayout(json, origin, badCenterLayout)
  await reload(cdp, waitFor)
  await waitFor(async () => (await cdp.eval('document.getElementById("toast")?.innerText || ""')).includes('布局配置已损坏'), 4000, 'bad center recovery toast')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 1'), 5000, 'bad center falls back to one group')
  const badCenterState = await cdp.eval(`({
    groups: document.querySelectorAll('[data-center-group]').length,
    tabs: [...document.querySelectorAll('.center-tab-label')].map((node) => node.textContent.trim()),
  })`)
  check(badCenterState.groups === 1 && badCenterState.tabs.length === 1 && badCenterState.tabs[0] === '全部元件',
    'bad center state must not render duplicate groups: ' + JSON.stringify(badCenterState))

  // 缺失插件：快照里记录的已卸载插件页必须被 prune，焦点回退到 home，不能留下空页签。
  const missingPageLayout = {
    ...defaultLayout(),
    version: 2,
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [{
        id: 'g1',
        pages: ['kf:symbols', 'view:missing.plugin'],
        active: 'view:missing.plugin',
        size: 100,
      }],
    },
  }
  await setLayout(json, origin, missingPageLayout)
  await reload(cdp, waitFor)
  await waitFor(() => cdp.eval(`(() => {
    const tabs = [...document.querySelectorAll('.center-tab-label')].map((node) => node.textContent.trim())
    return tabs.length === 1 && tabs[0] === '全部元件' && !document.querySelector('.plugin-view')
  })()`), 5000, 'missing plugin page pruned')
  const missingState = await cdp.eval(`({
    tabs: [...document.querySelectorAll('.center-tab-label')].map((node) => node.textContent.trim()),
    emptyViews: document.querySelectorAll('.plugin-view').length,
    toolbar: Boolean(document.querySelector('.center-group[data-center-group-active="true"] .toolbar')),
  })`)
  check(missingState.tabs.length === 1 && missingState.emptyViews === 0 && missingState.toolbar,
    'missing plugin page must not leave an empty tab: ' + JSON.stringify(missingState))
  await waitFor(async () => {
    const saved = await readLayout(json, origin)
    return saved.center?.groups?.length === 1
      && saved.center.groups[0].pages.length === 1
      && saved.center.groups[0].pages[0] === 'kf:symbols'
  }, 5000, 'missing plugin pruned from saved layout')

  // 保存失败：进入错误态，菜单出现重试；恢复网络后重试成功。
  await cdp.eval(`(() => {
    if (!window.__kfFetch) window.__kfFetch = window.fetch
    window.fetch = (...args) => {
      const [url, options] = args
      if (String(url).includes('/api/config') && options?.method === 'POST') return Promise.reject(new Error('offline'))
      return window.__kfFetch(...args)
    }
    return true
  })()`)
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector("[data-testid=workbench-primary]")'), 3000, 'hide primary while offline')
  await sleep(500)
  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find((node) => node.textContent.trim() === '视图')?.click()`)
  await waitFor(() => cdp.eval('[...document.querySelectorAll(".topbar-item")].some((node) => node.textContent.includes("重试保存"))'), 3000, 'retry save menu item')
  await cdp.eval('window.fetch = window.__kfFetch')
  await cdp.eval(`[...document.querySelectorAll('.topbar-item')].find((node) => node.textContent.includes("重试保存"))?.click()`)
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.visible === false, 4000, 'retry save persisted hidden')
  await key(cdp, { key: 'b', code: 'KeyB', vk: 66, modifiers: MOD.ctrl })
  await waitFor(async () => (await readLayout(json, origin)).primarySidebar.visible === true, 4000, 'restore visible after retry')

  // 编辑命令上下文：只作用于菜单打开前的可见输入框；切页/弹层后必须失效。
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".toolbar input"))'), 5000, 'symbol search input')
  await cdp.eval('document.querySelector(".toolbar input")?.focus()')
  await cdp.send('Input.insertText', { text: 'resistor' })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar input")?.value === "resistor"'), 3000, 'search input value')
  await cdp.eval(`(() => {
    const input = document.querySelector('.toolbar input')
    input.focus(); input.setSelectionRange(2, 5)
  })()`)

  const openEditMenu = async () => {
    await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find((node) => node.textContent.trim() === '编辑')?.click()`)
    await waitFor(() => cdp.eval('Boolean(document.querySelector(".topbar-dropdown"))'), 3000, 'edit menu opens')
  }
  const editMenuState = () => cdp.eval(`(() => {
    const items = [...document.querySelectorAll('.topbar-item')]
    const pick = (label) => items.find((node) => node.textContent.trim().startsWith(label))
    return {
      copy: pick('复制')?.disabled,
      cut: pick('剪切')?.disabled,
      selectAll: pick('全选')?.disabled,
      paste: pick('粘贴')?.disabled,
    }
  })()`)

  await openEditMenu()
  let editState = await editMenuState()
  check(editState.copy === false && editState.cut === false && editState.selectAll === false,
    'edit commands must follow the focused visible input: ' + JSON.stringify(editState))
  await cdp.eval(`[...document.querySelectorAll('.topbar-item')].find((node) => node.textContent.trim().startsWith('全选'))?.click()`)
  await waitFor(() => cdp.eval('!document.querySelector(".topbar-dropdown")'), 3000, 'edit menu closes after select all')
  const selected = await cdp.eval(`(() => {
    const input = document.querySelector('.toolbar input')
    return { start: input.selectionStart, end: input.selectionEnd, length: input.value.length }
  })()`)
  check(selected.start === 0 && selected.end === selected.length,
    'select all must target the captured input: ' + JSON.stringify(selected))

  // 打开设置页后首页被 keep-alive 隐藏，最近输入框不得继续作为命令目标。
  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".settings-page"))'), 5000, 'settings page opens for edit context')
  await openEditMenu()
  editState = await editMenuState()
  check(editState.copy === true && editState.cut === true && editState.selectAll === true,
    'hidden center page input must not be an edit target: ' + JSON.stringify(editState))
  await key(cdp, { key: 'Escape', code: 'Escape', vk: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".topbar-dropdown")'), 3000, 'Escape closes only the edit menu')
  check(await cdp.eval('Boolean(document.querySelector(".settings-page"))'), 'Escape on menu must not close the center page')

  // 设置 → 工具 → 重置数据库：确认框进浮层栈，Escape 只关确认并还焦点。
  await cdp.eval(`[...document.querySelectorAll('.settings-page div')].find((node) => node.textContent.trim() === '🔧 工具')?.click()`)
  await waitFor(() => cdp.eval(`[...document.querySelectorAll('.settings-page button')].some((node) => node.textContent.trim() === '重置数据库')`), 5000, 'reset database button')
  const resetPoint = await cdp.eval(`(() => {
    const button = [...document.querySelectorAll('.settings-page button')].find((node) => node.textContent.trim() === '重置数据库')
    button.scrollIntoView({ block: 'center' })
    const r = button.getBoundingClientRect()
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 }
  })()`)
  await clickAt(cdp, resetPoint.x, resetPoint.y)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".confirm-modal[role=dialog]"))'), 4000, 'confirm dialog in overlay stack')
  const confirmState = await cdp.eval(`(() => {
    const modal = document.querySelector('.confirm-modal')
    return {
      title: modal?.querySelector('h3')?.textContent,
      message: modal?.querySelector('.confirm-message')?.textContent || '',
      focused: document.activeElement?.textContent?.trim(),
    }
  })()`)
  check(confirmState.title === '确认操作' && confirmState.message.includes('重置数据库'),
    'confirm dialog should use the shared shell: ' + JSON.stringify(confirmState))
  check(confirmState.focused === '取消', 'confirm default focus should be Cancel: ' + JSON.stringify(confirmState))
  await key(cdp, { key: 'Escape', code: 'Escape', vk: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".confirm-modal")'), 3000, 'Escape closes the confirm dialog')
  check(await cdp.eval('Boolean(document.querySelector(".settings-page"))'),
    'closing the confirm dialog must keep the settings center page')
  const restored = await cdp.eval('document.activeElement?.textContent?.trim()')
  check(restored === '重置数据库', 'focus must return to the confirm trigger: ' + restored)

  // About 也走同一个浮层栈：顶层 Escape 先关编辑菜单，再关关于。
  await cdp.eval(`document.querySelector('[data-center-tab-close="kf:settings"]')?.click()`)
  await waitFor(() => cdp.eval('!document.querySelector(".settings-page")'), 3000, 'settings page closes')
  await cdp.eval('document.querySelector(".toolbar input")?.focus()')
  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find((node) => node.textContent.trim() === '关于')?.click()`)
  await waitFor(() => cdp.eval(`[...document.querySelectorAll('.topbar-item')].some((node) => node.textContent.trim() === '关于 KiCad Forge')`), 3000, 'about menu opens')
  await cdp.eval(`[...document.querySelectorAll('.topbar-item')].find((node) => node.textContent.trim() === '关于 KiCad Forge')?.click()`)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".about-modal[role=dialog]"))'), 4000, 'about dialog opens')
  await openEditMenu()
  editState = await editMenuState()
  check(editState.copy === true && editState.cut === true,
    'dialog surface must block the page input as edit target: ' + JSON.stringify(editState))
  await key(cdp, { key: 'Escape', code: 'Escape', vk: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".topbar-dropdown")'), 3000, 'top layer Escape closes only menu')
  check(await cdp.eval('Boolean(document.querySelector(".about-modal"))'), 'about must stay open after closing the menu')
  await key(cdp, { key: 'Escape', code: 'Escape', vk: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".about-modal")'), 3000, 'second Escape closes about')
  check(await cdp.eval('document.activeElement?.matches(".toolbar input") === true'),
    'about close must restore focus to the trigger context')
  await cdp.eval('document.activeElement?.blur()')

  // 重复操作：DOM 数量稳定，5 次往返各自有可测耗时。
  await setViewport(cdp, 1280, 900)
  const nodesBefore = await cdp.eval('document.querySelectorAll("*").length')
  const timings = []
  for (let index = 0; index < 5; index += 1) {
    const started = Date.now()
    await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
    await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 3000, `perf open ${index + 1}`)
    await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
    await waitFor(() => cdp.eval('!document.querySelector(".bottom-panel")'), 3000, `perf close ${index + 1}`)
    timings.push(Date.now() - started)
  }
  const nodesAfter = await cdp.eval('document.querySelectorAll("*").length')
  check(nodesBefore === nodesAfter, `DOM nodes leaked: ${nodesBefore} -> ${nodesAfter}`)
  check(timings.every(ms => ms < 2000), `repeat toggle too slow: ${timings.join(', ')}`)

  // 200 次往返稳态：完成后 DOM、样式、插件视图回到同一规模，底部面板不在打开态。
  await cdp.eval(`(() => {
    if (window.__kfFetchProbe) return true
    const original = window.fetch.bind(window)
    window.__kfActiveFetches = 0
    window.__kfFetchCount = 0
    window.fetch = (...args) => {
      // 心跳（desktopSession 每秒 /api/status）是页面常驻行为，不是 toggle 泄漏；
      // 把它算进 in-flight 会让这项断言随采样时刻随机失败。
      const tracked = !String(args[0]).includes('/api/status')
      if (tracked) {
        window.__kfActiveFetches += 1
        window.__kfLastTrackedFetch = String(args[0])
      }
      window.__kfFetchCount += 1
      return original(...args).finally(() => { if (tracked) window.__kfActiveFetches -= 1 })
    }
    window.__kfFetchProbe = true
    return true
  })()`)
  const steadyBefore = await cdp.eval(`({
    nodes: document.querySelectorAll('*').length,
    styles: document.querySelectorAll('style').length,
    pluginViews: document.querySelectorAll('.plugin-view').length,
    overlays: document.querySelectorAll('[role=dialog]').length,
    activeFetches: window.__kfActiveFetches,
  })`)
  const steadyStarted = Date.now()
  await cdp.eval(`(async () => {
    for (let index = 0; index < 200; index += 1) {
      window.dispatchEvent(new KeyboardEvent('keydown', {
        key: 'j', code: 'KeyJ', ctrlKey: true, bubbles: true, cancelable: true,
      }))
      await new Promise((resolve) => requestAnimationFrame(resolve))
    }
    return true
  })()`)
  await sleep(300)
  // 布局持久化保存（/api/config）由 toggle 触发且可能晚于 300ms 完成；
  // 这里等它在有界时间内结算，再断言稳态，避免把合法保存误判成泄漏。
  await waitFor(() => cdp.eval('window.__kfActiveFetches === 0'), 5000, 'tracked requests settle')
  const steadyAfter = await cdp.eval(`({
    nodes: document.querySelectorAll('*').length,
    styles: document.querySelectorAll('style').length,
    pluginViews: document.querySelectorAll('.plugin-view').length,
    overlays: document.querySelectorAll('[role=dialog]').length,
    activeFetches: window.__kfActiveFetches,
    lastTrackedFetch: window.__kfLastTrackedFetch || '',
    bottomOpen: Boolean(document.querySelector('.bottom-panel')),
  })`)
  check(!steadyAfter.bottomOpen, '200 toggles must end with the bottom panel closed')
  check(steadyAfter.nodes === steadyBefore.nodes,
    `200 toggles leaked DOM: ${steadyBefore.nodes} -> ${steadyAfter.nodes}`)
  check(steadyAfter.styles === steadyBefore.styles,
    `200 toggles leaked styles: ${steadyBefore.styles} -> ${steadyAfter.styles}`)
  check(steadyAfter.pluginViews === steadyBefore.pluginViews && steadyAfter.overlays === steadyBefore.overlays,
    '200 toggles changed plugin/overlay ownership: ' + JSON.stringify({ before: steadyBefore, after: steadyAfter }))
  check(steadyBefore.activeFetches === 0 && steadyAfter.activeFetches === 0,
    '200 toggles left requests in flight: ' + JSON.stringify({ before: steadyBefore, after: steadyAfter }))

  await setViewport(cdp, 1920, 1080)
  await capture(cdp, results, 'workbench-1920.png')
  await setViewport(cdp, 1280, 900)
  await capture(cdp, results, 'workbench-1280.png')
  await setViewport(cdp, 960, 640)
  await capture(cdp, results, 'workbench-960.png')

  const sorted = [...timings].sort((a, b) => a - b)
  console.log(JSON.stringify({
    sizes,
    drag: { before: 280, after: round(hiddenWidth), reset: 280 },
    keyboardResize: { width: round(keyboardWidth) },
    repeatToggleMs: timings,
    repeatToggleMedianMs: sorted[Math.floor(sorted.length / 2)],
    domNodes: { before: nodesBefore, after: nodesAfter },
    steady: { before: steadyBefore, after: steadyAfter, elapsedMs: Date.now() - steadyStarted },
  }, null, 2))
})