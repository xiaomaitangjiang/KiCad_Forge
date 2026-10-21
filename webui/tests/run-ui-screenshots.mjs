// U1 界面验收截图：五档窗口 + 亮/暗 + 中/英 + 编辑菜单。
// KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/run-ui-screenshots.mjs
import { mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, runAppFixture } from './plugin-e2e-harness.mjs'
import { seedEditingFixture } from './editing-fixtures.mjs'

const VIEWPORTS = [[1920, 1080], [1280, 900], [960, 640], [800, 600], [600, 800]]
const NAMES_A = ['Alpha', 'Beta', 'VeryLongSymbolNameForLayoutCheck']
const NAMES_B = ['Gamma', 'Delta']
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms))

async function setViewport(cdp, width, height) {
  await cdp.send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false })
  await sleep(350)
  const inner = await cdp.eval('({ width: window.innerWidth, height: window.innerHeight })')
  check(inner?.width === width && inner?.height === height, `viewport ${width}x${height}: ${JSON.stringify(inner)}`)
}

async function waitForCards(cdp, waitFor, expected) {
  await waitFor(() => cdp.eval(`document.querySelectorAll('.symbol-card').length === ${expected}`), 30000,
    `${expected} symbol cards`)
}

async function capture(cdp, results, name) {
  const shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
  const file = path.join(results, 'ui-acceptance', name)
  writeFileSync(file, Buffer.from(shot.data, 'base64'))
  return file
}

async function measure(cdp) {
  return cdp.eval(`(() => {
    const box = (selector) => {
      const node = document.querySelector(selector)
      if (!node) return null
      const r = node.getBoundingClientRect()
      return { width: Math.round(r.width * 10) / 10, x: Math.round(r.x * 10) / 10 }
    }
    const style = (selector) => {
      const node = document.querySelector(selector)
      return node ? getComputedStyle(node) : null
    }
    return {
      scrollWidth: document.documentElement.scrollWidth,
      bodyScrollWidth: document.body.scrollWidth,
      center: box('[data-testid=workbench-center-column]'),
      toolbar: box('.toolbar'),
      actionLabelDisplay: style('.toolbar-action-label')?.display || '',
      headingMetaDisplay: style('.center-heading-meta')?.display || '',
      searchDisplay: style('.toolbar-search')?.display || '',
      dark: document.querySelector('.app.dark') !== null,
      toolbarLabel: document.querySelector('.toolbar')?.getAttribute('aria-label') || '',
    }
  })()`)
}

async function reload(cdp, waitFor) {
  const before = Number(await cdp.eval('performance.timeOrigin'))
  await cdp.send('Page.reload')
  await waitFor(() => cdp.eval(`performance.timeOrigin !== ${before}`), 30000, 'document reload')
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
}

await runAppFixture('ui-screenshots', async ({ root, launch, stop, waitFor, results }) => {
  let session = await launch()
  await waitFor(() => session.cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'first workbench boot')
  await stop(session)

  seedEditingFixture(root, { namesA: NAMES_A, namesB: NAMES_B })
  session = await launch()
  const { cdp } = session
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
  await waitForCards(cdp, waitFor, 5)

  const output = path.join(results, 'ui-acceptance')
  mkdirSync(output, { recursive: true })
  const shots = []
  const sizes = []
  const darkLight = []
  for (const [width, height] of VIEWPORTS) {
    await setViewport(cdp, width, height)
    const measured = await measure(cdp)
    check(measured.scrollWidth <= width + 1 && measured.bodyScrollWidth <= width + 1,
      `horizontal overflow at ${width}x${height}: ${measured.scrollWidth}/${measured.bodyScrollWidth}`)
    sizes.push({ viewport: `${width}x${height}`, ...measured })
    shots.push(await capture(cdp, results, `ui-${width}x${height}-light-zh.png`))
  }

  // 选中对象后打开编辑菜单，记录六命令状态与中文菜单的视觉层级。
  await setViewport(cdp, 1280, 900)
  await cdp.eval(`document.querySelector('.symbol-card')?.click()`)
  await sleep(120)
  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find(n => n.textContent.trim() === '编辑')?.click()`)
  await waitFor(() => cdp.eval('document.querySelectorAll(".topbar-dropdown [role=menuitem]").length === 6'), 3000,
    'edit menu six items')
  shots.push(await capture(cdp, results, 'ui-1280x900-light-zh-menu.png'))
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".topbar-dropdown")'), 3000, 'menu closes')

  // 内置插件只出现在左侧活动栏 + 主侧栏（WP-001/002）：记录完整面板视觉。
  const selectActivity = (title) => cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === ${JSON.stringify(title)})?.click()`)
  await selectActivity('Git')
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-panel] [data-testid=git-target]"))'), 10000, 'Git sidebar panel')
  await waitFor(() => cdp.eval(`!document.querySelector('[data-testid=git-running]') && Boolean(document.querySelector('[data-testid=git-uninitialized],[data-testid=git-clean],[data-testid=git-changes],[data-testid=git-task-result]'))`), 20000, 'Git repository state')
  shots.push(await capture(cdp, results, 'ui-1280x900-light-zh-git.png'))
  await selectActivity('从 LCSC 导入')
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=lcsc-panel] [data-testid=lcsc-id-input]"))'), 10000, 'LCSC sidebar panel')
  await sleep(300)
  shots.push(await capture(cdp, results, 'ui-1280x900-light-zh-lcsc.png'))

  // 窄窗：左抽屉承载同一完整面板，中心仍然可见且被遮罩。
  await setViewport(cdp, 600, 800)
  await selectActivity('工作空间')
  await selectActivity('从 LCSC 导入')
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench-overlay-panel [data-testid=lcsc-panel]"))'), 10000, 'LCSC drawer at 600px')
  await sleep(300)
  shots.push(await capture(cdp, results, 'ui-600x800-light-zh-lcsc-drawer.png'))
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".workbench-overlay-panel")'), 3000, 'drawer closes')
  await selectActivity('工作空间')
  await setViewport(cdp, 1280, 900)

  // 暗色 + 英文：刷新后应用读取 localStorage 中的 kf-dark / kf-lang。
  await cdp.eval("localStorage.setItem('kf-dark', '1'); localStorage.setItem('kf-lang', 'en')")
  await reload(cdp, waitFor)
  await waitForCards(cdp, waitFor, 5)
  for (const [width, height] of VIEWPORTS) {
    await setViewport(cdp, width, height)
    const measured = await measure(cdp)
    check(measured.dark === true, `dark class at ${width}x${height}`)
    check(measured.toolbarLabel === 'Symbol toolbar', `english toolbar label at ${width}x${height}: ${measured.toolbarLabel}`)
    check(measured.scrollWidth <= width + 1 && measured.bodyScrollWidth <= width + 1,
      `dark english overflow at ${width}x${height}: ${measured.scrollWidth}/${measured.bodyScrollWidth}`)
    darkLight.push({ viewport: `${width}x${height}`, ...measured })
    shots.push(await capture(cdp, results, `ui-${width}x${height}-dark-en.png`))
  }

  writeFileSync(path.join(output, 'summary.json'), JSON.stringify({
    sizes,
    darkLight,
    screenshots: shots.map(file => path.basename(file)),
  }, null, 2) + '\n')
  await stop(session)
})
