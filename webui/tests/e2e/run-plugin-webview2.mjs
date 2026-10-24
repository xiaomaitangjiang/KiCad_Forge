// Real WebView2 acceptance using a disposable empty-data package copy.
// KFORGE_BUILD_ROOT must point to a prepared CI package, never a live build.
import { cpSync, existsSync, mkdirSync, readdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { check, runAppFixture } from '../support/plugin-e2e-harness.mjs'

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..')
const examplesRoot = path.join(repoRoot, 'examples', 'plugins')
const nestedParentId = 'test.nested-parent'
const nestedChildId = 'test.nested-child'
const nestedLeafId = 'test.nested-leaf'

// These fixtures exist only in the disposable package. Separate plugins keep
// the child consumers alive while their parent's slot declarations disappear.
function stageNestedSlotFixtures(pluginsDir) {
  const fixtures = [
    {
      id: nestedParentId,
      source: `
import React from 'react'
import { cardAccessory, definePlugin, defineSlot } from '@kiforge/plugin-sdk'
const body = defineSlot('test.nested.body', 'list')
let generation = 0
export default definePlugin({
  name: 'test.nested-parent',
  apply(ctx) {
    const value = 'round-' + (++generation)
    ctx.slots.register(cardAccessory, {
      id: 'nested-parent',
      children: { [body.name]: { kind: 'list' } },
      render: ({ renderSlot }) => React.createElement('section', {
        'data-testid': 'nested-parent',
      }, renderSlot(body, { value })),
    })
  },
})
`,
    },
    {
      id: nestedChildId,
      source: `
import React from 'react'
import { definePlugin, defineSlot } from '@kiforge/plugin-sdk'
const body = defineSlot('test.nested.body', 'list')
const leaf = defineSlot('test.nested.leaf', 'list')
export default definePlugin({
  name: 'test.nested-child',
  apply(ctx) {
    ctx.slots.onDeclared(body, (available) => {
      if (!available) return
      ctx.slots.register(body, {
        id: 'nested-child',
        children: { [leaf.name]: { kind: 'list' } },
        render: ({ value, renderSlot }) => React.createElement('article', {
          'data-testid': 'nested-child',
        }, renderSlot(leaf, { value: value + '/child' })),
      })
    })
  },
})
`,
    },
    {
      id: nestedLeafId,
      source: `
import React from 'react'
import { definePlugin, defineSlot } from '@kiforge/plugin-sdk'
const leaf = defineSlot('test.nested.leaf', 'list')
export default definePlugin({
  name: 'test.nested-leaf',
  apply(ctx) {
    ctx.slots.onDeclared(leaf, (available) => {
      if (!available) return
      ctx.slots.register(leaf, {
        id: 'nested-leaf',
        render: ({ value }) => React.createElement('span', {
          'data-testid': 'nested-leaf',
        }, value + '/leaf'),
      })
    })
  },
})
`,
    },
  ]
  for (const fixture of fixtures) {
    const target = path.join(pluginsDir, fixture.id)
    mkdirSync(path.join(target, 'dist'), { recursive: true })
    writeFileSync(path.join(target, 'manifest.json'), JSON.stringify({
      manifestVersion: 2,
      id: fixture.id,
      name: fixture.id,
      version: '1.0.0',
      license: 'MIT',
      engines: { sdk: '^1.0.0' },
      frontend: { root: 'dist', entry: 'index.js' },
    }))
    writeFileSync(path.join(target, 'dist', 'index.js'), fixture.source)
  }
}


function stageSidebarFixtures(pluginsDir) {
  const fixtures = [
    {
      id: 'test.sidebar-a',
      source: `import React from 'react'
import { definePlugin, sidebarPanel } from '@kiforge/plugin-sdk'
function BrokenIcon() { throw new Error('sidebar icon boom') }
export default definePlugin({
  name: 'test.sidebar-a',
  apply(ctx) {
    ctx.slots.register(sidebarPanel, {
      id: 'shared',
      title: 'Sidebar A',
      icon: React.createElement(BrokenIcon),
      render: () => { throw new Error('sidebar panel boom') },
    })
  },
})
`,
    },
    {
      id: 'test.sidebar-b',
      source: `import React from 'react'
import { definePlugin, sidebarPanel } from '@kiforge/plugin-sdk'
export default definePlugin({
  name: 'test.sidebar-b',
  apply(ctx) {
    ctx.slots.register(sidebarPanel, {
      id: 'shared',
      title: 'Sidebar B',
      render: () => React.createElement('div', { 'data-testid': 'sidebar-panel-b' }, 'B panel'),
    })
  },
})
`,
    },
    {
      id: 'test.sidebar-section',
      source: `import React from 'react'
import { definePlugin, sidebarSection } from '@kiforge/plugin-sdk'
export default definePlugin({
  name: 'test.sidebar-section',
  apply(ctx) {
    ctx.slots.register(sidebarSection, {
      id: 'legacy-section',
      title: 'Legacy section',
      render: () => React.createElement('div', { 'data-testid': 'legacy-section' }, 'legacy section'),
    })
  },
})
`,
    },
  ]
  for (const fixture of fixtures) {
    const target = path.join(pluginsDir, fixture.id)
    mkdirSync(path.join(target, 'dist'), { recursive: true })
    writeFileSync(path.join(target, 'manifest.json'), JSON.stringify({
      manifestVersion: 2,
      id: fixture.id,
      name: fixture.id,
      version: '1.0.0',
      license: 'MIT',
      engines: { sdk: '^1.1.0' },
      frontend: { root: 'dist', entry: 'index.js' },
    }))
    writeFileSync(path.join(target, 'dist', 'index.js'), fixture.source)
  }
}

async function verifySidebarContributionIsolation(cdp, waitFor) {
  await waitFor(() => cdp.eval(`Boolean([...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === 'Sidebar B'))`), 10000, 'sidebar panel fixtures')
  check(await cdp.eval(`Boolean(document.querySelector('[data-testid=legacy-section]'))`), 'legacy sidebar.section is not embedded in the workspace')
  check(await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].filter((item) => item.title === 'Sidebar A' || item.title === 'Sidebar B').length === 2`), 'same local id must not collapse panels from two plugins')

  await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === 'Sidebar A')?.click()`)
  await waitFor(() => cdp.eval(`Boolean(document.querySelector('.kf-plugin-error'))`), 5000, 'sidebar A panel error boundary')
  check(await cdp.eval(`!document.querySelector('[data-testid=sidebar-panel-b]')`), 'A panel failure leaked into B')

  await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === 'Sidebar B')?.click()`)
  await waitFor(() => cdp.eval(`Boolean(document.querySelector('[data-testid=sidebar-panel-b]'))`), 5000, 'sidebar B panel')
  check(await cdp.eval(`!document.querySelector('.kf-plugin-error')`), 'switching to B must clear A boundary state')

  await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === 'Sidebar A')?.click()`)
  await waitFor(() => cdp.eval(`Boolean(document.querySelector('.kf-plugin-error'))`), 5000, 'A panel error again')
  check(await cdp.eval(`window.__KF_APP_RUNTIME__.host.diagnostics.some((item) => item.code === 'plugin/icon-error' && item.plugin === 'test.sidebar-a')`), 'icon failure was not isolated on the activity bar')

  await cdp.eval(`window.__KF_APP_RUNTIME__.loader.setEnabled('test.sidebar-a', false)`)
  await waitFor(() => cdp.eval(`![...document.querySelectorAll('.activitybar .nav-item')].some((item) => item.title === 'Sidebar A')`), 5000, 'A activity withdrawn')
  check(await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].some((item) => item.title === 'Sidebar B')`), 'disabling A removed B')
  await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === 'Sidebar B')?.click()`)
  await waitFor(() => cdp.eval(`Boolean(document.querySelector('[data-testid=sidebar-panel-b]'))`), 5000, 'B still renders after A disabled')

  await cdp.eval(`window.__KF_APP_RUNTIME__.loader.setEnabled('test.sidebar-a', true)`)
  await waitFor(() => cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].some((item) => item.title === 'Sidebar A')`), 5000, 'A activity restored')
  await cdp.eval(`[...document.querySelectorAll('.activitybar .nav-item')].find((item) => item.title === '工作空间')?.click()`)
  await waitFor(() => cdp.eval(`Boolean(document.querySelector('[data-testid=legacy-section]'))`), 5000, 'workspace view after panel switching')
}

function stageExamples(root) {
  const pluginsDir = path.join(root, 'plugins')
  mkdirSync(pluginsDir, { recursive: true })
  for (const entry of readdirSync(examplesRoot, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue
    const source = path.join(examplesRoot, entry.name)
    if (!existsSync(path.join(source, 'manifest.json'))) continue
    check(existsSync(path.join(source, 'dist')), `build example plugin first: ${source}`)
    const target = path.join(pluginsDir, entry.name)
    mkdirSync(target, { recursive: true })
    for (const name of ['manifest.json', 'icon.svg', 'dist']) {
      if (existsSync(path.join(source, name))) cpSync(path.join(source, name), path.join(target, name), { recursive: true })
    }
  }
  stageNestedSlotFixtures(pluginsDir)
  stageSidebarFixtures(pluginsDir)
}

async function verifyNestedSlotLifecycle(cdp, waitFor) {
  const snapshot = () => cdp.eval(`({
    parent: document.querySelectorAll('[data-testid=nested-parent]').length,
    child: document.querySelectorAll('[data-testid=nested-child]').length,
    leaf: document.querySelectorAll('[data-testid=nested-leaf]').length,
    text: document.querySelector('[data-testid=nested-leaf]')?.textContent,
    bodyEntries: window.__KF_PROTOTYPE__.host.count('test.nested.body'),
    leafEntries: window.__KF_PROTOTYPE__.host.count('test.nested.leaf'),
  })`)
  const restored = async (generation) => {
    const expectedText = `round-${generation}/child/leaf`
    await waitFor(async () => (await snapshot()).text === expectedText, 5000, `nested slot generation ${generation}`)
    const state = await snapshot()
    check(state.parent === 1 && state.child === 1 && state.leaf === 1,
      `nested contributions must render exactly once: ${JSON.stringify(state)}`)
    check(state.bodyEntries === 1 && state.leafEntries === 1,
      `nested registrations must exist exactly once: ${JSON.stringify(state)}`)
    check(await cdp.eval(`Boolean(document.querySelector(
      '[data-testid=nested-parent] > [data-testid=nested-child] > [data-testid=nested-leaf]'
    ))`), 'nested contributions did not render the parent/child/leaf DOM tree')
  }
  await restored(1)
  for (const generation of [2, 3]) {
    await cdp.eval(`window.__KF_PROTOTYPE__.disable(${JSON.stringify(nestedParentId)})`)
    await waitFor(async () => {
      const state = await snapshot()
      return state.parent === 0 && state.child === 0 && state.leaf === 0
        && state.bodyEntries === 0 && state.leafEntries === 0
    }, 5000, 'nested parent withdrawal and recursive cleanup')
    const consumers = await cdp.eval(`window.__KF_PROTOTYPE__.plugins().filter(
      (plugin) => ${JSON.stringify([nestedChildId, nestedLeafId])}.includes(plugin.id)
    )`)
    check(consumers.length === 2 && consumers.every((plugin) => plugin.status === 'active'),
      `nested consumers must survive parent withdrawal: ${JSON.stringify(consumers)}`)
    await cdp.eval(`window.__KF_PROTOTYPE__.enable(${JSON.stringify(nestedParentId)})`)
    await restored(generation)
  }
}

async function runScenarios(cdp, consoleErrors, { request, json, waitFor }) {
  await cdp.send('Page.navigate', { url: `${new URL(cdp.__origin).origin}/prototype/` })
  await waitFor(() => cdp.eval('Boolean(window.__KF_PROTOTYPE__)'), 20000, 'prototype boot')
  await cdp.eval('window.__KF_PROTOTYPE__.ready.then(() => true)')

  // Dependency restoration can still be reconciling after initial discovery.
  // Wait for the same observable state the UI requires, with a bounded timeout.
  const plugins = await waitFor(async () => {
    const list = await cdp.eval('window.__KF_PROTOTYPE__.plugins()')
    check(list.length > 0 && list.every((plugin) => plugin.status === 'active'),
      `plugins not active: ${JSON.stringify(list)}`)
    return list
  }, 10000, 'all fixture plugins active')
  const expectedIds = [
    'demo.supplier', 'demo.supply-page', 'demo.supply-tab',
    nestedParentId, nestedChildId, nestedLeafId,
    'test.sidebar-a', 'test.sidebar-b', 'test.sidebar-section',
  ]
  check(plugins.length === expectedIds.length && expectedIds.every((id) => plugins.some((plugin) => plugin.id === id)),
    `unexpected plugin fixture set: ${JSON.stringify(plugins)}`)
  check(plugins.every((plugin) => plugin.status === 'active'), `plugins not active: ${JSON.stringify(plugins)}`)

  const counts = await cdp.eval('window.__KF_PROTOTYPE__.counts()')
  check(counts.page === 1 && counts.aux === 1 && counts.tabs === 1, `bad initial counts: ${JSON.stringify(counts)}`)

  const identity = await cdp.eval('window.__KF_PROTOTYPE__.runtimeChecks')
  check(identity.reactSame === true, `plugin React identity mismatch: ${JSON.stringify(identity)}`)
  check(identity.pluginReactVersion === identity.reactVersion, `React version mismatch: ${JSON.stringify(identity)}`)

  await verifyNestedSlotLifecycle(cdp, waitFor)

  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'supply page missing')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 1'), 'supply tab missing')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supplier-status]").length === 1'), 'supplier status missing')
  check(await cdp.eval('document.querySelector("[data-testid=supply-page]").innerText.includes("价格 ¥1.23")'), 'plugin config defaults not applied')
  check(await cdp.eval('getComputedStyle(document.querySelector("[data-testid=supply-page]")).borderLeftWidth === "4px"'), 'plugin stylesheet not applied')
  check(await cdp.eval('Boolean(document.querySelector("link[data-kforge-plugin=\\"demo.supply-page\\"]"))'), 'plugin stylesheet link missing')

  await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supply-page")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 0'), 5000, 'page withdrawal')
  check(await cdp.eval('window.__KF_PROTOTYPE__.counts().page === 0'), 'page contribution survived page withdrawal')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 0'), 'page still visible')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 0'), 'tab survived page withdrawal')
  check(await cdp.eval('!document.querySelector("link[data-kforge-plugin=\\"demo.supply-page\\"]")'), 'plugin stylesheet survived page withdrawal')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supplier-status]").length === 1'), 'service consumer did not survive page withdrawal')

  await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supply-page")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 1'), 5000, 'page restore')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'page not restored')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 1'), 'tab not restored once')
  await waitFor(() => cdp.eval('getComputedStyle(document.querySelector("[data-testid=supply-page]")).borderLeftWidth === "4px"'), 5000, 'plugin stylesheet restored')

  await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supplier")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().page === 0 && window.__KF_PROTOTYPE__.counts().aux === 0'), 5000, 'provider withdrawal')
  await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supplier")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 1 && window.__KF_PROTOTYPE__.counts().aux === 1'), 5000, 'provider restore')
  check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'page not restored after provider recovery')

  await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supply-tab")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().aux === 0 && window.__KF_PROTOTYPE__.counts().tabs === 0'), 5000, 'dependent withdrawal')
  await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supply-tab")')
  await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().aux === 1 && window.__KF_PROTOTYPE__.counts().tabs === 1'), 5000, 'dependent restore')

  // End-to-end plugin config: write through the host route, reload the plugin,
  // observe the UI follow the persisted value, then restore the default.
  const configUrl = `${cdp.__origin}/api/plugin-config/demo.supply-page`
  const writeConfig = async (value) => {
    const response = await request(configUrl, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(value),
    })
    check(response.ok, `plugin config write failed: HTTP ${response.status}`)
  }
  const reloadPagePlugin = async () => {
    await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supply-page")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().page === 0'), 5000, 'page config reload off')
    await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supply-page")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().page === 1'), 5000, 'page config reload on')
  }

  await writeConfig({ showPrices: false })
  await reloadPagePlugin()
  check(await cdp.eval('document.querySelector("[data-testid=supply-page]").innerText.includes("价格已隐藏")'), 'persisted plugin config not applied')
  const readBack = await json(configUrl)
  check(readBack?.ok === true && readBack?.config?.showPrices === false, `plugin config read-back failed: ${JSON.stringify(readBack)}`)

  await writeConfig({ showPrices: true })
  await reloadPagePlugin()
  check(await cdp.eval('document.querySelector("[data-testid=supply-page]").innerText.includes("价格 ¥1.23")'), 'plugin config restore failed')

  const diagnostics = await cdp.eval('window.__KF_PROTOTYPE__.errors')
  check(!diagnostics.length, `diagnostics: ${JSON.stringify(diagnostics)}`)

  // Main window: the same plugin registers a view into app.view; the sidebar
  // lists it and selecting it renders the plugin view inside the real app.
  await cdp.send('Page.navigate', { url: `${new URL(cdp.__origin).origin}/` })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".app"))'), 20000, 'main app boot')
  await verifySidebarContributionIsolation(cdp, waitFor)
  await waitFor(() => cdp.eval('[...document.querySelectorAll(".nav-item")].some((item) => item.textContent.includes("供应信息"))'), 10000, 'main app plugin view entry').catch(async (error) => {
    console.error('MAIN APP BODY=', (await cdp.eval('document.body.innerText')).slice(0, 1500))
    console.error('MAIN APP CONSOLE=', consoleErrors.join(' | '))
    console.error('MAIN APP RUNTIME=', JSON.stringify(await cdp.eval('window.__KF_APP_RUNTIME__ ? { ...window.__KF_APP_RUNTIME__.loader.getSnapshot(), diagnostics: window.__KF_APP_RUNTIME__.host.diagnostics } : null')))
    throw error
  })
  check(await cdp.eval('[...document.querySelectorAll(".nav-item")].filter((item) => item.textContent.includes("Git Commit")).length === 0'), 'legacy Git button still rendered for frontend plugin')
  await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "Git")?.click()')
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=git-panel] [data-testid=git-target]"))'), 10000, 'Git sidebar panel')
  check(await cdp.eval('Boolean(document.querySelector(".gf-git-details [data-testid=git-target]"))'), 'Git sidebar panel missing')
  const gitOptionCount = await cdp.eval('document.querySelectorAll("[data-testid=git-target] option").length')
  if (gitOptionCount === 0) {
    throw new Error(`Git library service returned no options: ${await cdp.eval('document.querySelector("[data-testid=git-panel]").innerText')}`)
  }
  check(await cdp.eval('[...document.querySelectorAll(".nav-item")].filter((item) => item.textContent.includes("Import LCSC")).length === 0'), 'legacy LCSC button still rendered for frontend plugin')
  await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "从 LCSC 导入")?.click()')
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=lcsc-panel] [data-testid=lcsc-id-input]"))'), 10000, 'LCSC sidebar panel')
  check(await cdp.eval('document.querySelector("[data-testid=lcsc-inline]") && !document.querySelector("[data-testid=lcsc-inline]").closest(".modal")'), 'LCSC must not be a modal')

  // True disable: the frontend loader asks the backend admission gate, removes
  // the plugin UI, and re-enabling restores it after the admission gate opens.
  await cdp.eval('window.__KF_APP_RUNTIME__.loader.setEnabled("com.kicad_forge.lcsc_import", false)')
  await waitFor(() => cdp.eval('!document.querySelector("[data-testid=lcsc-inline]") && ![...document.querySelectorAll(".activitybar .nav-item")].some((item) => item.title === "从 LCSC 导入")'), 5000, 'LCSC plugin disabled')
  const disabledState = await json(`${cdp.__origin}/api/plugins/com.kicad_forge.lcsc_import/state`)
  check(disabledState?.ok === true && disabledState?.enabled === false, `LCSC backend not disabled: ${JSON.stringify(disabledState)}`)
  await cdp.eval('window.__KF_APP_RUNTIME__.loader.setEnabled("com.kicad_forge.lcsc_import", true)')
  await waitFor(() => cdp.eval('Boolean([...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "从 LCSC 导入"))'), 5000, 'LCSC activity restored')
  await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "从 LCSC 导入")?.click()')
  await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=lcsc-panel] [data-testid=lcsc-id-input]"))'), 5000, 'LCSC sidebar panel after re-enable')
  const enabledState = await json(`${cdp.__origin}/api/plugins/com.kicad_forge.lcsc_import/state`)
  check(enabledState?.ok === true && enabledState?.enabled === true, `LCSC backend not enabled: ${JSON.stringify(enabledState)}`)
  const pluginList = await json(`${cdp.__origin}/api/plugins`)
  const lcscEntry = Array.isArray(pluginList) ? pluginList.find((item) => item.id === 'com.kicad_forge.lcsc_import') : null
  check(lcscEntry?.enabled === true && Number.isInteger(lcscEntry?.revision), `plugin list missing enabled/revision: ${JSON.stringify(lcscEntry)}`)
  // Git write/read action success is covered by run-git-write-e2e.mjs.
  await cdp.eval('[...document.querySelectorAll(".activitybar .nav-item")].find((item) => item.title === "工作空间")?.click()')
  await waitFor(() => cdp.eval('[...document.querySelectorAll(".nav-item")].some((item) => item.textContent.includes("供应信息"))'), 10000, 'workspace panel restored')
  await cdp.eval('[...document.querySelectorAll(".nav-item")].find((item) => item.textContent.includes("供应信息")).click()')
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".plugin-view [data-testid=supply-page]"))'), 10000, 'main app plugin view render')
  check(await cdp.eval('document.querySelector(".plugin-view [data-testid=supply-page]").innerText.includes("价格 ¥1.23")'), 'main app plugin view content missing')
  check(await cdp.eval('document.querySelectorAll(".center-tab").length === 2'), 'plugin view did not get a center tab')
  check(await cdp.eval('document.querySelector(".center-tab.active .center-tab-label").textContent.includes("供应信息")'), 'active center tab label missing')
  await cdp.eval("document.querySelector('.center-tab.active .center-tab-close').click()")
  await waitFor(() => cdp.eval('!document.querySelector(".plugin-view") && Boolean(document.querySelector(".toolbar"))'), 5000, 'main app view close')
  await cdp.eval('[...document.querySelectorAll(".nav-item")].find((item) => item.textContent.includes("供应信息")).click()')
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".plugin-view [data-testid=supply-page]"))'), 5000, 'main app view reopen')
  const unexpectedErrors = consoleErrors.filter((message) =>
    !/sidebar (panel|icon) boom/.test(message))
  check(!unexpectedErrors.length, `console errors: ${unexpectedErrors.join(' | ')}`)
}

runAppFixture('plugin-webview2', async (fixture) => {
  stageExamples(fixture.root)
  // A real, synthetic symbol library makes Git's library selection meaningful;
  // no developer library or database is copied into the fixture.
  const library = path.join(fixture.root, 'WebViewFixture.kicad_sym')
  writeFileSync(library, '(kicad_symbol_lib (version 20231120))\n')
  const session = await fixture.launch()
  try {
    const added = await fixture.json(`${session.origin}/api/libraries`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ name: library.slice(0, -'.kicad_sym'.length) }),
    })
    check(added.ok === true, `synthetic library registration failed: ${JSON.stringify(added)}`)
    await runScenarios(session.cdp, session.errors, fixture)
  } finally { await fixture.stop(session) }
}).catch((error) => {
  console.error(error.stack || error.message)
  process.exitCode = 1
})
