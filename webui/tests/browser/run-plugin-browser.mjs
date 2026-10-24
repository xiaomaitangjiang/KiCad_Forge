// Phase-1 prototype browser acceptance against the built webui/dist.
//
// Runs the prototype in Edge + CDP with a local mock of the host API and the
// plugin asset route. The real WebView2 check is a separate step after the C++
// host route lands.

import { createServer } from 'node:http'
import { createReadStream, existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { spawn } from 'node:child_process'
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Cdp, stopEdge } from '../support/cdp.mjs'

const here = path.dirname(fileURLToPath(import.meta.url))
const webuiRoot = path.resolve(here, '..', '..')
const repoRoot = path.resolve(webuiRoot, '..')
const distRoot = path.join(webuiRoot, 'dist')
const examplesRoot = path.join(repoRoot, 'examples', 'plugins')

const EDGE_CANDIDATES = [
  process.env.EDGE_PATH,
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
].filter(Boolean)

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
}

function pluginDescriptors() {
  const descriptors = []
  const byId = new Map()
  for (const name of readDir(examplesRoot)) {
    const dir = path.join(examplesRoot, name)
    const manifestPath = path.join(dir, 'manifest.json')
    if (!existsSync(manifestPath)) continue
    const manifest = JSON.parse(readFileSync(manifestPath, 'utf8'))
    const frontend = manifest.frontend
    if (!frontend) continue
    const id = manifest.id
    byId.set(id, { dir, frontend })
    descriptors.push({
      id,
      name: manifest.name,
      version: manifest.version,
      engines: manifest.engines,
      config: manifest.config,
      frontend: {
        base_url: `/plugin-assets/${id}/`,
        entry_url: `/plugin-assets/${id}/index.js`,
        styles: (frontend.styles || []).map((style) => `/plugin-assets/${id}/${style}`),
        requires: frontend.requires || [],
        provides: frontend.provides || [],
        version: manifest.version,
      },
    })
  }

  // Phase-2 fixtures exercise diagnostics scoping in the real loader.
  const fixtureDir = path.join(here, '..', 'fixtures', 'phase2')
  descriptors.push({
    id: 'test.duplicate-provider',
    name: 'Duplicate Provider Fixture',
    version: '1.0.0',
    frontend: {
      base_url: '/plugin-assets/test.duplicate-provider/',
      entry_url: '/plugin-assets/test.duplicate-provider/index.js',
      styles: [],
      provides: [{ contract: 'demo.supplier', version: '9.9.9' }],
      version: '1.0.0',
    },
  })
  descriptors.push({
    id: 'test.bad-version',
    name: 'Bad Version Fixture',
    version: '1.0.0',
    frontend: {
      base_url: '/plugin-assets/test.bad-version/',
      entry_url: '/plugin-assets/test.bad-version/index.js',
      styles: [],
      requires: [{ contract: 'demo.supplier', range: '^2.0.0' }],
      version: '1.0.0',
    },
  })
  byId.set('test.duplicate-provider', { dir: fixtureDir, entryFile: 'duplicate-provider.js' })
  byId.set('test.bad-version', { dir: fixtureDir, entryFile: 'bad-version.js' })
  descriptors.push({
    id: 'test.slots',
    name: 'Six Slot Fixture',
    version: '1.0.0',
    frontend: {
      base_url: '/plugin-assets/test.slots/',
      entry_url: '/plugin-assets/test.slots/index.js',
      styles: [],
      version: '1.0.0',
    },
  })
  byId.set('test.slots', { dir: path.join(here, '..', 'fixtures', 'phase3'), entryFile: 'slots.js' })
  return { descriptors, byId }
}

function readDir(dir) {
  return readdirSync(dir, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
}

function startServer() {
  const { descriptors, byId } = pluginDescriptors()
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1')
    if (url.pathname === '/api/plugins') {
      res.writeHead(200, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' })
      res.end(JSON.stringify(descriptors))
      return
    }
    if (url.pathname.startsWith('/plugin-assets/')) {
      const rest = url.pathname.slice('/plugin-assets/'.length)
      const slash = rest.indexOf('/')
      const id = slash < 0 ? rest : rest.slice(0, slash)
      const relative = slash < 0 ? '' : rest.slice(slash + 1)
      const entry = byId.get(id)
      if (!entry) return notFound(res)
      const target = entry.entryFile
        ? path.resolve(entry.dir, entry.entryFile)
        : path.resolve(entry.dir, 'dist', relative)
      const allowedRoot = entry.entryFile ? entry.dir : path.resolve(entry.dir, 'dist')
      if (!target.startsWith(allowedRoot + path.sep) || !existsSync(target) || !statSync(target).isFile()) {
        return notFound(res)
      }
      res.writeHead(200, {
        'content-type': MIME[path.extname(target)] || 'application/octet-stream',
        'cache-control': 'no-store',
      })
      createReadStream(target).pipe(res)
      return
    }
    const relative = url.pathname === '/' ? '/index.html' : url.pathname
    let target = path.resolve(distRoot, `.${relative}`)
    if (existsSync(target) && statSync(target).isDirectory()) target = path.join(target, 'index.html')
    if (!target.startsWith(distRoot + path.sep) || !existsSync(target) || !statSync(target).isFile()) {
      return notFound(res)
    }
    res.writeHead(200, { 'content-type': MIME[path.extname(target)] || 'application/octet-stream' })
    createReadStream(target).pipe(res)
  })
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve({ server, port: server.address().port }))
  })
}

function notFound(res) {
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' })
  res.end('not found')
}

async function freePort() {
  const { createServer: netServer } = await import('node:net')
  return new Promise((resolve, reject) => {
    const srv = netServer()
    srv.on('error', reject)
    srv.listen(0, '127.0.0.1', () => {
      const { port } = srv.address()
      srv.close(() => resolve(port))
    })
  })
}

async function waitFor(fn, timeoutMs, label) {
  const deadline = Date.now() + timeoutMs
  let last
  while (Date.now() < deadline) {
    try {
      const value = await fn()
      if (value) return value
    } catch (error) {
      last = error
    }
    await new Promise((resolve) => setTimeout(resolve, 120))
  }
  throw new Error(`timeout waiting for ${label}${last ? `: ${last.message}` : ''}`)
}

async function launchEdge(port, profileDir) {
  const edge = EDGE_CANDIDATES.find(existsSync)
  if (!edge) throw new Error('Microsoft Edge not found')
  return spawn(edge, [
    '--headless=new',
    '--disable-gpu',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-extensions',
    '--disable-background-networking',
    `--user-data-dir=${profileDir}`,
    `--remote-debugging-port=${port}`,
    'about:blank',
  ], { stdio: 'ignore', windowsHide: true })
}

async function connectCdp(port, pageUrl) {
  const created = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(pageUrl)}`, { method: 'PUT' })
  if (!created.ok) throw new Error(`CDP new page failed: HTTP ${created.status}`)
  const target = await created.json()
  const socket = new WebSocket(target.webSocketDebuggerUrl)
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true })
    socket.addEventListener('error', () => reject(new Error('CDP socket failed')), { once: true })
  })
  const errors = []
  const cdp = new Cdp(socket, { onConsoleError: (message) => errors.push(message) })
  await cdp.send('Runtime.enable')
  await cdp.send('Page.enable')
  return { cdp, errors }
}

function check(value, message) {
  if (!value) throw new Error(message)
}

async function main() {
  const { server, port } = await startServer()
  const debugPort = await freePort()
  const tempRoot = mkdtempSync(path.join(tmpdir(), 'kforge-plugin-run-'))
  const profile = mkdtempSync(path.join(tempRoot, 'edge-profile-'))
  let edge
  let cdp
  try {
    edge = await launchEdge(debugPort, profile)
    const connected = await waitFor(async () => {
      try {
        return await connectCdp(debugPort, `http://127.0.0.1:${port}/prototype/`)
      } catch {
        return null
      }
    }, 20000, 'edge cdp')
    cdp = connected.cdp
    const errors = connected.errors

    await waitFor(() => cdp.eval('Boolean(window.__KF_PROTOTYPE__)'), 20000, 'prototype boot').catch(async (error) => {
      console.error('BOOT DEBUG href=', await cdp.eval('location.href'))
      console.error('BOOT DEBUG readyState=', await cdp.eval('document.readyState'))
      console.error('BOOT DEBUG html=', (await cdp.eval('document.documentElement.outerHTML')).slice(0, 4000))
      console.error('BOOT DEBUG errors=', errors.join(' | '))
      throw error
    })
    await cdp.eval('window.__KF_PROTOTYPE__.ready.then(() => true)')

    const state = await cdp.eval('window.__KF_PROTOTYPE__.plugins()')
    const good = state.filter((plugin) => !plugin.id.startsWith('test.'))
    check(good.length === 3, `expected 3 real plugins, got ${good.length}`)
    check(good.every((plugin) => plugin.status === 'active'), `plugins not active: ${JSON.stringify(good)} diagnostics=${JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.host.diagnostics'))} console=${JSON.stringify(errors)}`)
    const duplicate = state.find((plugin) => plugin.id === 'test.duplicate-provider')
    const badVersion = state.find((plugin) => plugin.id === 'test.bad-version')
    const slotsFixture = state.find((plugin) => plugin.id === 'test.slots')
    check(duplicate?.status === 'error', `duplicate provider was not quarantined: ${JSON.stringify(duplicate)}`)
    check(badVersion?.status === 'error', `bad version was not quarantined: ${JSON.stringify(badVersion)}`)
    check(slotsFixture?.status === 'active', `slot fixture not active: ${JSON.stringify(slotsFixture)}`)
    const diagnostics = await cdp.eval('window.__KF_PROTOTYPE__.errors')
    const codes = new Set(diagnostics.map((item) => item.code))
    if (!codes.has('contract/duplicate-provider') || !codes.has('contract/version-incompatible')) {
      console.error('DIAG DEBUG state=', JSON.stringify(state))
      console.error('DIAG DEBUG diagnostics=', JSON.stringify(diagnostics))
      console.error('DIAG DEBUG hostDiagnostics=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.host.diagnostics')))
    }
    check(codes.has('contract/duplicate-provider'), `missing duplicate diagnostic: ${JSON.stringify(diagnostics)}`)
    check(codes.has('contract/version-incompatible'), `missing version diagnostic: ${JSON.stringify(diagnostics)}`)

    const counts = await cdp.eval('window.__KF_PROTOTYPE__.counts()')
    if (!(counts.page === 1) || counts.tabs !== 1 || counts.aux !== 1) {
      console.error('COUNT DEBUG plugins=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.plugins()')))
      console.error('COUNT DEBUG diagnostics=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.errors')))
      console.error('COUNT DEBUG snapshot=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.host.core.snapshot()')))
      console.error('COUNT DEBUG rootEntries=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.host.core.entries("root").map(e => e.options)')))
      console.error('COUNT DEBUG body=', (await cdp.eval('document.body.innerText')).slice(0, 2000))
      console.error('COUNT DEBUG console=', errors.join(' | '))
    }
    check(counts.page === 1, `expected one page entry, got ${JSON.stringify(counts)}`)
    check(counts.tabs === 1, `expected one tab contribution, got ${JSON.stringify(counts)}`)

    const identity = await cdp.eval('window.__KF_PROTOTYPE__.runtimeChecks')
    check(identity.reactSame === true, `plugin React identity mismatch: ${JSON.stringify(identity)}`)
    check(identity.pluginReactVersion === identity.reactVersion, `react version mismatch: ${JSON.stringify(identity)}`)

    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'supply page missing')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 1'), 'supply tab missing')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supplier-status]").length === 1'), 'supplier status missing')
    check(await cdp.eval('document.querySelector("[data-testid=supply-page]").innerText.includes("价格 ¥1.23")'), 'plugin config defaults not applied')

    // Six public slots: all outlets resolve to the fixture's contribution.
    for (const testid of ['slot-sidebar-item', 'slot-toolbar-item', 'slot-card-item', 'slot-menu-item', 'slot-settings-item']) {
      check(await cdp.eval(`Boolean(document.querySelector("[data-testid=${testid}]"))`), `missing slot contribution ${testid}`)
    }
    check(await cdp.eval('document.querySelector("[data-testid=slot-card-item]").innerText.includes("A")'), 'card slot owner props missing')

    // Rapid selection: A is slow, B is fast; after switching, only B may publish
    // and the late A result must not replace it.
    await cdp.eval('document.querySelector("[data-testid=rapid-switch]").click()')
    await waitFor(() => cdp.eval('document.querySelector("[data-testid=rapid-result]").textContent.startsWith("B:ready:B")'), 5000, 'rapid switch B ready')
    await new Promise((resolve) => setTimeout(resolve, 500))
    check(await cdp.eval('document.querySelector("[data-testid=rapid-result]").textContent.startsWith("B:ready:B")'), 'stale A result overrode B')

    // Inspector tabs: default content until a plugin tab is selected; one
    // throwing tab does not remove its sibling or the default tab.
    check(await cdp.eval('Boolean(document.querySelector("[data-testid=inspector-default]"))'), 'inspector default content missing')
    const clickTab = async (label) => {
      return cdp.eval(`(() => { const button = [...document.querySelectorAll('[role=tab]')].find((item) => item.textContent.trim() === ${JSON.stringify(label)}); button?.click(); return Boolean(button) })()`)
    }
    check(await clickTab('正常页签'), 'inspector ok tab button missing')
    await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=slot-tab-ok]"))'), 5000, 'inspector ok tab')
    await cdp.eval('globalThis.__KF_BOOM__ = true')
    check(await clickTab('异常页签'), 'inspector boom tab button missing')
    await waitFor(() => cdp.eval('Boolean(document.querySelector(".kf-plugin-error")) && window.__KF_PROTOTYPE__.errors.some((item) => item.code === "plugin/render-error" && item.localId === "boom" && item.plugin === "test.slots" && item.slot === "inspector.tab")'), 5000, 'inspector tab fallback + diagnostic')
    check(await clickTab('正常页签'), 'inspector ok tab button missing after error')
    await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=slot-tab-ok]"))'), 5000, 'inspector sibling tab after error')
    check(await clickTab('详情'), 'inspector default tab missing')
    check(await cdp.eval('Boolean(document.querySelector("[data-testid=inspector-default]"))'), 'inspector default tab did not return')

    // Parent/slot withdrawal: disabling the contributor clears all six slots;
    // re-enabling registers each contribution exactly once (no duplicates).
    await cdp.eval('globalThis.__KF_BOOM__ = false')
    await cdp.eval('window.__KF_PROTOTYPE__.disable("test.slots")')
    await waitFor(() => cdp.eval('!document.querySelector("[data-testid=slot-sidebar-item]") && !document.querySelector("[data-testid=slot-settings-item]")'), 5000, 'slot withdrawal')
    await cdp.eval('window.__KF_PROTOTYPE__.enable("test.slots")')
    await waitFor(() => cdp.eval('Boolean(document.querySelector("[data-testid=slot-sidebar-item]"))'), 5000, 'slot restore')
    const slotCounts = await cdp.eval('({ sidebar: window.__KF_PROTOTYPE__.host.count("sidebar.section"), toolbar: window.__KF_PROTOTYPE__.host.count("toolbar.action"), card: window.__KF_PROTOTYPE__.host.count("card.accessory"), inspector: window.__KF_PROTOTYPE__.host.count("inspector.tab"), context: window.__KF_PROTOTYPE__.host.count("contextmenu.item"), settings: window.__KF_PROTOTYPE__.host.count("settings.section") })')
    check(slotCounts.sidebar === 1 && slotCounts.toolbar === 1 && slotCounts.card === 1 && slotCounts.context === 1 && slotCounts.settings === 1, `slot restore not once: ${JSON.stringify(slotCounts)}`)
    check(slotCounts.inspector === 2, `inspector tabs restore not once: ${JSON.stringify(slotCounts)}`)
    check(await cdp.eval('getComputedStyle(document.querySelector("[data-testid=supply-page]")).borderLeftWidth === "4px"'), 'plugin stylesheet not applied')
    check(await cdp.eval('Boolean(document.querySelector("link[data-kforge-plugin=\\"demo.supply-page\\"]"))'), 'plugin stylesheet link missing')

    // Withdraw the page contribution: UI tab/page disappear, service and the
    // independent status consumer continue.
    await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supply-page")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 0'), 5000, 'page withdrawal').catch(async (error) => {
      console.error('WITHDRAW DEBUG counts=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.counts()')))
      console.error('WITHDRAW DEBUG plugins=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.plugins()')))
      throw error
    })
    check(await cdp.eval('window.__KF_PROTOTYPE__.counts().page === 0'), 'page contribution survived page withdrawal')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 0'), 'page still visible')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 0'), 'tab survived page withdrawal')
    check(await cdp.eval('!document.querySelector("link[data-kforge-plugin=\\"demo.supply-page\\"]")'), 'plugin stylesheet survived page withdrawal')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supplier-status]").length === 1'), 'service consumer did not survive page withdrawal')

    // Restore the page: the tab registers exactly once.
    await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supply-page")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 1'), 5000, 'page restore').catch(async (error) => {
      console.error('RESTORE DEBUG counts=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.counts()')))
      console.error('RESTORE DEBUG plugins=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.plugins()')))
      console.error('RESTORE DEBUG diagnostics=', JSON.stringify(await cdp.eval('window.__KF_PROTOTYPE__.errors')))
      throw error
    })
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'page not restored')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-tab]").length === 1'), 'tab not restored once')
    check(await cdp.eval('getComputedStyle(document.querySelector("[data-testid=supply-page]")).borderLeftWidth === "4px"'), 'plugin stylesheet not restored')

    // Withdraw the service provider: dependent plugins unload, then recover.
    await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supplier")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().page === 0 && window.__KF_PROTOTYPE__.counts().aux === 0'), 5000, 'provider withdrawal')
    await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supplier")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().tabs === 1 && window.__KF_PROTOTYPE__.counts().aux === 1'), 5000, 'provider restore')
    check(await cdp.eval('document.querySelectorAll("[data-testid=supply-page]").length === 1'), 'page not restored after provider recovery')

    // Disabling/enabling a dependent after a dependency-driven restart must
    // dispose the live fiber, not a stale handle.
    await cdp.eval('window.__KF_PROTOTYPE__.disable("demo.supply-tab")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().aux === 0 && window.__KF_PROTOTYPE__.counts().tabs === 0'), 5000, 'dependent withdrawal')
    await cdp.eval('window.__KF_PROTOTYPE__.enable("demo.supply-tab")')
    await waitFor(() => cdp.eval('window.__KF_PROTOTYPE__.counts().aux === 1 && window.__KF_PROTOTYPE__.counts().tabs === 1'), 5000, 'dependent restore')

    const finalDiagnostics = await cdp.eval('window.__KF_PROTOTYPE__.errors')
    const unexpected = finalDiagnostics.filter(
      (item) =>
        item.code !== 'contract/duplicate-provider' &&
        item.code !== 'contract/version-incompatible' &&
        item.code !== 'plugin/render-error',
    )
    check(!unexpected.length, `unexpected diagnostics: ${JSON.stringify(unexpected)}`)
    const unexpectedConsole = errors.filter((item) => !/fixture tab boom/.test(item))
    check(!unexpectedConsole.length, `console errors: ${unexpectedConsole.join(' | ')}`)
    console.log('phase-1 prototype browser check: OK')
  } finally {
    try { if (cdp) cdp.socket?.close() } catch { /* ignore */ }
    try { if (edge) await stopEdge(edge) } catch { /* ignore */ }
    await new Promise((resolve) => server.close(resolve))
    try { rmSync(tempRoot, { recursive: true, force: true }) } catch { /* ignore */ }
  }
}

main().catch((error) => {
  console.error(error.stack || error.message)
  process.exit(1)
})
