// Real WebView2 acceptance for the shared desktop select chrome (D0–D4).
// KFORGE_SELECT_STYLE_E2E=1 KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/e2e/run-select-style-e2e.mjs
import { mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, requireOptIn, runAppFixture } from '../support/plugin-e2e-harness.mjs'

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

async function setViewport(cdp, width, height, deviceScaleFactor = 1) {
  await cdp.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor, mobile: false })
  await sleep(250)
  const inner = await cdp.eval('({ width: window.innerWidth, height: window.innerHeight })')
  check(inner?.width === width && inner?.height === height, 'viewport ' + width + 'x' + height + ': ' + JSON.stringify(inner))
}

async function reload(cdp, waitFor) {
  const before = Number(await cdp.eval('performance.timeOrigin'))
  await cdp.send('Page.reload')
  await waitFor(() => cdp.eval('performance.timeOrigin !== ' + before), 30000, 'document reload')
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
}

async function focusWithKeyboard(cdp, selector) {
  await cdp.eval('document.querySelector(' + JSON.stringify(selector) + ').focus()')
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9 })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9, modifiers: 8 })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9, modifiers: 8 })
  await sleep(160)
}

async function press(cdp, key, code, virtualKeyCode, modifiers = 0) {
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key, code, windowsVirtualKeyCode: virtualKeyCode, modifiers })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: virtualKeyCode, modifiers })
}

function measureScript(selector) {
  return `(() => {
    const node = document.querySelector(${JSON.stringify(selector)})
    if (!node) return null
    const r = node.getBoundingClientRect()
    const s = getComputedStyle(node)
    const clippers = []
    for (let parent = node.parentElement; parent; parent = parent.parentElement) {
      const cs = getComputedStyle(parent)
      if (cs.overflowX !== 'visible' || cs.overflowY !== 'visible') {
        const pr = parent.getBoundingClientRect()
        clippers.push({
          name: typeof parent.className === 'string' ? parent.className : parent.tagName,
          overflowX: cs.overflowX,
          overflowY: cs.overflowY,
          rect: { left: pr.left, right: pr.right, top: pr.top, bottom: pr.bottom },
        })
      }
    }
    const contained = clippers.every((item) => r.left >= item.rect.left - 0.5
      && r.right <= item.rect.right + 0.5 && r.top >= item.rect.top - 0.5 && r.bottom <= item.rect.bottom + 0.5)
    const outlineWidth = parseFloat(s.outlineWidth) || 0
    const outlineOffset = parseFloat(s.outlineOffset) || 0
    return {
      id: node.getAttribute('data-testid') || node.className,
      value: node.value,
      width: r.width,
      height: r.height,
      rect: { left: r.left, right: r.right, top: r.top, bottom: r.bottom },
      fontSize: s.fontSize,
      borderRadius: s.borderRadius,
      borderTopWidth: parseFloat(s.borderTopWidth) || 0,
      paddingLeft: parseFloat(s.paddingLeft) || 0,
      paddingRight: parseFloat(s.paddingRight) || 0,
      backgroundColor: s.backgroundColor,
      backgroundImage: s.backgroundImage,
      appearance: s.appearance,
      whiteSpace: s.whiteSpace,
      textOverflow: s.textOverflow,
      overflow: s.overflow,
      nodeScrollWidth: node.scrollWidth,
      nodeClientWidth: node.clientWidth,
      focusVisible: node.matches(':focus-visible'),
      outlineStyle: s.outlineStyle,
      outlineWidth,
      outlineOffset,
      outlineColor: s.outlineColor,
      ringInside: outlineWidth > 0 && outlineOffset + outlineWidth <= 0.2,
      clippers,
      clippersContain: contained,
      activeElement: document.activeElement === node,
      panelWidth: document.querySelector('.gf-git-panel') ? document.querySelector('.gf-git-panel').getBoundingClientRect().width : null,
      sidebarWidth: node.closest('.sidebar') ? node.closest('.sidebar').getBoundingClientRect().width : null,
      pluginViewWidth: node.closest('.sidebar-plugin-view') ? node.closest('.sidebar-plugin-view').getBoundingClientRect().width : null,
      innerWidth: window.innerWidth,
      clientWidth: document.documentElement.clientWidth,
      devicePixelRatio: window.devicePixelRatio,
      visualScale: window.visualViewport ? window.visualViewport.scale : null,
    }
  })()`
}

function assertSelect(metrics, label) {
  check(metrics, label + ': select missing')
  check(Math.abs(metrics.height - 30) <= 0.6, label + ': height ' + metrics.height)
  check(Math.abs(parseFloat(metrics.fontSize) - 13) <= 0.15, label + ': font ' + metrics.fontSize)
  check(Math.abs(parseFloat(metrics.borderRadius) - 6) <= 0.5, label + ': radius ' + metrics.borderRadius)
  check(metrics.borderTopWidth >= 0.7 && metrics.borderTopWidth <= 1.2, label + ': border ' + metrics.borderTopWidth)
  check(Math.abs(metrics.paddingLeft - 8) <= 0.6, label + ': padding-left ' + metrics.paddingLeft)
  check(Math.abs(metrics.paddingRight - 28) <= 0.8, label + ': padding-right ' + metrics.paddingRight)
  check(metrics.backgroundImage.includes('svg'), label + ': missing shared arrow')
  check(metrics.appearance !== 'auto', label + ': native appearance leaked')
  check(metrics.whiteSpace === 'nowrap' && metrics.textOverflow === 'ellipsis' && metrics.overflow === 'hidden', label + ': long text is not truncated: ' + JSON.stringify({ whiteSpace: metrics.whiteSpace, textOverflow: metrics.textOverflow, overflow: metrics.overflow }))
  check(metrics.width > 60, label + ': width ' + metrics.width)
  check(metrics.focusVisible && metrics.activeElement, label + ': not keyboard focused')
  check(metrics.outlineStyle === 'solid' && metrics.outlineWidth >= 1.5, label + ': ring ' + metrics.outlineStyle + ' ' + metrics.outlineWidth)
  check(metrics.outlineOffset <= -1.4, label + ': ring offset ' + metrics.outlineOffset)
  check(metrics.ringInside, label + ': ring is not inside the control')
  check(metrics.clippersContain, label + ': clipped by an ancestor: ' + JSON.stringify(metrics.clippers))
}

async function capture(cdp, results, name, clip = null) {
  const shot = await cdp.send('Page.captureScreenshot', clip ? { format: 'png', clip } : { format: 'png' })
  const file = path.join(results, 'select-style', name)
  writeFileSync(file, Buffer.from(shot.data, 'base64'))
  return file
}

async function main() {
  if (!requireOptIn('KFORGE_SELECT_STYLE_E2E')) return
  await runAppFixture('select-style', async ({ root, launch, json, waitFor, results }) => {
    const output = path.join(results, 'select-style')
    mkdirSync(output, { recursive: true })
    const libraryDir = path.join(root, 'select-fixture')
    mkdirSync(libraryDir)
    writeFileSync(path.join(libraryDir, 'SelectE2E.kicad_sym'), '(kicad_symbol_lib (version 20231120))\n')
    const gitConfig = path.join(root, 'select-git.config')
    writeFileSync(gitConfig, '[commit]\n\tgpgsign = false\n')
    const session = await launch({ env: {
      GIT_AUTHOR_NAME: 'KF Select E2E', GIT_AUTHOR_EMAIL: 'select@example.invalid',
      GIT_COMMITTER_NAME: 'KF Select E2E', GIT_COMMITTER_EMAIL: 'select@example.invalid',
      GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: gitConfig, GIT_TERMINAL_PROMPT: '0',
    } })
    const { cdp, origin } = session
    await setViewport(cdp, 1280, 900)
    await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
    const added = await json(origin + '/api/component-libraries', {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        action: 'add',
        name: 'Select E2E Library With A Very Long Name',
        symbol_path: libraryDir,
        footprint_path: '',
      }),
    })
    check(added.ok === true, 'library add failed: ' + JSON.stringify(added))
    await reload(cdp, waitFor)
    await setViewport(cdp, 1280, 900)

    const setWidth = async (width) => {
      // Preserve the center tree when a layout already exists. A first run has no
      // saved layout, so use a v1 snapshot: it is migrated to the default center
      // without being classified as recovered, which avoids a test-only toast.
      let current = null
      const config = await json(origin + '/api/config')
      const raw = config?.['workbench.layout.v1']
      if (typeof raw === 'string' && raw.trim()) {
        try {
          const parsed = JSON.parse(raw)
          if (parsed && typeof parsed === 'object' && parsed.center) current = parsed
        } catch { /* a damaged snapshot is replaced below */ }
      }
      const layout = current
        ? { ...current, primarySidebar: { ...current.primarySidebar, visible: true, width, activeViewId: 'kf:workspace' } }
        : {
            version: 1,
            primarySidebar: { visible: true, width, activeViewId: 'kf:workspace' },
            auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
            bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
          }
      const saved = await json(origin + '/api/config', {
        method: 'POST', headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ 'workbench.layout.v1': JSON.stringify(layout) }),
      })
      check(saved.ok === true, 'layout width ' + width + ' failed: ' + JSON.stringify(saved))
    }

    const openWorkspace = async () => {
      await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "工作空间" || item.title === "Workspace")?.click()')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".sidebar-workspace select"))'), 15000, 'workspace select')
    }

    const openGit = async () => {
      if (!(await cdp.eval('Boolean(document.querySelector("[data-testid=git-panel]"))'))) {
        await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "Git")?.click()')
      }
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-panel] [data-testid=git-target]"))'), 20000, 'Git panel')
      await waitFor(() => cdp.eval('(() => { const select = document.querySelector("[data-testid=git-target]"); return Boolean(select && [...select.options].some((option) => option.value === ' + JSON.stringify(libraryDir) + ')); })()'), 15000, 'Git target option')
      await cdp.eval('(() => { const select = document.querySelector("[data-testid=git-target]"); const option = [...select.options].find((item) => item.value === ' + JSON.stringify(libraryDir) + '); if (select.value === option.value) return; const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, "value").set; setter.call(select, option.value); select.dispatchEvent(new Event("change", { bubbles: true })); })()')
      await waitFor(() => cdp.eval('[...document.querySelectorAll(".gf-git-actions button")].every((button) => !button.disabled)'), 20000, 'Git actions idle')
    }

    const openHistory = async () => {
      await cdp.eval('document.querySelector("[data-testid=git-tab-history]").click()')
      await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-history-range]"))'), 15000, 'history range')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".gf-git-history-list .gf-git-history-row"))'), 20000, 'history rows')
      await waitFor(() => cdp.eval('!document.querySelector("[data-testid=git-running]")'), 20000, 'history idle')
    }

    const clickButton = (text) => cdp.eval('(() => { const button = [...document.querySelectorAll(".gf-git-panel button")].find((item) => item.textContent.trim() === ' + JSON.stringify(text) + '); if (!button || button.disabled) return false; button.click(); return true; })()')
    const tasks = async () => (await json(origin + '/api/plugins/com.kicad_forge.git_version/tasks')).tasks

    await openGit()
    const before = new Set((await tasks()).map((task) => task.task_id))
    check(await clickButton('初始化仓库'), 'init button missing')
    await waitFor(async () => {
      const task = (await tasks()).find((item) => !before.has(item.task_id) && item.action === 'init')
      return task && ['succeeded', 'failed'].includes(task.state) ? task : null
    }, 45000, 'init terminal')
    await openHistory()

    const metrics = []
    const screenshots = []

    async function measureAt(label, width) {
      await setViewport(cdp, 1280, 900)
      await setWidth(width)
      await reload(cdp, waitFor)
      await setViewport(cdp, 1280, 900)
      await openWorkspace()
      const workspace = await (async () => {
        await focusWithKeyboard(cdp, '.sidebar-workspace select')
        return cdp.eval(measureScript('.sidebar-workspace select'))
      })()
      assertSelect(workspace, label + ' workspace')
      screenshots.push(await capture(cdp, results, 'select-' + label + '-workspace.png'))
      await openGit()
      const target = await (async () => {
        await focusWithKeyboard(cdp, '[data-testid=git-target]')
        return cdp.eval(measureScript('[data-testid=git-target]'))
      })()
      assertSelect(target, label + ' git target')
      screenshots.push(await capture(cdp, results, 'select-' + label + '-git-target.png'))
      if (label === 'width-200') {
        await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: ' ', code: 'Space', windowsVirtualKeyCode: 32, text: ' ' })
        await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: ' ', code: 'Space', windowsVirtualKeyCode: 32 })
        await sleep(300)
        const targetOpen = await cdp.eval('document.querySelector("[data-testid=git-target]").matches(":open")')
        check(targetOpen, 'long Git target picker did not open')
        screenshots.push(await capture(cdp, results, 'select-width-200-git-target-open.png'))
        await press(cdp, 'Escape', 'Escape', 27)
        await sleep(200)
      }
      await openHistory()
      const range = await (async () => {
        await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
        return cdp.eval(measureScript('[data-testid=git-history-range]'))
      })()
      assertSelect(range, label + ' history range')
      check(Math.abs(range.panelWidth - (width - 24)) <= 1.2, label + ': panel width ' + range.panelWidth + ' for sidebar ' + width)
      check(range.width > 70, label + ': range too narrow ' + range.width)
      metrics.push({ label, viewport: '1280x900', workspace, target, range })
      screenshots.push(await capture(cdp, results, 'select-' + label + '-history-range.png'))
      const ringClip = {
        x: Math.max(0, range.rect.left - 8), y: Math.max(0, range.rect.top - 8),
        width: range.rect.right - range.rect.left + 16, height: range.rect.bottom - range.rect.top + 16, scale: 3,
      }
      screenshots.push(await capture(cdp, results, 'select-' + label + '-history-range-ring.png', ringClip))
      return { workspace, target, range }
    }

    await measureAt('width-280', 280)
    await measureAt('width-200', 200)
    await measureAt('width-440', 440)
    await measureAt('width-480', 480)

    await setWidth(280)
    await reload(cdp, waitFor)
    await setViewport(cdp, 1280, 900)

    // Settings reuses the same base control. Keep this separate from the Git
    // sidebar measurements so both call sites are exercised in the real app.
    const openSettings = async () => {
      const result = await cdp.eval('(() => { const items = [...document.querySelectorAll(".activitybar .nav-item")]; const item = items[items.length - 1]; if (!item) return { clicked: false, titles: [] }; const titles = items.map((entry) => entry.title); item.click(); return { clicked: true, titles }; })()')
      check(result?.clicked === true, 'settings activity button missing: ' + JSON.stringify(result))
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".settings-page"))'), 15000, 'settings page')
      const tabClicked = await cdp.eval('(() => { const nodes = [...document.querySelectorAll(".settings-page div")]; const tab = nodes.find((node) => node.children.length === 0 && /工具|Utilities/.test(node.textContent)); if (!tab) return false; tab.click(); return true; })()')
      check(tabClicked, 'settings utilities tab missing')
      await waitFor(() => cdp.eval('Boolean(document.querySelector(".settings-page select.kf-select"))'), 15000, 'settings select')
    }
    await openSettings()
    await focusWithKeyboard(cdp, '.settings-page select.kf-select')
    const settingsSelect = await cdp.eval(measureScript('.settings-page select.kf-select'))
    assertSelect(settingsSelect, 'settings language')
    check(settingsSelect.width >= 120, 'settings language select lost its readable width: ' + settingsSelect.width)
    const disabledStyle = await cdp.eval('(() => { const node = document.querySelector(".settings-page select.kf-select"); node.disabled = true; const style = getComputedStyle(node); const result = { opacity: parseFloat(style.opacity), cursor: style.cursor, value: node.value }; node.disabled = false; return result; })()')
    check(disabledStyle.opacity <= 0.6, 'disabled select is not visually muted: ' + JSON.stringify(disabledStyle))
    metrics.push({ label: 'settings', viewport: '1280x900', select: settingsSelect, disabled: disabledStyle })
    screenshots.push(await capture(cdp, results, 'select-settings-1280.png'))
    const settingsClosed = await cdp.eval('(() => { const tab = [...document.querySelectorAll(".center-tab")].find((node) => /设置|Settings/.test(node.textContent)); const close = tab ? tab.querySelector(".center-tab-close") : null; if (!close) return false; close.click(); return true; })()')
    check(settingsClosed, 'settings center tab could not be closed')
    await waitFor(() => cdp.eval('![...document.querySelectorAll(".center-tab-label")].some((node) => /设置|Settings/.test(node.textContent))'), 10000, 'settings tab closed')

    await openGit()
    await openHistory()
    for (const [width, height] of [[1920, 1080], [960, 900], [800, 900]]) {
      await setViewport(cdp, width, height)
      await openGit()
      await openHistory()
      const range = await (async () => {
        await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
        return cdp.eval(measureScript('[data-testid=git-history-range]'))
      })()
      assertSelect(range, 'viewport-' + width + ' range')
      metrics.push({ label: 'viewport-' + width, viewport: width + 'x' + height, range })
      screenshots.push(await capture(cdp, results, 'select-viewport-' + width + '-history-range.png'))
    }

    // CSS pixels must stay at 30px/6px under DPI scaling. This is an emulated
    // device-pixel check; a real Windows 125/150/200% desktop capture is still
    // tracked separately in the acceptance report.
    for (const dpr of [1.25, 1.5, 2]) {
      await setViewport(cdp, 1280, 900, dpr)
      await openGit()
      await openHistory()
      const range = await (async () => {
        await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
        return cdp.eval(measureScript('[data-testid=git-history-range]'))
      })()
      assertSelect(range, 'dpi-emulation-' + dpr + ' range')
      check(Math.abs(range.devicePixelRatio - dpr) < 0.01, 'DPI emulation ' + dpr + ' reported ' + range.devicePixelRatio)
      metrics.push({ label: 'dpi-emulation-' + dpr, viewport: '1280x900@' + dpr, range })
      screenshots.push(await capture(cdp, results, 'select-dpi-emulation-' + String(dpr).replace('.', '-') + '-history-range.png'))
    }
    await setViewport(cdp, 1280, 900)

    // 600px: the primary sidebar becomes an overlay drawer; the same panel and
    // select must keep the same geometry inside the drawer.
    await setViewport(cdp, 600, 900)
    await openGit()
    await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench-overlay-panel [data-testid=git-panel]"))'), 10000, 'Git drawer at 600px')
    await openHistory()
    const drawerRange = await (async () => {
      await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
      return cdp.eval(measureScript('[data-testid=git-history-range]'))
    })()
    assertSelect(drawerRange, 'drawer range')
    metrics.push({ label: 'drawer-600', viewport: '600x900', range: drawerRange })
    screenshots.push(await capture(cdp, results, 'select-viewport-600-drawer.png'))
    await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: ' ', code: 'Space', windowsVirtualKeyCode: 32, text: ' ' })
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: ' ', code: 'Space', windowsVirtualKeyCode: 32 })
    await sleep(300)
    check(await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")'), 'drawer range picker did not open')
    screenshots.push(await capture(cdp, results, 'select-viewport-600-drawer-open.png'))
    await press(cdp, 'Escape', 'Escape', 27)
    await sleep(200)

    // A short window must still let the picker open and stay reachable.
    await setViewport(cdp, 1280, 600)
    await openGit()
    await openHistory()
    const shortRange = await (async () => {
      await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
      return cdp.eval(measureScript('[data-testid=git-history-range]'))
    })()
    assertSelect(shortRange, 'short window range')
    metrics.push({ label: 'viewport-1280x600', viewport: '1280x600', range: shortRange })
    screenshots.push(await capture(cdp, results, 'select-short-600-history-range.png'))
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: ' ', code: 'Space', windowsVirtualKeyCode: 32, text: ' ' })
    await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: ' ', code: 'Space', windowsVirtualKeyCode: 32 })
    await sleep(300)
    check(await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")'), 'short window range picker did not open')
    screenshots.push(await capture(cdp, results, 'select-short-600-range-open.png'))
    await press(cdp, 'Escape', 'Escape', 27)
    await sleep(200)

    // Dark English: the shared chrome must resolve dark tokens and the range
    // select must remain usable with the localized Git panel.
    await setViewport(cdp, 1280, 900)
    await cdp.eval("localStorage.setItem('kf-dark','1'); localStorage.setItem('kf-lang','en')")
    await reload(cdp, waitFor)
    await setViewport(cdp, 1280, 900)
    await openGit()
    await openHistory()
    const darkRange = await (async () => {
      await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
      return cdp.eval(measureScript('[data-testid=git-history-range]'))
    })()
    assertSelect(darkRange, 'dark-en range')
    check(darkRange.backgroundImage.includes('a4afb2'), 'dark select arrow did not use the dark token: ' + darkRange.backgroundImage)
    check(await cdp.eval('document.documentElement.lang === "en"'), 'English document language did not apply')
    check(await cdp.eval('document.querySelector("[data-testid=git-tab-history]").textContent.includes("History")'), 'Git panel is not English')
    metrics.push({ label: 'dark-en', viewport: '1280x900', range: darkRange })
    screenshots.push(await capture(cdp, results, 'select-dark-en-1280.png'))

    // Real mouse activation, not just programmatic value changes.
    const rangePoint = { x: darkRange.rect.left + (darkRange.rect.right - darkRange.rect.left) / 2, y: (darkRange.rect.top + darkRange.rect.bottom) / 2 }
    await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: rangePoint.x, y: rangePoint.y })
    await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: rangePoint.x, y: rangePoint.y, button: 'left', clickCount: 1 })
    await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: rangePoint.x, y: rangePoint.y, button: 'left', clickCount: 1 })
    await sleep(350)
    const mouseOpened = await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")')
    check(mouseOpened, 'mouse click did not open the customizable picker')
    await press(cdp, 'Escape', 'Escape', 27)
    await sleep(250)

    // Native semantics: Space opens the real customizable picker, Escape closes
    // it without changing the value, and ArrowDown+Enter commits a value.
    await focusWithKeyboard(cdp, '[data-testid=git-history-range]')
    const selectSupport = {
      baseSelect: await cdp.eval('CSS.supports("appearance", "base-select")'),
      pickerPseudo: await cdp.eval('CSS.supports("selector(::picker(select))")'),
      focused: await cdp.eval('document.activeElement === document.querySelector("[data-testid=git-history-range]")'),
      focusVisible: await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":focus-visible")'),
    }
    check(selectSupport.baseSelect && selectSupport.pickerPseudo, 'customizable select is unavailable: ' + JSON.stringify(selectSupport))
    check(selectSupport.focused && selectSupport.focusVisible, 'range is not keyboard focused: ' + JSON.stringify(selectSupport))

    const openPicker = async () => {
      await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: ' ', code: 'Space', windowsVirtualKeyCode: 32, text: ' ' })
      await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: ' ', code: 'Space', windowsVirtualKeyCode: 32 })
      await sleep(300)
      let isOpen = await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")')
      if (!isOpen) {
        await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'ArrowDown', code: 'ArrowDown', windowsVirtualKeyCode: 40, modifiers: 1 })
        await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'ArrowDown', code: 'ArrowDown', windowsVirtualKeyCode: 40, modifiers: 1 })
        await sleep(300)
        isOpen = await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")')
      }
      return isOpen
    }

    const beforeValue = await cdp.eval('document.querySelector("[data-testid=git-history-range]").value')
    const pickerOpened = await openPicker()
    check(pickerOpened, 'Space did not open the customizable picker')
    await press(cdp, 'ArrowDown', 'ArrowDown', 40)
    await sleep(250)
    screenshots.push(await capture(cdp, results, 'select-picker-open-dark-en.png'))
    await press(cdp, 'Escape', 'Escape', 27)
    await sleep(250)
    const afterEscape = await cdp.eval('({ open: document.querySelector("[data-testid=git-history-range]").matches(":open"), value: document.querySelector("[data-testid=git-history-range]").value })')
    check(!afterEscape.open, 'Escape did not close the picker')
    check(afterEscape.value === beforeValue, 'Escape changed the select value')

    const reopened = await openPicker()
    check(reopened, 'the picker did not reopen after Escape')
    await press(cdp, 'ArrowDown', 'ArrowDown', 40)
    await sleep(150)
    await press(cdp, 'Enter', 'Enter', 13)
    await sleep(500)
    const afterValue = await cdp.eval('document.querySelector("[data-testid=git-history-range]").value')
    const afterCommit = await cdp.eval('document.querySelector("[data-testid=git-history-range]").matches(":open")')
    check(afterValue !== beforeValue, 'keyboard picker did not change the native select value (' + beforeValue + ')')
    check(!afterCommit, 'Enter did not close the picker')
    await waitFor(() => cdp.eval('!document.querySelector("[data-testid=git-running]")'), 20000, 'keyboard history idle')
    // Restore the range for a clean final screenshot.
    await cdp.eval('(() => { const select = document.querySelector("[data-testid=git-history-range]"); const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, "value").set; setter.call(select, ' + JSON.stringify(beforeValue) + '); select.dispatchEvent(new Event("change", { bubbles: true })); })()')
    await waitFor(() => cdp.eval('!document.querySelector("[data-testid=git-running]")'), 20000, 'restored history idle')

    const summary = {
      checkedAt: new Date().toISOString(),
      baseSelect: selectSupport.baseSelect,
      pickerPseudo: selectSupport.pickerPseudo,
      pickerMouseOpened: mouseOpened,
      pickerKeyboard: { opened: pickerOpened, escaped: !afterEscape.open, restoredValue: afterEscape.value, committedValue: afterValue, committedClosed: !afterCommit },
      metrics,
      screenshots,
    }
    writeFileSync(path.join(results, 'select-style-metrics.json'), JSON.stringify(summary, null, 2) + '\n')
    console.log('select-style: ' + metrics.length + ' measurements; base-select=' + summary.baseSelect)
  })
}

await main()
