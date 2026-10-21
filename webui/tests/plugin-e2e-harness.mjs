// Shared real-WebView2 fixtures. Never run an app from a developer's live build.
import { spawn } from 'node:child_process'
import { appendFileSync, closeSync, cpSync, existsSync, lstatSync, mkdirSync, mkdtempSync, openSync, readdirSync, rmSync, writeFileSync } from 'node:fs'
import { createServer } from 'node:net'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Cdp } from './run-lcsc-browser.mjs'

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..')
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

export function requireOptIn(variable) {
  if (process.env[variable] === '1') return true
  const message = `SKIP: set ${variable}=1 to run this real WebView2 check`
  if (process.env.CI) throw new Error(`${message}; CI must explicitly enable its required checks`)
  console.log(message)
  return false
}

export function check(value, message) {
  if (!value) throw new Error(message)
}

export function copyRuntime(source, target) {
  const data = path.join(source, 'data')
  // Inspect directory entries only; never read or copy database/config/user files.
  check(existsSync(data) && lstatSync(data).isDirectory() && !lstatSync(data).isSymbolicLink(), 'KFORGE_BUILD_ROOT must be a packaged app with an empty data directory')
  check(readdirSync(data).length === 0, 'refusing app source with non-empty data; create an empty-data CI package first')
  const copy = (from, to) => cpSync(from, to, {
    recursive: true,
    filter: (entry) => {
      if (['data', 'node_modules', 'fetched', 'staging', '.git', '__pycache__'].includes(path.basename(entry))) return false
      check(!lstatSync(entry).isSymbolicLink(), `fixture source must not contain symlinks: ${entry}`)
      return true
    },
  })
  for (const entry of readdirSync(source, { withFileTypes: true })) {
    if (entry.isFile() && (entry.name === 'KiCad_Forge.exe' || /\.dll$/i.test(entry.name))) copy(path.join(source, entry.name), path.join(target, entry.name))
  }
  for (const directory of ['webui', 'plugins', 'config', 'licenses']) {
    const from = path.join(source, directory)
    if (existsSync(from)) copy(from, path.join(target, directory))
  }
  mkdirSync(path.join(target, 'data'))
}

async function freePort() {
  return new Promise((resolve, reject) => {
    const server = createServer()
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address()
      server.close((error) => error ? reject(error) : resolve(port))
    })
  })
}

function exited(child) {
  return child.exitCode !== null || child.signalCode !== null
}

async function waitForExit(child, milliseconds) {
  if (exited(child)) return true
  return new Promise((resolve) => {
    const finish = (value) => { clearTimeout(timer); child.removeListener('exit', onExit); resolve(value) }
    const onExit = () => finish(true)
    const timer = setTimeout(() => finish(false), milliseconds)
    child.once('exit', onExit)
    if (exited(child)) finish(true)
  })
}

async function killTree(child) {
  if (exited(child)) return
  const killer = spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true })
  killer.on('error', () => {})
  if (!await waitForExit(killer, 10000)) {
    killer.kill()
    throw new Error(`taskkill timed out for fixture PID ${child.pid}`)
  }
  if (!await waitForExit(child, 10000)) throw new Error(`fixture PID ${child.pid} did not exit after tree cleanup`)
}

export async function runAppFixture(name, scenario) {
  const results = path.resolve(process.env.KFORGE_TEST_RESULTS || path.join(repo, 'test-results', 'plugin-e2e'))
  mkdirSync(results, { recursive: true })
  const report = { name, status: 'running', started: new Date().toISOString() }
  const logPath = path.join(results, `${name}.app.log`)
  const connections = new Set()
  const apps = new Set()
  let temp
  let timeout
  let failure
  let aborted = false
  const timeoutMs = Number(process.env.KFORGE_E2E_TIMEOUT_MS || 240000)
  const checkActive = () => { if (aborted) throw new Error(`${name}: fixture deadline exceeded`) }
  const request = async (url, options = {}) => {
    checkActive()
    const response = await fetch(url, { ...options, signal: AbortSignal.timeout(5000) })
    if (!response.ok) throw new Error(`HTTP ${response.status} ${url}: ${(await response.text()).slice(0, 2000)}`)
    return response
  }
  const json = async (url, options) => (await request(url, options)).json()
  const waitFor = async (fn, milliseconds, label) => {
    const deadline = Date.now() + milliseconds
    let last
    while (Date.now() < deadline) {
      checkActive()
      try { const value = await fn(); if (value) return value } catch (error) { last = error }
      await sleep(150)
    }
    throw new Error(`timeout waiting for ${label}${last ? `: ${last.message}` : ''}`)
  }
  const stop = async ({ app, origin, cdp }) => {
    if (cdp) {
      try { writeFileSync(path.join(results, `${name}.body.txt`), String(await cdp.eval('document.body.innerText', { timeoutMs: 2000 }))) } catch { /* app may be gone */ }
      cdp.failAll(new Error('fixture closing')); cdp.socket.close(); connections.delete(cdp)
    }
    if (!exited(app) && origin) {
      try { await fetch(`${origin}/api/bye`, { method: 'POST', signal: AbortSignal.timeout(3000) }) } catch { /* forced cleanup follows */ }
    }
    if (!await waitForExit(app, 5000)) await killTree(app)
    apps.delete(app)
  }
  try {
    check(process.platform === 'win32', 'real WebView2 fixtures require Windows')
    check(Number.isFinite(timeoutMs) && timeoutMs > 0 && timeoutMs <= 900000, 'invalid KFORGE_E2E_TIMEOUT_MS (expected 1..900000)')
    check(process.env.KFORGE_BUILD_ROOT, 'set KFORGE_BUILD_ROOT to a packaged app with an empty data directory')
    const source = path.resolve(process.env.KFORGE_BUILD_ROOT)
    check(existsSync(path.join(source, 'KiCad_Forge.exe')), `missing app: ${source}`)
    temp = mkdtempSync(path.join(os.tmpdir(), `kf-${name}-`))
    copyRuntime(source, temp)
    report.source = source
    report.fixture = temp
    const launch = async ({ connect = true, env = {} } = {}) => {
      checkActive()
      const port = await freePort()
      const log = openSync(logPath, 'a')
      const inherited = { ...process.env }
      // Git commands must never inherit a developer's GIT_DIR/WORK_TREE/index
      // override or injected config; scenarios supply their own test identity.
      for (const key of Object.keys(inherited)) if (key.startsWith('GIT_')) delete inherited[key]
      let app
      try {
        app = spawn(path.join(temp, 'KiCad_Forge.exe'), ['--no-auto-import'], {
          cwd: temp, stdio: ['ignore', log, log], windowsHide: true,
          env: {
            ...inherited, ...env,
            WEBVIEW2_USER_DATA_FOLDER: path.join(temp, 'data', 'webview-profile'),
            WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`,
          },
        })
      } finally { closeSync(log) }
      apps.add(app)
      let launchError
      app.once('error', (error) => { launchError = error })
      const target = await waitFor(async () => {
        if (launchError) throw launchError
        if (exited(app)) throw new Error(`app exited (${app.exitCode ?? app.signalCode}); see ${logPath}`)
        const list = await json(`http://127.0.0.1:${port}/json/list`)
        return list.find((item) => item.type === 'page' && /^http:\/\/127\.0\.0\.1:\d+/.test(item.url || ''))
      }, 40000, 'WebView2 target')
      const origin = new URL(target.url).origin
      const errors = []
      let cdp
      if (connect) {
        const socket = new WebSocket(target.webSocketDebuggerUrl)
        await new Promise((resolve, reject) => {
          const timer = setTimeout(() => { socket.close(); reject(new Error('WebView2 CDP socket open timed out')) }, 5000)
          socket.addEventListener('open', () => { clearTimeout(timer); resolve() }, { once: true })
          socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('WebView2 CDP socket failed')) }, { once: true })
        })
        cdp = new Cdp(socket, { timeoutMs: 15000, onConsoleError: (message) => {
          errors.push(message)
          appendFileSync(path.join(results, `${name}.console.log`), `${message}\n`)
        } })
        connections.add(cdp)
        cdp.__origin = origin
        await cdp.send('Runtime.enable')
        await cdp.send('Page.enable')
      }
      return { app, origin, cdp, errors }
    }
    const deadline = new Promise((_, reject) => {
      timeout = setTimeout(() => {
        aborted = true
        for (const cdp of connections) cdp.failAll(new Error(`${name}: overall timeout after ${timeoutMs}ms`))
        reject(new Error(`${name}: overall timeout after ${timeoutMs}ms`))
      }, timeoutMs)
    })
    await Promise.race([scenario({ root: temp, launch, stop, request, json, waitFor, results }), deadline])
    report.status = 'passed'
  } catch (error) {
    failure = error
    report.status = 'failed'
    report.error = error.stack || error.message
    for (const cdp of connections) {
      try { writeFileSync(path.join(results, `${name}.body.txt`), String(await cdp.eval('document.body.innerText', { timeoutMs: 2000 }))) } catch { /* app may be gone */ }
    }
  } finally {
    clearTimeout(timeout)
    for (const cdp of connections) { cdp.failAll(new Error('fixture cleanup')); cdp.socket.close() }
    for (const app of apps) {
      try { await killTree(app) } catch (error) { failure ||= error; report.status = 'failed'; report.cleanup_error = error.message }
    }
    if (temp) {
      try { rmSync(temp, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 }) } catch (error) { failure ||= error; report.status = 'failed'; report.cleanup_error = error.message }
    }
    report.finished = new Date().toISOString()
    writeFileSync(path.join(results, `${name}.json`), `${JSON.stringify(report, null, 2)}\n`)
  }
  if (failure) throw failure
  console.log(`${name}: PASS (logs: ${results})`)
}
