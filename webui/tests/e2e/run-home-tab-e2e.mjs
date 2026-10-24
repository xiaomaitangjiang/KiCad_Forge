// 真实 WebView2 验收：H1 首页可关闭、空组重开、布局恢复与左侧入口。
// KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/e2e/run-home-tab-e2e.mjs
import { writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, runAppFixture } from '../support/plugin-e2e-harness.mjs'

const KEY = 'workbench.layout.v1'
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms))

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
}

async function capture(cdp, results, name) {
  try {
    const shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
    writeFileSync(path.join(results, name), Buffer.from(shot.data, 'base64'))
  } catch (error) {
    console.log(`screenshot ${name} skipped: ${error.message}`)
  }
}

async function tabState(cdp) {
  return cdp.eval(`(() => ({
    labels: [...document.querySelectorAll('.center-tab-label')].map(node => node.textContent.trim()),
    active: document.querySelector('.center-tab.active .center-tab-label')?.textContent.trim() || null,
    closable: [...document.querySelectorAll('[data-center-tab-close]')].map(node => node.dataset.centerTabClose),
    groups: document.querySelectorAll('[data-center-group]').length,
    placeholder: Boolean(document.querySelector('[data-testid=center-empty-open-home]')),
  }))()`)
}

await runAppFixture('home-tab-e2e', async ({ launch, json, waitFor, results }) => {
  const session = await launch()
  const { cdp, origin } = session
  await waitForWorkbench(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  await setLayout(json, origin, defaultLayout())
  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)

  let state = await tabState(cdp)
  check(state.labels.length === 1 && state.labels[0] === '全部元件', 'home tab missing: ' + JSON.stringify(state))
  check(state.closable.includes('kf:symbols'), 'home tab must expose a close button: ' + JSON.stringify(state))

  await cdp.eval('document.querySelector(".toolbar input")?.focus()')
  await cdp.send('Input.insertText', { text: 'Alpha' })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar input")?.value === "Alpha"'), 3000, 'search value before closing home')

  await cdp.eval('document.querySelector(".center-tab.active .center-tab-close")?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 0'), 4000, 'home tab closes')
  state = await tabState(cdp)
  check(state.groups === 1 && state.placeholder, 'closed home must leave one empty group with a reopen entry: ' + JSON.stringify(state))
  check(await cdp.eval('!document.querySelector(".toolbar")'), 'closing home must unmount the symbol page')
  await capture(cdp, results, 'home-tab-empty-1280-light-zh.png')

  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find(node => node.textContent.trim() === '编辑')?.click()`)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".topbar-dropdown"))'), 3000, 'edit menu after closing home')
  const commands = await cdp.eval(`(() => {
    const items = [...document.querySelectorAll('.topbar-item')]
    const pick = (label) => items.find(node => node.textContent.trim().startsWith(label))
    return {
      copy: pick('复制')?.disabled,
      cut: pick('剪切')?.disabled,
      paste: pick('粘贴')?.disabled,
      undo: pick('撤销')?.disabled,
      redo: pick('重做')?.disabled,
    }
  })()`)
  check(commands.copy === true && commands.cut === true && commands.paste === true
    && commands.undo === true && commands.redo === true,
  'closed home must disable background object commands: ' + JSON.stringify(commands))
  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find(node => node.textContent.trim() === '编辑')?.click()`)

  await waitFor(async () => {
    const saved = await readLayout(json, origin)
    const group = saved.center?.groups?.[0]
    return saved.center?.groups?.length === 1 && group?.pages?.length === 0 && group?.active === null
  }, 4000, 'closed home persisted')

  await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].find(node => node.textContent.includes('全部元件'))?.click()`)
  await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 1'), 4000, 'home reopens from sidebar')
  state = await tabState(cdp)
  check(state.labels[0] === '全部元件' && state.closable.includes('kf:symbols'), 'reopened home must be a singleton tab: ' + JSON.stringify(state))
  check(await cdp.eval('document.querySelector(".toolbar input")?.value === "Alpha"'), 'reopening must keep the search query')
  await capture(cdp, results, 'home-tab-reopened-1280-light-zh.png')

  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  await waitFor(() => cdp.eval('document.querySelector(".center-tab.active .center-tab-label")?.textContent.trim() === "设置"'), 4000, 'settings active before issue shortcut')
  await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].find(node => node.textContent.includes('问题'))?.click()`)
  await waitFor(() => cdp.eval('document.querySelector(".center-tab.active .center-tab-label")?.textContent.trim() === "全部元件"'), 4000, 'issue entry reopens home')
  check(await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].some(node => node.classList.contains('active') && node.textContent.includes('问题'))`), 'issue entry must activate its filter')
  await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].find(node => node.textContent.includes('问题'))?.click()`)
  await waitFor(() => cdp.eval(`![...document.querySelectorAll('.sidebar-navigation .nav-item')].some(node => node.classList.contains('active') && node.textContent.includes('问题'))`), 4000, 'issue filter clears')
  await cdp.eval('document.querySelector(\'[data-center-tab-close="kf:settings"]\')?.click()')
  await waitFor(() => cdp.eval('!document.querySelector(\'[data-center-tab-close="kf:settings"]\')'), 3000, 'settings closed before split')

  await cdp.eval('document.querySelector(\'[data-center-group="g1"] [data-center-split="row"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 2'), 4000, 'split creates a second group')
  await cdp.eval(`document.querySelector('.activitybar button[aria-label="设置"]')?.click()`)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(\'[data-center-group="g2"] [data-center-tab-close="kf:settings"]\'))'), 5000, 'settings opens in the active empty group')

  await cdp.eval('document.querySelector(\'[data-center-group="g1"] [data-center-tab-close="kf:symbols"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll("[data-center-group]").length === 1'), 4000, 'closing home collapses the empty secondary group')
  state = await tabState(cdp)
  check(state.labels.length === 1 && state.labels[0] === '设置', 'settings should remain after collapsing the home group: ' + JSON.stringify(state))

  await cdp.eval('document.querySelector(\'[data-center-tab-close="kf:settings"]\')?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 0'), 4000, 'closing the last page leaves an empty group')
  await waitFor(async () => {
    const saved = await readLayout(json, origin)
    return saved.center?.groups?.length === 1 && saved.center.groups[0].pages.length === 0
  }, 4000, 'empty last group persisted')

  await reload(cdp, waitFor)
  await setViewport(cdp, 1280, 900)
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=center-empty-open-home]"))'), 5000, 'empty center survives reload')
  state = await tabState(cdp)
  check(state.labels.length === 0 && state.groups === 1, 'reload must not force the home tab back: ' + JSON.stringify(state))

  await cdp.eval('document.querySelector("[data-testid=center-empty-open-home]")?.click()')
  await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 1'), 4000, 'placeholder reopens home')
  state = await tabState(cdp)
  check(state.labels[0] === '全部元件', 'placeholder must reopen the home tab: ' + JSON.stringify(state))

  const legacy = {
    version: 2,
    primarySidebar: { visible: true, width: 280, activeViewId: 'kf:workspace' },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: false, height: 220, activeViewId: 'tasks' },
    center: {
      activeGroupId: 'g1',
      nextId: 2,
      tree: { type: 'group', groupId: 'g1', size: 100 },
      groups: [{ id: 'g1', pages: ['kf:symbols', 'kf:settings'], active: 'kf:settings', size: 100 }],
    },
  }
  await setLayout(json, origin, legacy)
  await reload(cdp, waitFor)
  await waitFor(() => cdp.eval('document.querySelectorAll(".center-tab-label").length === 2'), 5000, 'legacy center layout loads')
  state = await tabState(cdp)
  check(state.labels.includes('全部元件') && state.active === '设置', 'legacy center pages and active page must survive: ' + JSON.stringify(state))
  check(session.errors.every(message => !/home tab|Uncaught/.test(message)), 'unexpected browser errors: ' + session.errors.join(' | '))
})
