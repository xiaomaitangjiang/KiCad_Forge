// 交付复审真实 WebView2 验收：
// N1 旧刷新不得覆盖用户新查询；O1 旧 output 布局迁移为 tasks 且可持久化。
// KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/run-delivery-review-e2e.mjs
import { check, runAppFixture } from './plugin-e2e-harness.mjs'
import { seedEditingFixture } from './editing-fixtures.mjs'

const MOD = { ctrl: 2 }
const LAYOUT_KEY = 'workbench.layout.v1'
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms))

async function key(cdp, { key: value, code, vk, modifiers = 0 }) {
  const base = { modifiers, key: value, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk }
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base })
}

async function clickCard(cdp, name) {
  const point = await cdp.eval(`(() => {
    const card = [...document.querySelectorAll('.symbol-card')].find(n => n.querySelector('.symbol-name')?.textContent.trim() === ${JSON.stringify(name)})
    if (!card) return null
    card.scrollIntoView({ block: 'center' })
    const r = card.getBoundingClientRect()
    return { x: r.x + Math.min(40, r.width / 2), y: r.y + r.height / 2 }
  })()`)
  check(point, `symbol card missing: ${name}`)
  await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: point.x, y: point.y, button: 'left', buttons: 1, clickCount: 1 })
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: point.x, y: point.y, button: 'left', buttons: 0, clickCount: 1 })
  await sleep(80)
}

async function chooseTarget(cdp, libraryId) {
  await cdp.eval(`(() => {
    const select = document.querySelector('.paste-target-modal select')
    if (!select) return false
    const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set
    setter.call(select, ${JSON.stringify(libraryId)})
    select.dispatchEvent(new Event('change', { bubbles: true }))
    return true
  })()`)
  await sleep(80)
}

async function setLayout(json, origin, layout) {
  await json(`${origin}/api/config`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ [LAYOUT_KEY]: JSON.stringify(layout) }),
  })
}

async function readLayout(json, origin) {
  const config = await json(`${origin}/api/config`)
  const raw = config?.[LAYOUT_KEY]
  check(typeof raw === 'string' && raw.length > 0, `config has no ${LAYOUT_KEY}: ${JSON.stringify(config)}`)
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

function bottomTabs(cdp) {
  return cdp.eval(`[...document.querySelectorAll('.bottom-panel-tabs [role=tab]')].map((tab) => ({
    view: tab.dataset.view || '',
    selected: tab.getAttribute('aria-selected') === 'true',
  }))`)
}

function assertMigratedTabs(tabs, label) {
  const views = tabs.map(tab => tab.view)
  check(JSON.stringify(views) === JSON.stringify(['tasks', 'problems']),
    `${label}: bottom tabs must be tasks/problems only, got ${JSON.stringify(tabs)}`)
  check(tabs.find(tab => tab.view === 'tasks')?.selected === true,
    `${label}: migrated output layout must select tasks: ${JSON.stringify(tabs)}`)
}

await runAppFixture('delivery-review-n1', async ({ root, launch, stop, json, waitFor }) => {
  let session = await launch()
  await waitFor(() => session.cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'first workbench boot')
  await stop(session)

  seedEditingFixture(root)
  session = await launch()
  const { cdp, origin } = session
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
  await waitFor(() => cdp.eval('document.querySelectorAll(".symbol-card").length === 4'), 30000, 'four seeded cards')

  // 触发一次真实写入（跨库移动 Beta），随后冻结刷新探测。
  await clickCard(cdp, 'Beta')
  await key(cdp, { key: 'x', code: 'KeyX', vk: 88, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelectorAll(".symbol-card.pending-cut").length === 1'), 5000, 'cut marker')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".paste-target-modal"))'), 5000, 'move dialog')
  await chooseTarget(cdp, 'e2e-lib-b')
  await cdp.eval(`(() => {
    const original = window.fetch.bind(window)
    window.__reviewGate = { hit: 0, release: null }
    window.fetch = (input, init) => {
      const url = typeof input === 'string' ? input : (input && input.url) || ''
      if (window.__reviewGate.hit === 0 && url.includes('/api/symbols')) {
        window.__reviewGate.hit = 1
        return new Promise((resolve, reject) => {
          window.__reviewGate.release = () => original(input, init).then(resolve, reject)
        })
      }
      return original(input, init)
    }
    return true
  })()`)
  await cdp.eval('document.querySelector(".paste-target-modal .btn-primary")?.click()')
  await waitFor(() => cdp.eval('!document.querySelector(".paste-target-modal")'), 10000, 'move commit closes dialog')
  await waitFor(() => cdp.eval('window.__reviewGate?.hit === 1'), 15000, 'refresh symbols probe paused')
  check(await cdp.eval('typeof window.__reviewGate.release === "function"'), 'paused probe has no release hook')

  // 用户在新刷新仍在途时改查询：旧刷新返回后不得回写旧列表/旧查询。
  await cdp.eval('document.querySelector(".toolbar-search")?.focus()')
  await cdp.send('Input.insertText', { text: 'Gamma' })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "Gamma"'), 5000, 'user search Gamma')
  await waitFor(() => cdp.eval('document.querySelectorAll(".symbol-card").length === 1'), 10000, 'Gamma result count')
  const visibleName = await cdp.eval('document.querySelector(".symbol-card .symbol-name")?.textContent.trim()')
  check(visibleName === 'Gamma', `user search shows ${visibleName}, expected Gamma`)

  const historyBefore = await json(`${origin}/api/editing/history`)
  await cdp.eval('(() => { const release = window.__reviewGate.release; window.__reviewGate.release = null; release?.(); return true })()')
  await sleep(1800)

  check(await cdp.eval('document.querySelector(".toolbar-search")?.value === "Gamma"'),
    'stale refresh overwrote the user query')
  check(await cdp.eval('document.querySelectorAll(".symbol-card").length === 1'),
    'stale refresh published the old symbol list')
  const afterName = await cdp.eval('document.querySelector(".symbol-card .symbol-name")?.textContent.trim()')
  check(afterName === 'Gamma', `stale refresh replaced the visible result: ${afterName}`)
  const historyAfter = await json(`${origin}/api/editing/history`)
  check(historyAfter.count === historyBefore.count,
    `refresh retried or replayed a write: history ${historyBefore.count} -> ${historyAfter.count}`)
  await stop(session)
})

await runAppFixture('delivery-review-o1', async ({ launch, stop, json, waitFor }) => {
  let session = await launch()
  let { cdp, origin } = session
  await waitForWorkbench(cdp, waitFor)

  const legacy = {
    version: 1,
    primarySidebar: { visible: true, width: 280, activeViewId: 'kf:workspace' },
    auxiliarySidebar: { visible: true, width: 280, activeViewId: null },
    bottomPanel: { visible: true, height: 300, activeViewId: 'output' },
  }
  await setLayout(json, origin, legacy)
  await reload(cdp, waitFor)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 5000, 'migrated bottom panel')
  assertMigratedTabs(await bottomTabs(cdp), 'legacy output load')
  let snapshot = await cdp.eval(`(() => {
    const panel = document.querySelector('.bottom-panel')
    return panel ? panel.getBoundingClientRect().height : 0
  })()`)
  check(Math.abs(snapshot - 300) <= 2, `legacy bottom height lost: ${snapshot}`)
  check(await cdp.eval('!document.querySelector(".bottom-panel-tabs [data-view=output]")'),
    'legacy output tab is still rendered')
  check(await cdp.eval('document.body.innerText.includes("输出") === false'),
    'legacy output entry text is still visible')

  // 关闭再打开，强制一次布局保存；落盘必须是 tasks，不能写回 output。
  await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector(".bottom-panel")'), 3000, 'hide migrated bottom panel')
  await key(cdp, { key: 'j', code: 'KeyJ', vk: 74, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 3000, 'show migrated bottom panel')
  const saved = await waitFor(async () => {
    const layout = await readLayout(json, origin)
    return layout.bottomPanel?.activeViewId === 'tasks' ? layout : null
  }, 5000, 'migrated bottom tab saved')
  check(saved.bottomPanel.visible === true && saved.bottomPanel.height === 300,
    `migration changed visibility/height: ${JSON.stringify(saved.bottomPanel)}`)
  check(saved.center && saved.primarySidebar?.width === 280 && saved.auxiliarySidebar?.width === 280,
    `migration changed other layout fields: ${JSON.stringify(saved)}`)
  assertMigratedTabs(await bottomTabs(cdp), 'after save')

  await reload(cdp, waitFor)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 5000, 'bottom panel after reload')
  assertMigratedTabs(await bottomTabs(cdp), 'after reload')
  const heightAfterReload = await cdp.eval(`document.querySelector('.bottom-panel')?.getBoundingClientRect().height || 0`)
  check(Math.abs(heightAfterReload - 300) <= 2, `bottom height after reload ${heightAfterReload}`)

  await stop(session)
  session = await launch()
  cdp = session.cdp
  await waitForWorkbench(cdp, waitFor)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".bottom-panel"))'), 5000, 'bottom panel after restart')
  assertMigratedTabs(await bottomTabs(cdp), 'after restart')
  const heightAfterRestart = await cdp.eval(`document.querySelector('.bottom-panel')?.getBoundingClientRect().height || 0`)
  check(Math.abs(heightAfterRestart - 300) <= 2, `bottom height after restart ${heightAfterRestart}`)
  await stop(session)
})
