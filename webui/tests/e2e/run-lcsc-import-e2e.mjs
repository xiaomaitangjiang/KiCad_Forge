// Optional real-network LCSC end-to-end check.
//
//   python scripts/package.py --binary ... --name lcsc-e2e --output build/lcsc-e2e
//   KFORGE_BUILD_ROOT=build/lcsc-e2e/lcsc-e2e KFORGE_LCSC_E2E=1 \
//     node webui/tests/e2e/run-lcsc-import-e2e.mjs
//
// It launches the packaged app with CDP, creates a temporary component library,
// runs analyze + the full staged import through /api/tasks, verifies the files
// and operation replay, then restores an empty data directory. It refuses a
// non-empty data directory so test imports never mutate a developer/library DB.
// Not part of the default CI because it depends on easyeda2kicad/network.
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, readdirSync, rmSync } from 'node:fs'
import path from 'node:path'

if (process.env.KFORGE_LCSC_E2E !== '1') {
  console.log('LCSC real-import E2E skipped (set KFORGE_LCSC_E2E=1 to run)')
  process.exit(0)
}

const buildRoot = process.env.KFORGE_BUILD_ROOT
  ? path.resolve(process.env.KFORGE_BUILD_ROOT)
  : ''
if (!buildRoot)
  throw new Error('KFORGE_LCSC_E2E requires KFORGE_BUILD_ROOT pointing to a packaged app with empty data')
const exe = path.join(buildRoot, 'KiCad_Forge.exe')
if (!existsSync(exe)) throw new Error(`missing app: ${exe}`)
const dataRoot = path.join(buildRoot, 'data')
if (!existsSync(dataRoot) || readdirSync(dataRoot).length > 0)
  throw new Error('KFORGE_LCSC_E2E requires an empty data directory; refusing to modify a developer database')

const stamp = Date.now()
const data = path.join(dataRoot, `lcsc-e2e-${stamp}`)
const symbols = path.join(data, 'symbols')
const footprints = path.join(data, 'footprints')
mkdirSync(symbols, { recursive: true })
mkdirSync(footprints, { recursive: true })

const port = 9300 + (stamp % 400)
const app = spawn(exe, ['--no-auto-import'], {
  cwd: buildRoot,
  stdio: 'ignore',
  windowsHide: false,
  env: {
    ...process.env,
    WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}`,
  },
})
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

async function findTarget() {
  for (let attempt = 0; attempt < 120; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json`)
      if (response.ok) {
        const list = await response.json()
        const page = list.find(
          (item) => item.type === 'page' && /^http:\/\/127\.0\.0\.1:\d+/.test(item.url || ''),
        )
        if (page) return page
      }
    } catch {
      // app/CDP not ready yet
    }
    await sleep(250)
  }
  throw new Error('WebView2 CDP target not found')
}

async function waitTask(origin, taskId, timeoutMs = 120000) {
  for (let attempt = 0; attempt < timeoutMs / 500; attempt += 1) {
    const record = await fetch(`${origin}/api/tasks/${taskId}`).then((response) => response.json())
    if (['succeeded', 'failed', 'cancelled'].includes(record.state)) return record
    await sleep(500)
  }
  throw new Error(`task ${taskId} did not reach a terminal state`)
}

let origin = ''
try {
  const target = await findTarget()
  origin = new URL(target.url).origin
  const post = (pathname, body) =>
    fetch(origin + pathname, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(body),
    }).then(async (response) => ({ status: response.status, body: await response.json() }))

  const added = await post('/api/component-libraries', {
    action: 'add',
    name: `LCSC E2E ${stamp}`,
    symbol_path: symbols,
    footprint_path: footprints,
    enabled: true,
  })
  if (!added.body?.ok) throw new Error(`component library add failed: ${JSON.stringify(added.body)}`)
  const libraryId = added.body.library.id

  const analyzeSubmit = await post('/api/tasks', {
    plugin_id: 'com.kicad_forge.lcsc_import',
    action: 'analyze',
    args: { lcsc_id: process.env.KFORGE_LCSC_ID || 'C14663' },
  })
  if (!analyzeSubmit.body?.ok) throw new Error(`analyze submit failed: ${JSON.stringify(analyzeSubmit.body)}`)
  const analyze = await waitTask(origin, analyzeSubmit.body.task_id)
  if (analyze.state !== 'succeeded' || analyze.result?.ok !== true) {
    throw new Error(`analyze failed: ${JSON.stringify(analyze)}`)
  }
  console.log(`analyze: ${analyze.result.symbol_names?.[0] || analyze.result.lcsc_id}`)

  const operationId = `lcsc-e2e-${stamp}`
  const importSubmit = await post('/api/tasks', {
    plugin_id: 'com.kicad_forge.lcsc_import',
    action: 'import',
    args: {
      lcsc_id: analyze.result.lcsc_id,
      operation_id: operationId,
      target_component_library_id: libraryId,
      symbol_library_choice: { mode: 'new', name: `Phase5E2E${stamp}` },
      footprint_choice: { mode: 'new' },
    },
  })
  if (!importSubmit.body?.ok) throw new Error(`import submit failed: ${JSON.stringify(importSubmit.body)}`)
  const imported = await waitTask(origin, importSubmit.body.task_id, 180000)
  if (imported.state !== 'succeeded' || imported.result?.ok !== true) {
    throw new Error(`import failed: ${JSON.stringify(imported)}`)
  }
  console.log(`import: ${imported.result.symbol_name} (${imported.result.mode})`)

  const symbolFiles = readdirSync(symbols).filter((name) => name.endsWith('.kicad_sym'))
  const footprintDirs = readdirSync(footprints)
  if (!symbolFiles.length || !footprintDirs.length) {
    throw new Error(`imported artifacts missing: symbols=${symbolFiles}, footprints=${footprintDirs}`)
  }

  const replay = await fetch(`${origin}/api/imports/operations/${operationId}`).then((response) =>
    response.json(),
  )
  if (replay?.ok !== true || replay.result?.operation_id !== operationId) {
    throw new Error(`operation replay failed: ${JSON.stringify(replay)}`)
  }
  console.log(`operation replay: ${replay.result.operation_id}`)

  const removed = await post('/api/component-libraries', { action: 'remove', id: libraryId })
  if (!removed.body?.ok) throw new Error(`component library remove failed: ${JSON.stringify(removed.body)}`)
  console.log('LCSC real-import E2E: OK')
} finally {
  try {
    if (origin) await fetch(`${origin}/api/bye`, { method: 'POST' })
  } catch {
    // ignore
  }
  await sleep(1000)
  try { app.kill() } catch { /* ignore */ }
  await sleep(500)
  // The package started with an empty data directory; drop everything this run
  // created (including the mutation journal) so reusing the package is clean.
  try {
    rmSync(dataRoot, { recursive: true, force: true })
    mkdirSync(dataRoot, { recursive: true })
  } catch { /* ignore */ }
}
