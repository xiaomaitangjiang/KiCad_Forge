// 用本机 Edge（headless）+ Vite + CDP 跑 webui/tests/browser/lcsc-browser.jsx 里的真实组件
// 交互场景。不引入浏览器依赖、不下载浏览器：只连本机已安装的 Edge。
//
//   node webui/tests/browser/run-lcsc-browser.mjs             # 专用流程（默认）
//   node webui/tests/browser/run-lcsc-browser.mjs --types     # 元件类型页（独立页面/夹具）
//   node webui/tests/browser/run-lcsc-browser.mjs --import-progress # 仅导入进度回归
//   node webui/tests/browser/run-lcsc-browser.mjs --type-matching # 类型匹配规则与请求代次
//
// 也可以被测试文件直接导入（runLcscBrowser / 下面导出的纯 helper），或经
// `node --test` 直接执行这个文件。
// 退出码：全部通过 0；有失败 1；环境不可用（找不到 Edge / 启动失败）2。
//
// 资源纪律（这些资源都是本次进程自己造的：临时 profile、Vite dev server、Edge）：
//   - import 这个模块不产生任何副作用（不建 profile、不起进程）；
//   - 每次 main() 自己建一个唯一临时根，profile 只在里面；
//   - 创建/启动与清理都在同一个 try/finally 里，启动失败也要清理；
//   - 清理失败不吞掉：记录后让本次运行失败（否则「跑绿了」是假的）；
//   - 删除目录前核验目标确实是本次 mkdtemp 返回的临时根的直接子目录。

import { spawn } from 'node:child_process'
import { mkdtempSync, rmSync, existsSync, lstatSync, realpathSync, statSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { Cdp, stopEdge } from '../support/cdp.mjs'
export { Cdp, stopEdge }

import { createServer } from 'vite'
import react from '@vitejs/plugin-react'

const here = path.dirname(fileURLToPath(import.meta.url))
const webuiRoot = path.resolve(here, '..', '..')
// 显式模式互斥；环境变量只能影响没有显式参数的默认选择。
export function resolveMode(argv = process.argv.slice(2), _env = process.env) {
  const hasTypes = argv.includes('--types')
  if (argv.includes('--legacy')) throw new Error('--legacy has been removed with the old Python UI')
  const hasMatching = argv.includes('--type-matching')
  const hasImportProgress = argv.includes('--import-progress')
  const hasSettingsImport = argv.includes('--settings-import')
  if (hasSettingsImport && (hasImportProgress || hasTypes || hasMatching))
    throw new Error('--settings-import 与其他显式模式不能同时使用')
  if (hasSettingsImport) return 'types'
  if (hasImportProgress && (hasTypes || hasMatching))
    throw new Error('--import-progress 与其他显式模式不能同时使用')
  if (hasImportProgress) return 'types'
  if (hasMatching && hasTypes)
    throw new Error('--type-matching 与 --types 不能同时使用：请显式选择一种模式')
  if (hasMatching) return 'type-matching'
  if (hasTypes) return 'types'
  return 'new'
}

// 类型管理/匹配各有独立页面与夹具，默认流程复用 LCSC 页面。
const MODE_PAGE = {
  'type-matching': 'tests/browser/type-matching-browser.html',
  types: 'tests/browser/type-management-browser.html',
  new: 'tests/browser/lcsc-browser.html',
}

const EDGE_CANDIDATES = [
  process.env.EDGE_PATH,
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
].filter(Boolean)

export const PROFILE_PREFIX = 'kforge-lcsc-edge-'
export const TEMP_ROOT_PREFIX = 'kforge-lcsc-run-'
// 单条 CDP 命令、单次 waitFor 轮询、整个 harness 的硬上限
const RUN_TIMEOUT_MS = 240000
// types 页整批场景里每条都要等真实的 ImportingModal 2s 轮询与 toast 定时器，
// 自然远超单条命令的 20s。只给这一次 harness.run 的求值放宽；其余 CDP 调用
// （含同一模式里的导航/原生输入）保持 20s 不变，也不动全局 240s 硬上限。
const HARNESS_RUN_TIMEOUT_MS = { types: 150000, 'type-matching': 90000 }

function sleep(ms) { return new Promise(r => setTimeout(r, ms)) }

/// 有界等待：被等的事情自己不结束时也要能退出（CDP 断线、页面卡死都不例外）
export function withTimeout(promise, ms, label) {
  let timer = null
  return Promise.race([
    Promise.resolve(promise).finally(() => { if (timer) clearTimeout(timer) }),
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${label} timed out after ${ms}ms`)), ms)
    }),
  ])
}

async function waitFor(fn, timeoutMs, label) {
  const deadline = Date.now() + timeoutMs
  let last = null
  while (Date.now() < deadline) {
    try {
      // 每一轮都带自己的上限：fn() 挂住时 deadline 依然有效
      const value = await withTimeout(Promise.resolve().then(fn),
        Math.max(1, deadline - Date.now()), label)
      if (value) return value
    } catch (cause) { last = cause }
    await sleep(120)
  }
  throw new Error(`timeout waiting for ${label}${last ? `: ${last.message}` : ''}`)
}

// ---- 临时目录：创建与安全清理 ----
/// 每次 main 建一个唯一临时根（import 本模块不会建任何东西）
export function createTempRoot() {
  return mkdtempSync(path.join(tmpdir(), TEMP_ROOT_PREFIX))
}

/// 在本次临时根里建 profile 目录
export function createProfileDir(tempRoot) {
  const root = requireTempRoot(tempRoot)
  return mkdtempSync(path.join(root, PROFILE_PREFIX))
}

function requireTempRoot(tempRoot) {
  if (typeof tempRoot !== 'string' || !tempRoot) throw new Error('tempRoot is required')
  const root = path.resolve(tempRoot)
  if (!path.isAbsolute(root) || path.dirname(root) === root) throw new Error(`bad tempRoot: ${root}`)
  return root
}

/// 目录本身必须是真实目录，不能是 junction / symlink：顺着链接做 realpath 会指到
/// 临时目录下的无关同级目录，递归删除就把别人的东西删了。
function assertRealDir(dir, what) {
  const stat = lstatSync(dir)
  if (stat.isSymbolicLink())
    throw new Error(`refusing to use ${what} ${dir}: it is a symlink or junction`)
  if (!stat.isDirectory()) throw new Error(`refusing to use ${what} ${dir}: not a directory`)
}

/// 删除前的核验：绝对、规范化后仍然是「本次临时根的直接子目录」且名字带专用前缀。
/// 父目录、根目录、临时根自身、越界路径、符号链接指向外部——一律拒绝。
export function assertRemovableProfile(target, tempRoot) {
  if (typeof target !== 'string' || !target) throw new Error('refusing to remove: empty path')
  const root = requireTempRoot(tempRoot)
  // 本次临时根被换成链接时不认它：realpath 一旦跟着链接走，下面的「直接子目录」
  // 校验就变成对链接目标校验，删的会是链接指向的那个目录。
  assertRealDir(root, 'temp root')
  const resolved = path.resolve(target)
  if (!path.isAbsolute(resolved) || path.dirname(resolved) === resolved)
    throw new Error(`refusing to remove: not a subdirectory (${resolved})`)
  // 规范化（解析短名/符号链接）之后再比对：避免用链接把删除带出临时根
  const realRoot = realpathSync(root)
  let realTarget
  try { realTarget = realpathSync(resolved) } catch (cause) {
    throw new Error(`refusing to remove: cannot resolve ${resolved} (${cause.message})`)
  }
  if (realTarget === realRoot) throw new Error(`refusing to remove the temp root itself: ${realTarget}`)
  if (path.dirname(realTarget) !== realRoot)
    throw new Error(`refusing to remove ${realTarget}: not a direct child of ${realRoot}`)
  const base = path.basename(realTarget)
  if (!base.startsWith(PROFILE_PREFIX) || base === PROFILE_PREFIX)
    throw new Error(`refusing to remove ${realTarget}: name must start with ${PROFILE_PREFIX}`)
  if (!statSync(realTarget).isDirectory())
    throw new Error(`refusing to remove ${realTarget}: not a directory`)
  return realTarget
}

/// 递归删除 profile：先核验，再删。删除失败会抛出去（由调用方记录并使验收失败）
export function removeProfileDir(target, tempRoot) {
  const realTarget = assertRemovableProfile(target, tempRoot)
  rmSync(realTarget, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 })
}

/// 删除本次临时根：同样先核验前缀与位置，而且根自己不能是链接 ——
/// 否则 realpath 会指到临时目录下的无关同级目录，递归删除会删掉它。
export function removeTempRoot(tempRoot) {
  const root = requireTempRoot(tempRoot)
  if (!path.basename(root).startsWith(TEMP_ROOT_PREFIX) || path.basename(root) === TEMP_ROOT_PREFIX)
    throw new Error(`refusing to remove temp root ${root}`)
  assertRealDir(root, 'temp root')
  const realRoot = realpathSync(root)
  // 解析之后再核对一次身份（前缀）：不顺着链接把别人的目录当成自己的临时根
  const realName = path.basename(realRoot)
  if (!realName.startsWith(TEMP_ROOT_PREFIX) || realName === TEMP_ROOT_PREFIX)
    throw new Error(`refusing to remove ${realRoot}: resolved name must start with ${TEMP_ROOT_PREFIX}`)
  if (path.dirname(realRoot) !== realpathSync(tmpdir()))
    throw new Error(`refusing to remove ${realRoot}: not inside the system temp dir`)
  rmSync(realRoot, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 })
}

// ---- Vite / Edge ----
async function startVite() {
  const server = await createServer({
    configFile: false,
    root: webuiRoot,
    plugins: [react()],
    resolve: { dedupe: ['react', 'react-dom'] },
    optimizeDeps: {
      noDiscovery: true,
      include: [
        'react', 'react-dom', 'react-dom/client', 'react/jsx-runtime', 'react/jsx-dev-runtime',
        'react-i18next', 'i18next',
        '@deepseek-ai/cordis', '@deepseek-ai/dsh-client-ui-slots', '@kiforge/plugin-sdk',
      ],
    },
    server: { host: '127.0.0.1', port: 0, strictPort: false, fs: { strict: true } },
    logLevel: 'warn',
  })
  try {
    await server.listen()
  } catch (cause) {
    await server.close().catch(() => {})
    throw cause
  }
  const url = server.resolvedUrls?.local?.[0]
  if (!url) {
    await server.close().catch(() => {})
    throw new Error('vite did not report a local url')
  }
  return { server, base: url.replace(/\/$/, '') }
}

function launchEdge(port, profileDir) {
  const exe = EDGE_CANDIDATES.find(p => existsSync(p))
  if (!exe) throw new Error('Microsoft Edge not found (set EDGE_PATH)')
  const child = spawn(exe, [
    '--headless=new',
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${profileDir}`,
    '--no-first-run', '--no-default-browser-check', '--disable-gpu',
    '--disable-extensions', '--disable-background-networking',
    '--window-size=1280,900',
    'about:blank',
  ], { stdio: 'ignore', windowsHide: true })
  return child
}

async function freePort() {
  const { createServer: createNetServer } = await import('node:net')
  return new Promise((resolve, reject) => {
    const srv = createNetServer()
    srv.on('error', reject)
    srv.listen(0, '127.0.0.1', () => {
      const { port } = srv.address()
      srv.close(() => resolve(port))
    })
  })
}

// ---- 原生鼠标驱动（CDP Input 域）----
// 「整行可点」只有在真实鼠标事件下才算验证过：DOM dispatchEvent 是脚本派发的合成
// 事件（isTrusted=false），命中的是脚本指定的元素，根本不能证明用户点得到这一行。
// 所以这里的点击一律走 Input.dispatchMouseEvent（浏览器输入管线 → isTrusted=true），
// 页内没有、也不允许有任何 DOM click 兜底。
const NATIVE_VIEWPORTS = [[1280, 900], [960, 640]]
const UUIDS = {
  A: '11111111-1111-4111-8111-111111111111',
  B: '22222222-2222-4222-8222-222222222222',
  C: '33333333-3333-4333-8333-333333333333',
}
// 正常候选必须显式带服务端存在性状态：界面只认 footprint_status='available'，
// 缺失状态按「未检查」处理（候选会被禁用）。异常候选在各自场景里显式标失效。
const CAND_A = { id: UUIDS.A, library: 'main', name: 'SOT-223', score: 0.94, pad_count: 4, footprint_status: 'available' }
const CAND_B = { id: UUIDS.B, library: 'main', name: 'SOT-223-3', score: 0.71, pad_count: 4, footprint_status: 'available' }

function check(cond, message) { if (!cond) throw new Error(message) }
function num(n) { return Math.round(n * 10) / 10 }
function describeHits(hits) {
  return hits.map(h => `${num(h.x)},${num(h.y)}→${h.testid || h.tag || 'null'}`
    + `${h.rowTestid && h.rowTestid !== h.testid ? `(行 ${h.rowTestid})` : ''}`).join(' | ')
}

function needRow(snapshot, testid, label) {
  const row = (snapshot?.rows || []).find(r => r.testid === testid)
  if (!row) {
    throw new Error(`${label}: 快照里没有 ${testid}；现有行：`
      + `${(snapshot?.rows || []).map(r => r.testid).join(', ') || '(空)'}`)
  }
  return row
}

/// 行布局硬要求：整行一个 flex 容器、radio 是固定小尺寸、名称首行与 radio 并排、
/// 长内容不许横向溢出。旧 CSS（.form-group label 的 display:block + input width:100%）
/// 在这些断言下必然失败。
function checkLayout(row, label) {
  check(row.display === 'flex', `${label}: computed display=${row.display}，候选行必须是 flex（整行一个可点区域）`)
  check(row.hasRadio, `${label}: 没有 radio`)
  check(row.radioWidth > 0, `${label}: radio 宽度为 0`)
  check(row.radioWidth <= 24, `${label}: radio 宽 ${num(row.radioWidth)}px 超出 24px 上限`)
  check(row.radioRatio < 0.12,
    `${label}: radio 占行宽 ${num(row.radioRatio * 100)}%，要求 <12%（被输入框的 width:100% 拉满）`)
  check(row.radioPadding === '0px', `${label}: radio 仍带着输入框内边距 ${row.radioPadding}`)
  check(row.sideBySide,
    `${label}: radio 与名称首行没有左右并排；radio=${JSON.stringify(row.radioRect)} name=${JSON.stringify(row.nameRect)}`)
  check(row.rowOverflowX <= 1, `${label}: 行内横向溢出 ${row.rowOverflowX}px`)
  check(row.listOverflowX <= 1, `${label}: 列表横向溢出 ${row.listOverflowX}px`)
}

/// 取点：先 scrollIntoView + 等两帧，再用 elementFromPoint 多点采样校验遮挡，
/// 最后用 CDP 原生鼠标按下/抬起。取不到点或点被遮挡就报失败，不退回 DOM click。
async function clickPoint(cdp, testid, { aim = 'row' } = {}) {
  const probe = await cdp.eval(
    `window.__harness.native.probe(${JSON.stringify(testid)}, ${JSON.stringify(aim)})`)
  if (!probe || !Array.isArray(probe.hits)) throw new Error(`native: probe(${testid}) 没有返回采样点`)
  const onTarget = h => h.rowTestid === testid || h.testid === testid
  const same = probe.hits.filter(onTarget)
  const point = aim === 'blank'
    ? same.find(h => h.isRowElement || (h.testid === testid && !h.rowTestid))
    : same[Math.floor(same.length / 2)]
  if (!point) {
    throw new Error(`原生点击取点失败：${testid} aim=${aim} 的采样点没有一个落在这个元素上；`
      + `命中：${describeHits(probe.hits)}`)
  }
  if (same.length < 3) {
    throw new Error(`遮挡校验不足：${testid} 只有 ${same.length}/${probe.hits.length} 个采样点命中该元素`
      + `（要求 ≥3，不能只看 bbox 中心）；命中：${describeHits(probe.hits)}`)
  }
  await cdp.eval('window.__harness.native.clearClicks()')
  const base = { x: point.x, y: point.y, clickCount: 1 }
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: base.x, y: base.y, button: 'none', buttons: 0 })
  await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...base, button: 'left', buttons: 1 })
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...base, button: 'left', buttons: 0 })
  await cdp.eval('window.__harness.native.settle()')
  const snapshot = await cdp.eval('window.__harness.native.snapshot()')
  const clicks = snapshot?.clickLog || []
  if (!clicks.length) {
    throw new Error(`原生点击没有被页面收到（坐标 ${num(point.x)},${num(point.y)} 落在 ${testid} 上）：`
      + `${describeHits(probe.hits)}`)
  }
  if (clicks[0].trusted !== true) {
    throw new Error(`页面收到的点击必须 isTrusted=true，实际 ${JSON.stringify(clicks)}`)
  }
  return { point, clicks, probe, snapshot }
}

/// 一次原生会话：准备（act 里的 DOM 输入）→ 开原生驱动 → 断言 → 收尾（恢复 act 环境）
async function session(ctx, spec, fn) {
  const prepared = await ctx.prepare(spec)
  const begun = await ctx.begin()
  check(begun?.active === true && begun?.actEnvironment === false,
    '原生会话必须关掉 IS_REACT_ACT_ENVIRONMENT（否则会刷 act 警告）')
  let out
  try {
    out = await fn(prepared)
  } finally {
    const ended = await ctx.end()
    check(ended?.actEnvironment === true, '原生会话结束后必须恢复 act 环境')
  }
  return out
}

const NATIVE_SCENARIOS = [
  {
    name: 'native-symbol: 子库名称、radio、空白均可选，同名路径区分，显式确认后才显示封装',
    async run(ctx) {
      await session(ctx, { stopAt: 'symbol' }, async snap => {
        const root = 'lcsc-symbol-Device.kicad_sym'
        const nested = 'lcsc-symbol-nested/Device.kicad_sym'
        for (const id of [root, nested, 'lcsc-symbol-new']) checkLayout(needRow(snap, id, id), id)
        check(snap.checked.length === 0 && snap.symbolNextDisabled === true,
          '不能默认选择符号子库')
        await ctx.click(root, { aim: 'name' })
        let current = await ctx.snap()
        check(JSON.stringify(current.checked) === JSON.stringify([root]), '点名称选中根目录子库')
        await ctx.click(nested, { aim: 'radio' })
        current = await ctx.snap()
        check(JSON.stringify(current.checked) === JSON.stringify([nested]), '点radio选中同名嵌套子库')
        await ctx.click(root, { aim: 'blank' })
        current = await ctx.snap()
        check(JSON.stringify(current.checked) === JSON.stringify([root]), '点行内空白也选中对应子库')
        await ctx.click(nested, { aim: 'name' })
        await ctx.click('lcsc-symbol-next', { aim: 'row' })
        current = await ctx.snap()
        check(current.hasCandidates === true, '确认子库后出现封装候选')
        check(current.summary.includes('nested/Device.kicad_sym'), '摘要使用点选的嵌套路径')
        check(current.importDisabled === true, '仍须明确选择封装')
        check(current.importBodies.length === 0, '选择符号子库不会导入')
      })
    },
  },
  {
    name: 'native-layout: 候选/新建/目标行 flex、radio ≤24px 且 <12% 行宽、名称首行并排、无横向溢出',
    async run(ctx) {
      await session(ctx, { candidates: [CAND_A, CAND_B] }, async snap => {
        checkLayout(needRow(snap, `lcsc-cand-${UUIDS.A}`, '自动候选1'), '自动候选1')
        checkLayout(needRow(snap, `lcsc-cand-${UUIDS.B}`, '自动候选2'), '自动候选2')
        checkLayout(needRow(snap, 'lcsc-new-footprint', '新建行'), '新建行')
        // 遮挡校验：整行横 5 竖 3 采样，全部都应落在这行上（不只 bbox 中心）
        const probe = await ctx.eval(
          `window.__harness.native.probe(${JSON.stringify(`lcsc-cand-${UUIDS.A}`)}, 'row')`)
        const same = probe.hits.filter(h => h.rowTestid === `lcsc-cand-${UUIDS.A}`)
        check(same.length === probe.hits.length && same.length >= 5,
          `遮挡校验：命中该行的采样点 ${same.length}/${probe.hits.length}；${describeHits(probe.hits)}`)
      })
      await session(ctx, { stopAt: 'target' }, async snap => {
        for (const id of ['lcsc-target-pkg-ok', 'lcsc-target-pkg-locked', 'lcsc-target-pkg-paths'])
          checkLayout(needRow(snap, id, id), id)
      })
    },
  },
  {
    name: 'native-click: 名称 / radio / 行内空白三处原生点击都能选中同一行',
    async run(ctx) {
      await session(ctx, { candidates: [CAND_A, CAND_B] }, async snap => {
        check(snap.checked.length === 0 && snap.importDisabled === true,
          `准备态必须没有任何选择且导入禁用：checked=${JSON.stringify(snap.checked)} disabled=${snap.importDisabled}`)
        const byName = await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'name' })
        let s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `点名称应选中该行，实际 checked=${JSON.stringify(s.checked)}`)
        check(s.importDisabled === false, '点名称选中后导入应可用')
        check(byName.clicks[0].trusted === true, '名称点击必须是 isTrusted=true')

        const byRadio = await ctx.click(`lcsc-cand-${UUIDS.B}`, { aim: 'radio' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.B}`]),
          `点 radio 应切到该行，实际 checked=${JSON.stringify(s.checked)}`)
        check(byRadio.clicks[0].trusted === true, 'radio 点击必须是 isTrusted=true')

        const byBlank = await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'blank' })
        check(byBlank.point.isRowElement === true,
          `行内空白点必须命中的是行本身：${JSON.stringify(byBlank.point)}`)
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `点行内空白应切到该行，实际 checked=${JSON.stringify(s.checked)}`)
        check(byBlank.clicks[0].trusted === true, '空白处点击必须是 isTrusted=true')
        check(s.importBodies.length === 0, '点候选行绝不能提交导入')
      })
    },
  },
  {
    name: 'native-click: 自动多候选互切、选中唯一且视觉可辨，点候选行不提交',
    async run(ctx) {
      await session(ctx, { candidates: [CAND_A, CAND_B] }, async _snap => {
        const first = await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'row' })
        let s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `第一行应被选中，实际 ${JSON.stringify(s.checked)}`)
        const rowA = needRow(s, `lcsc-cand-${UUIDS.A}`, '候选 A')
        const rowB = needRow(s, `lcsc-cand-${UUIDS.B}`, '候选 B')
        check(rowA.selected === true, '选中的行必须有 selected 标记')
        check(rowB.selected === false, '未选中的行不能有 selected 标记')
        check(rowA.background !== rowB.background,
          `选中与未选中的背景必须不同：${rowA.background} vs ${rowB.background}`)

        const second = await ctx.click(`lcsc-cand-${UUIDS.B}`, { aim: 'row' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.B}`]),
          `选择必须切到第二行且只有一个，实际 ${JSON.stringify(s.checked)}`)
        check(needRow(s, `lcsc-cand-${UUIDS.A}`, '候选 A').selected === false, '旧选中要取消标记')

        const third = await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'row' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `切回第一行，实际 ${JSON.stringify(s.checked)}`)
        check(s.importBodies.length === 0 && s.analyzeCalls === 1,
          `点候选行既不能提交也不能重新查找：import=${s.importBodies.length} analyze=${s.analyzeCalls}`)
        check([first, second, third].every(c => c.clicks[0].trusted === true),
          '三次原生点击都必须是 isTrusted=true')
      })
    },
  },
  {
    name: 'native-identity: 同名不同 UUID 的候选分别可选，提交的是点中的那个而不是首项',
    async run(ctx) {
      const sameName = [
        { id: UUIDS.A, library: 'main', name: 'SOT-223', score: 0.94, pad_count: 4, footprint_status: 'available' },
        { id: UUIDS.B, library: 'other', name: 'SOT-223', score: 0.71, pad_count: 4, footprint_status: 'available' },
      ]
      await session(ctx, { candidates: sameName }, async snap => {
        const rowA = needRow(snap, `lcsc-cand-${UUIDS.A}`, '同名候选 A')
        const rowB = needRow(snap, `lcsc-cand-${UUIDS.B}`, '同名候选 B')
        check(rowA.nameText === rowB.nameText, `两行显示同名才算同名用例：${rowA.nameText} / ${rowB.nameText}`)
        check(rowA.testid !== rowB.testid, '同名候选必须按 UUID 各占一行')
        await ctx.click(`lcsc-cand-${UUIDS.B}`, { aim: 'name' })
        let s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.B}`]),
          `同名场景下必须按 UUID 选中所点的那一行，实际 ${JSON.stringify(s.checked)}`)
        await ctx.click('lcsc-import', { aim: 'row' })
        s = await ctx.snap()
        check(s.importBodies.length === 1, `只应发出一次导入请求，实际 ${s.importBodies.length}`)
        check(s.importBodies[0].footprint_id === UUIDS.B,
          `提交的必须是点中的 UUID（不是首项 ${UUIDS.A}），实际 ${s.importBodies[0].footprint_id}`)
        check(s.importBodies[0].footprint_choice?.mode === 'reuse', '复用模式')
        check(s.clickLog.every(c => c.trusted === true), '页面收到的点击都必须是可信的原生事件')
      })
    },
  },
  {
    name: 'native-identity: 手动候选与自动候选同一 UUID 共存，选择一致且只提交该 UUID',
    async run(ctx) {
      const manual = [
        { id: UUIDS.A, library: 'main', name: 'SOT-223', footprint_status: 'available' },
        { id: UUIDS.C, library: 'main', name: 'SOT-23', footprint_status: 'available' },
      ]
      await session(ctx, { candidates: [CAND_A, CAND_B], manual, manualQuery: 'SOT' }, async snap => {
        needRow(snap, `lcsc-cand-${UUIDS.A}`, '自动候选 A')
        needRow(snap, `lcsc-manual-${UUIDS.A}`, '手动候选 A')
        needRow(snap, `lcsc-manual-${UUIDS.C}`, '手动候选 C')
        await ctx.click(`lcsc-manual-${UUIDS.A}`, { aim: 'name' })
        let s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-manual-${UUIDS.A}`]),
          `手动行 A 被点中后只能有一个选中，实际 ${JSON.stringify(s.checked)}`)
        check(s.importDisabled === false, '手动选中后应可导入')
        await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'row' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `切回自动行 A：同一 UUID，选中唯一且落在自动行上，实际 ${JSON.stringify(s.checked)}`)
        await ctx.click('lcsc-import', { aim: 'row' })
        s = await ctx.snap()
        check(s.importBodies.at(-1)?.footprint_id === UUIDS.A,
          `无论从哪一侧选中，提交的都是同一个 UUID，实际 ${s.importBodies.at(-1)?.footprint_id}`)
      })
    },
  },
  {
    name: 'native-disabled: 缺 UUID 的候选行原生点击真的落在行上也不生效',
    async run(ctx) {
      const noId = [
        { library: 'main', name: 'NO-UUID', score: 0.9, pad_count: 4 },
        { id: UUIDS.A, library: 'main', name: 'OK', score: 0.5, pad_count: 4, footprint_status: 'available' },
      ]
      await session(ctx, { candidates: noId }, async snap => {
        const row = needRow(snap, 'lcsc-cand-none-0', '缺 UUID 行')
        check(row.selectable === 'false', `缺 UUID 行必须标记不可选，实际 ${row.selectable}`)
        check(row.radioDisabled === true, '缺 UUID 行的 radio 必须禁用')
        check(row.text.includes('UUID'), `缺 UUID 行要说明原因，实际文案：${row.text}`)
        const click = await ctx.click('lcsc-cand-none-0', { aim: 'name' })
        check(click.point.rowTestid === 'lcsc-cand-none-0',
          `点击必须真的落在这一行上（否则这条用例没有意义）：${JSON.stringify(click.point)}`)
        let s = await ctx.snap()
        check(s.checked.length === 0, `缺 UUID 的行点了也不能有选择，实际 ${JSON.stringify(s.checked)}`)
        check(s.importDisabled === true, '导入必须仍然禁用')
        check(s.importBodies.length === 0, '不能发出导入请求')
        // 同一会话里可选行照常能点：证明不是「所有原生点击都没生效」
        await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'name' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `同一会话里可选行必须照常能选中，实际 ${JSON.stringify(s.checked)}`)
      })
    },
  },
  {
    name: 'native-new: 新建封装行布局正确、可原生点选，提交 new 且不带 footprint_id',
    async run(ctx) {
      await session(ctx, { candidates: [CAND_A, CAND_B] }, async snap => {
        checkLayout(needRow(snap, 'lcsc-new-footprint', '新建行'), '新建行')
        const click = await ctx.click('lcsc-new-footprint', { aim: 'blank' })
        check(click.point.isRowElement === true, '行内空白点命中的必须是新建行本身')
        let s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify(['lcsc-new-footprint']),
          `新建行应被选中，实际 ${JSON.stringify(s.checked)}`)
        check(s.importDisabled === false, '选新建后应可导入')
        await ctx.click('lcsc-import', { aim: 'row' })
        s = await ctx.snap()
        const body = s.importBodies.at(-1)
        check(body?.footprint_choice?.mode === 'new', `提交 new 模式，实际 ${JSON.stringify(body)}`)
        check(!('footprint_id' in (body || {})), 'new 模式不能带 footprint_id')
      })
    },
  },
  {
    name: 'native-scroll: 多项候选列表内滚动后仍能点中最后一项并提交它的 UUID',
    async run(ctx) {
      const many = Array.from({ length: 12 }, (_, i) => ({
        id: `${String(i + 1).padStart(8, '0')}-1111-4111-8111-111111111111`,
        library: 'main', name: `FP-${i + 1}`, score: 0.9 - i * 0.01, pad_count: 4,
        footprint_status: 'available',
      }))
      const lastId = `lcsc-cand-${many[11].id}`
      await session(ctx, { candidates: many }, async snap => {
        const first = needRow(snap, `lcsc-cand-${many[0].id}`, '第一项')
        check(first.listScrollRange > 0,
          `候选列表必须真的需要滚动（scrollRange=${first.listScrollRange}）`)
        const click = await ctx.click(lastId, { aim: 'row' })
        const s = await ctx.snap()
        const lastRow = needRow(s, lastId, '最后一项')
        check(lastRow.listScrollTop > 0,
          `滚动到不可见的最后一项后，列表必须已经滚动过（scrollTop=${lastRow.listScrollTop}）`)
        check(click.point.rowTestid === lastId, '取点必须落在最后一项那一行上')
        check(JSON.stringify(s.checked) === JSON.stringify([lastId]),
          `选中的是点中的最后一项，实际 ${JSON.stringify(s.checked)}`)
        await ctx.click('lcsc-import', { aim: 'row' })
        const after = await ctx.snap()
        check(after.importBodies.at(-1)?.footprint_id === many[11].id,
          `提交最后一项的 UUID，实际 ${after.importBodies.at(-1)?.footprint_id}`)
      })
    },
  },
  {
    name: 'native-long: 超长候选名称换行不溢出，原生点击名称仍能选中',
    async run(ctx) {
      const longName = [{
        id: UUIDS.A,
        library: 'a-very-long-library-name-that-keeps-going-and-going-and-going',
        name: `SOT-223-3_L6.5-W3.5-P2.30-LS7.0-BR_${'X'.repeat(90)}-END`,
        score: 0.5, pad_count: 4, footprint_status: 'available',
      }]
      await session(ctx, { candidates: longName }, async snap => {
        const row = needRow(snap, `lcsc-cand-${UUIDS.A}`, '长名称行')
        checkLayout(row, '长名称行')
        check(row.nameLines >= 2, `超长名称必须换行，实际只有 ${row.nameLines} 行`)
        check(row.nameOverflowX <= 1, `名称自身横向溢出 ${row.nameOverflowX}px`)
        check(row.nameRect.right <= row.rowRect.right + 1,
          `名称右边界 ${num(row.nameRect.right)} 超出行右边界 ${num(row.rowRect.right)}`)
        const click = await ctx.click(`lcsc-cand-${UUIDS.A}`, { aim: 'name' })
        check(click.clicks[0].trusted === true, '点击长名称必须是 isTrusted=true')
        const s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify([`lcsc-cand-${UUIDS.A}`]),
          `点长名称也要能选中，实际 ${JSON.stringify(s.checked)}`)
      })
    },
  },
  {
    name: 'native-target: 目标库行布局正确，锁定行原生点击不生效，可选行能继续',
    async run(ctx) {
      await session(ctx, { stopAt: 'target' }, async snap => {
        for (const id of ['lcsc-target-pkg-ok', 'lcsc-target-pkg-locked', 'lcsc-target-pkg-paths'])
          checkLayout(needRow(snap, id, id), id)
        check(needRow(snap, 'lcsc-target-pkg-locked', '锁定行').selectable === 'false', '锁定行必须不可选')
        const locked = await ctx.click('lcsc-target-pkg-locked', { aim: 'name' })
        check(locked.point.rowTestid === 'lcsc-target-pkg-locked', '点击必须真的落在锁定行上')
        let s = await ctx.snap()
        check(s.checked.length === 0, `锁定行点了也不能被选中，实际 ${JSON.stringify(s.checked)}`)
        check(s.targetNextDisabled === true, '没有可用目标时不能继续')
        await ctx.click('lcsc-target-pkg-ok', { aim: 'blank' })
        s = await ctx.snap()
        check(JSON.stringify(s.checked) === JSON.stringify(['lcsc-target-pkg-ok']),
          `可选包应被选中，实际 ${JSON.stringify(s.checked)}`)
        check(s.targetNextDisabled === false, '选中可用包后应能继续')
      })
    },
  },
]

async function runNativeSuite(cdp) {
  const results = []
  await cdp.send('Page.bringToFront')
  for (const [width, height] of NATIVE_VIEWPORTS) {
    await cdp.send('Emulation.setDeviceMetricsOverride', {
      width, height, deviceScaleFactor: 1, mobile: false,
    })
    const vp = await cdp.eval('({ width: window.innerWidth, height: window.innerHeight })')
    if (vp?.width !== width || vp?.height !== height) {
      throw new Error(`viewport ${width}x${height} 没有生效：页面报告 ${vp?.width}x${vp?.height}`)
    }
    const ctx = {
      viewport: `${width}x${height}`,
      prepare: (spec) => {
        const snapshot = cdp.eval(`window.__harness.native.prepare(${JSON.stringify(spec || {})})`)
        return snapshot.then(s => {
          if (!s || !Array.isArray(s.rows)) throw new Error('native prepare 没有返回布局快照')
          return s
        })
      },
      begin: () => cdp.eval('window.__harness.native.begin()'),
      end: () => cdp.eval('window.__harness.native.end()'),
      click: (testid, opts) => clickPoint(cdp, testid, opts || {}),
      snap: () => cdp.eval('window.__harness.native.snapshot()'),
      eval: (expression) => cdp.eval(expression),
    }
    for (const def of NATIVE_SCENARIOS) {
      const name = `${def.name} @${width}x${height}`
      try {
        await def.run(ctx)
        results.push({ name, ok: true, viewport: `${width}x${height}` })
      } catch (cause) {
        results.push({ name, ok: false, viewport: `${width}x${height}`, error: String(cause?.message || cause) })
      }
    }
  }
  await cdp.send('Emulation.clearDeviceMetricsOverride')
  return results
}

// ---- 结果校验 ----
/// 0/0 不是通过：没有结果、结果不是数组、跑的不是要求的 mode，都必须报错
export function validateOutcome(outcome, expectedMode) {
  if (!outcome || typeof outcome !== 'object') throw new Error('harness returned no outcome')
  if (!Array.isArray(outcome.results) || outcome.results.length === 0)
    throw new Error('harness returned no scenario results (0/0 is not a pass)')
  if (expectedMode && outcome.mode !== expectedMode)
    throw new Error(`harness ran mode=${outcome.mode}, expected ${expectedMode}`)
  for (const r of outcome.results) {
    if (!r || typeof r.name !== 'string' || typeof r.ok !== 'boolean')
      throw new Error(`harness returned a malformed result: ${JSON.stringify(r)}`)
  }
  return outcome.results
}

/// 原生场景同样不许 0/0：一条都没跑、结果形状不对、两个 viewport 没覆盖满都算失败
export function validateNativeOutcome(results, viewports = NATIVE_VIEWPORTS) {
  if (!Array.isArray(results) || results.length === 0)
    throw new Error('no native scenarios ran (0/0 is not a pass)')
  for (const r of results) {
    if (!r || typeof r.name !== 'string' || typeof r.ok !== 'boolean' || typeof r.viewport !== 'string')
      throw new Error(`malformed native result: ${JSON.stringify(r)}`)
  }
  const covered = new Set(results.map(r => r.viewport))
  for (const [w, h] of viewports) {
    if (!covered.has(`${w}x${h}`))
      throw new Error(`native 场景没有覆盖 viewport ${w}x${h}（实际：${[...covered].join(', ')}）`)
  }
  return results
}

export async function main() {
  const mode = resolveMode()
  const consoleErrors = []
  const cleanupErrors = []
  const tempRoot = createTempRoot()   // 每次 main 唯一；import 不建任何东西
  let profileDir = null
  let edge = null
  let server = null
  let ws = null
  let failure = null
  try {
    await withTimeout((async () => {
      const port = await freePort()
      const vite = await startVite()
      server = vite.server
      const base = vite.base
      profileDir = createProfileDir(tempRoot)
      edge = launchEdge(port, profileDir)
      edge.on('error', (cause) => { consoleErrors.push(`edge spawn: ${cause.message}`) })

      await waitFor(async () => {
        const r = await fetch(`http://127.0.0.1:${port}/json/version`)
        return r.ok ? r.json() : null
      }, 30000, 'edge devtools endpoint')

      const pageUrl = `${base}/${MODE_PAGE[mode]}`
      const created = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(pageUrl)}`,
        { method: 'PUT' })
      const target = await created.json()
      const socket = new WebSocket(target.webSocketDebuggerUrl)
      await new Promise((resolve, reject) => {
        socket.addEventListener('open', resolve, { once: true })
        socket.addEventListener('error', () => reject(new Error('cdp socket error')), { once: true })
      })
      ws = socket
      const cdp = new Cdp(socket, { onConsoleError: (e) => consoleErrors.push(e) })
      await cdp.send('Runtime.enable')
      await cdp.send('Page.enable')

      await waitFor(() => cdp.eval('window.__ready === true'), 45000, 'harness page')

      const runTimeoutMs = HARNESS_RUN_TIMEOUT_MS[mode]
      const options = { onlyImportProgress: process.argv.includes('--import-progress'),
        onlySettingsImport: process.argv.includes('--settings-import') }
      const outcome = await cdp.eval(`window.__harness.run(${JSON.stringify(mode)}, ${JSON.stringify(options)})`,
        runTimeoutMs ? { timeoutMs: runTimeoutMs } : {})
      const results = validateOutcome(outcome, mode)
      if (mode === 'type-matching') {
        for (const [width, height] of [[1440, 900], [390, 844]]) {
          await cdp.send('Emulation.setDeviceMetricsOverride', {
            width, height, deviceScaleFactor: 1, mobile: false,
          })
          for (const dark of [false, true]) {
            const layout = await cdp.eval(`window.__harness.layout(${JSON.stringify({ width, height, dark })})`)
            results.push(...validateOutcome(layout, mode))
          }
        }
      }
      let failed = 0
      for (const r of results) {
        if (r.ok) console.log(`  ok   ${r.name}`)
        else { failed += 1; console.log(`  FAIL ${r.name}\n       ${r.error}`) }
      }
      console.log(`\n[${mode}] ${results.length - failed}/${results.length} 场景通过`)

      // 专用流程还要跑原生鼠标场景（真实 CDP Input，不是 DOM dispatchEvent）。
      // legacy 是旧行为记录，类型管理/匹配是独立页面：这些模式不跑 LCSC 原生套件，
      // 但 new 的原生套件一条都不能少。
      if (mode === 'new') {
        const nativeResults = validateNativeOutcome(await runNativeSuite(cdp))
        let nativeFailed = 0
        for (const r of nativeResults) {
          if (r.ok) console.log(`  ok   ${r.name}`)
          else { nativeFailed += 1; console.log(`  FAIL ${r.name}\n       ${r.error}`) }
        }
        console.log(`\n[native] ${nativeResults.length - nativeFailed}/${nativeResults.length} 场景通过`
          + `（viewport: ${NATIVE_VIEWPORTS.map(v => v.join('x')).join(', ')}）`)
        failed += nativeFailed
      }
      if (consoleErrors.length) {
        console.log('\nconsole errors:')
        for (const e of consoleErrors.slice(0, 20)) console.log(`  ${e}`)
      }
      // 页面里的 React 报错（Invalid hook call 之类）本身就是失败信号
      if (consoleErrors.length) failed += 1
      if (failed) failure = new Error(`${failed} scenario/console failure(s)`)
    })(), RUN_TIMEOUT_MS, 'lcsc browser harness')
  } catch (cause) {
    failure = cause
  } finally {
    // 清理顺序固定：先停自己启动的 Edge（等人退干净），再关 Vite，最后删临时目录。
    // 每一步的失败都记录下来 —— 清理不干净就不能算通过。
    if (ws) { try { ws.close() } catch (cause) { cleanupErrors.push(`close cdp socket: ${cause.message}`) } }
    if (edge) {
      try { await stopEdge(edge) } catch (cause) { cleanupErrors.push(`stop edge: ${cause.message}`) }
    }
    if (server) {
      try { await server.close() } catch (cause) { cleanupErrors.push(`close vite: ${cause.message}`) }
    }
    if (profileDir) {
      try { removeProfileDir(profileDir, tempRoot) } catch (cause) {
        cleanupErrors.push(`remove profile: ${cause.message}`)
      }
    }
    try { removeTempRoot(tempRoot) } catch (cause) { cleanupErrors.push(`remove temp root: ${cause.message}`) }
  }
  if (cleanupErrors.length) {
    const detail = `${cleanupErrors.length} cleanup failure(s): ${cleanupErrors.join('; ')}`
    throw new Error(failure ? `${failure.message}; ${detail}` : detail)
  }
  if (failure) throw failure
}

/// 供别的测试文件复用（不自动执行）
export { main as runLcscBrowser }

// 直接执行（含 `node --test <此文件>`）时才自动跑；被别的测试文件 import 时不跑
const isEntry = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)
if (isEntry) {
  main().then(() => {
    process.exit(0)
  }).catch((cause) => {
    console.error(`runner failed: ${cause?.message || cause}`)
    const envProblem = /Edge not found|vite did not|cdp socket/.test(String(cause?.message || ''))
    process.exit(envProblem ? 2 : 1)
  })
}
