// 立创导入流程的实际逻辑（组件用的就是这些函数，不是重写一遍的镜像）：
// 编号规范化、查找/手动搜索的会话与版本校验、插件包装解包、目标元件库校验、
// 候选身份统一（footprint_id 别名）、提交体构造。
import test from 'node:test'
import assert from 'node:assert/strict'

import { readdirSync, readFileSync, writeFileSync, existsSync, lstatSync, rmSync, symlinkSync } from 'node:fs'
import { tmpdir } from 'node:os'
import path from 'node:path'

import {
  normalizeLcscId, createRequestGuard, pluginOutcome, parseAnalyzeResult, sanitizeCandidates,
  validateTargetLibrary, targetSnapshot, sameTarget,
  candidateIdentity, candidateSelectable, candidateAvailable, candidateChoice,
  buildImportBody, newOperationId, formatScore, refreshOutcome,
  parseSymbolLibraries, normalizeSymbolLibraryName, validateSymbolLibraryChoice, symbolLibraryPath,
} from '../../../plugins/lcsc_import/frontend/src/lcscImport.js'

// ---------- 编号规范化 ----------
test('编号规范化：纯数字补 C、大写、去首尾空白', () => {
  assert.deepEqual(normalizeLcscId('37593'), { ok: true, value: 'C37593' })
  assert.deepEqual(normalizeLcscId('  c37593 '), { ok: true, value: 'C37593' })
  assert.deepEqual(normalizeLcscId('C37593'), { ok: true, value: 'C37593' })
})

test('编号规范化：空与非法输入分别给出原因', () => {
  assert.equal(normalizeLcscId('   ').reason, 'empty')
  assert.equal(normalizeLcscId('').reason, 'empty')
  assert.equal(normalizeLcscId('C12A3').ok, false)
  assert.equal(normalizeLcscId('../etc/passwd').ok, false)
  assert.equal(normalizeLcscId('C').ok, false)
  assert.equal(normalizeLcscId(null).ok, false)
})

// ---------- 请求会话 / 版本（组件里用的同一个 guard） ----------
test('guard：新请求让旧 token 失效，旧 finally 不能动新请求的状态', () => {
  const guard = createRequestGuard()
  const first = guard.begin()
  assert.equal(guard.isCurrent(first), true, 'fresh token is current')

  const second = guard.begin()
  assert.equal(guard.isCurrent(first), false, 'starting a new lookup invalidates the old one')
  assert.equal(guard.isCurrent(second), true)
})

test('guard：改编号/关闭（invalidate）之后在途响应与 finally 全部作废', () => {
  const guard = createRequestGuard()
  const token = guard.begin()
  guard.invalidate()
  assert.equal(guard.isCurrent(token), false, 'invalidated session drops in-flight tokens')

  // 失效之后的新请求重新有效，旧 token 依旧无效
  const next = guard.begin()
  assert.equal(guard.isCurrent(next), true)
  assert.equal(guard.isCurrent(token), false)
})

test('guard：旧请求先返回也不算当前，新请求仍处于 pending', () => {
  const guard = createRequestGuard()
  const oldToken = guard.begin()
  const newToken = guard.begin()
  // 旧请求此刻返回：它的回调被丢弃；新请求还在飞 → 组件的 loading 保持 true
  assert.equal(guard.isCurrent(oldToken), false)
  assert.equal(guard.isCurrent(newToken), true)
  assert.equal(guard.isCurrent(undefined), false)
})

test('guard：同一 token 被重复使用时不会互相冒充', () => {
  const guard = createRequestGuard()
  const guardB = createRequestGuard()
  const token = guard.begin()
  assert.equal(guardB.isCurrent(token), false, 'tokens are scoped to their own guard')
})

// ---------- 插件返回包装 ----------
test('插件包装：外层失败、内层失败都算失败，具体错误优先内层', () => {
  assert.deepEqual(pluginOutcome(null), { ok: false, error: 'Invalid plugin response' })
  assert.equal(pluginOutcome({ ok: false, error: 'http 500' }).error, 'http 500')
  assert.equal(pluginOutcome({ ok: false }).ok, false)
  const inner = pluginOutcome({ ok: true, plugin_result: { ok: false, error: 'conversion failed' } })
  assert.equal(inner.ok, false)
  assert.equal(inner.error, 'conversion failed')
  assert.equal(pluginOutcome({ ok: true, plugin_result: { ok: true, candidates: [] } }).ok, true)
})

test('真实包装：outer ok=false + inner ok=false 时必须显示内层的具体原因', () => {
  // routes.cpp 把插件的 {ok:false,error:"..."} 包成 {ok:false, plugin_result:{...}}，
  // 外层没有 error 字段 —— 通用文案会把下载/转换失败的具体原因吃掉
  const real = { ok: false, plugin_result: { ok: false, error: 'download conversion reason' } }
  const outcome = pluginOutcome(real)
  assert.equal(outcome.ok, false)
  assert.equal(outcome.error, 'download conversion reason')
  assert.notEqual(outcome.error, 'Plugin request failed')

  // 外层完全没有信息时才退回通用文案
  assert.equal(pluginOutcome({ ok: false }).error, 'Plugin request failed')
  assert.equal(pluginOutcome({ ok: false, plugin_result: { ok: false } }).error, 'Plugin request failed')
})

test('候选清洗：null/数字/数组等损坏元素被剔除并计数，合法候选全部保留', () => {
  const valid = { id: 'u1', name: 'SOT-223' }
  const sanitized = sanitizeCandidates([valid, null, 42, 'x', [], { footprint_id: 'u2' }])
  assert.equal(sanitized.ok, true)
  assert.deepEqual(sanitized.items, [valid, { footprint_id: 'u2' }])
  assert.equal(sanitized.dropped, 4)

  assert.deepEqual(sanitizeCandidates(null), { ok: false, items: [], dropped: 0 })
  assert.deepEqual(sanitizeCandidates({ id: 'u1' }), { ok: false, items: [], dropped: 0 })
  assert.deepEqual(sanitizeCandidates([]), { ok: true, items: [], dropped: 0 })
})

test('analyze 结果：损坏候选不会打崩解析，计数交给界面提示', () => {
  const parsed = parseAnalyzeResult({
    ok: true,
    plugin_result: {
      ok: true, lcsc_id: 'C37593', footprint_name: 'SOT-223',
      candidates: [{ id: 'u1', name: 'OK' }, null, 7],
    },
  })
  assert.equal(parsed.ok, true)
  assert.deepEqual(parsed.data.candidates, [{ id: 'u1', name: 'OK' }])
  assert.equal(parsed.data.droppedCandidates, 2)
})

test('analyze 结果：完整保留候选，不筛选不改序', () => {
  const candidates = [
    { id: 'u2', library: 'b', name: 'B', score: 0.4, pad_count: 6 },
    { id: 'u1', library: 'a', name: 'A', score: 0.9, pad_count: 3 },
  ]
  const parsed = parseAnalyzeResult({
    ok: true,
    plugin_result: {
      ok: true, lcsc_id: 'C37593', symbol_names: ['AMS1117-3.3'],
      footprint_name: 'SOT-223', model_name: 'SOT-223', candidates,
    },
  })
  assert.equal(parsed.ok, true)
  assert.deepEqual(parsed.data.candidates, candidates, 'order and content preserved')
  assert.equal(parsed.data.footprintName, 'SOT-223')

  const failed = parseAnalyzeResult({ ok: true, plugin_result: { ok: false, error: 'no such part' } })
  assert.equal(failed.ok, false)
  assert.equal(failed.error, 'no such part')
  assert.equal(parseAnalyzeResult({ ok: true, plugin_result: { ok: true } }).ok, false,
    'a response without candidates/footprint is not a usable analysis')
})

// ---------- 目标元件库校验（字段与 GET /api/component-libraries 一致） ----------
const okLib = {
  id: 'pkg-1', name: '主元件库', symbol_path: 'D:\\libs\\sym', footprint_path: 'D:\\libs\\fp',
  model_3d_path: '', enabled: true, sort_order: 0, locked: false, is_protected: false,
}

test('目标校验：可写包通过，锁定/保护/停用/缺路径逐项给出原因', () => {
  assert.deepEqual(validateTargetLibrary(okLib), { ok: true, reason: '' })
  assert.equal(validateTargetLibrary(null).reason, 'missing')
  assert.equal(validateTargetLibrary({ name: 'x' }).reason, 'missing')
  assert.equal(validateTargetLibrary({ ...okLib, enabled: false }).reason, 'disabled')
  assert.equal(validateTargetLibrary({ ...okLib, locked: true }).reason, 'locked')
  assert.equal(validateTargetLibrary({ ...okLib, is_protected: true }).reason, 'protected')
  assert.equal(validateTargetLibrary({ ...okLib, symbol_path: '' }).reason, 'paths')
  assert.equal(validateTargetLibrary({ ...okLib, footprint_path: '   ' }).reason, 'paths')
})

test('目标快照：名称/路径/保护状态任一变化都必须重新确认', () => {
  const snapshot = targetSnapshot(okLib)
  assert.equal(sameTarget(snapshot, targetSnapshot({ ...okLib })), true)
  for (const changed of [
    { ...okLib, name: '改名' },
    { ...okLib, symbol_path: 'E:\\other' },
    { ...okLib, footprint_path: 'E:\\other' },
    { ...okLib, locked: true },
    { ...okLib, is_protected: true },
    { ...okLib, enabled: false },
  ]) {
    assert.equal(sameTarget(snapshot, targetSnapshot(changed)), false,
      `change must invalidate the confirmation: ${JSON.stringify(changed)}`)
  }
  assert.equal(sameTarget(snapshot, null), false)
})

// ---------- 候选身份（UUID 才算身份） ----------
test('候选身份：id 与 footprint_id 别名等价，其它字段不冒充身份', () => {
  assert.equal(candidateIdentity({ id: 'u1' }), 'u1')
  assert.equal(candidateIdentity({ footprint_id: 'u2' }), 'u2')
  assert.equal(candidateIdentity({ id: 'u1', footprint_id: 'u2' }), 'u1', 'id wins when both exist')
  assert.equal(candidateIdentity({ library: 'main', name: 'SOT-223' }), '')
  assert.equal(candidateIdentity(null), '')
})

test('候选选择：footprint_id 别名的候选同样能生成带 UUID 的复用选择', () => {
  // 正常候选必须显式带 available：存在性由服务端状态决定，测试夹具不替它兜底
  const aliased = { footprint_id: 'u9', library: 'main', name: 'SOT-223', footprint_status: 'available' }
  assert.equal(candidateSelectable(aliased), true)
  const choice = candidateChoice(aliased)
  assert.equal(choice.mode, 'reuse')
  assert.equal(choice.id, 'u9', 'identity is carried into the choice, not dropped')

  assert.equal(candidateSelectable({ library: 'main', name: 'SOT-223' }), false)
  assert.equal(candidateChoice({ library: 'main', name: 'SOT-223' }), null,
    'a candidate without a UUID is never turned into a choice')
})

test('候选可用性：未检查或失效的封装不能选，即使 UUID 与名称都在', () => {
  const id = 'aaaaaaaa-1111-4111-8111-111111111111'
  // 缺失状态 = 未检查 → 不可选
  assert.equal(candidateSelectable({ id, library: 'main', name: 'P1' }), false)
  assert.equal(candidateAvailable({ id, library: 'main', name: 'P1' }), false)
  // 失效/歧义/检查失败 → 不可选
  for (const status of ['stale', 'ambiguous', 'not_found', 'check_failed', 'unassigned']) {
    assert.equal(candidateSelectable({ id, footprint_status: status }), false, status)
  }
  // 旧字段说「有封装」也没用
  assert.equal(candidateSelectable({ id, has_footprint: true, has_3d_model: true }), false)
  // 明确 available 才可选，且照常生成选择
  assert.equal(candidateSelectable({ id, footprint_status: 'available' }), true)
  assert.equal(candidateChoice({ id, footprint_status: 'available' }).id, id)
})

test('候选可用性：失效候选仍然可以生成身份对象，拒绝交给界面/host', () => {
  // candidateChoice 只做身份；是否可选由 candidateSelectable 决定 —— 两条规则
  // 分开，避免「不能选」被误当成「没有身份」
  const stale = { id: 'u10', footprint_status: 'stale', footprint_reason: 'missing_file' }
  assert.equal(candidateSelectable(stale), false)
  assert.equal(candidateChoice(stale).id, 'u10')
})

// ---------- 提交体 ----------
test('符号子库：空列表与损坏响应严格区分，嵌套同名路径保留身份', () => {
  assert.deepEqual(parseSymbolLibraries({ ok: true, libraries: [], can_create: true }),
    { ok: true, libraries: [], canCreate: true })
  for (const response of [null, [], {}, { ok: true, libraries: [] },
    { ok: false, libraries: [], can_create: true },
    { ok: true, libraries: [{ name: 'Device', path: '../Device.kicad_sym' }], can_create: true }])
    assert.equal(parseSymbolLibraries(response).ok, false)
  const libraries = [{ name: 'Device', path: 'a/Device.kicad_sym', id: 'a' },
    { name: 'Device', path: 'b/Device.kicad_sym', id: 'b' }]
  assert.deepEqual(parseSymbolLibraries({ ok: true, libraries, can_create: false }).libraries, libraries)
  assert.equal(parseSymbolLibraries({ ok: true, can_create: true,
    libraries: [{ name: 'Mixed', path: 'Mixed.KICAD_SYM' }] }).ok, true)
})

test('符号子库：新名称只接受单文件名，单次去尾缀并检查根目录同名', () => {
  assert.deepEqual(normalizeSymbolLibraryName(' Switches.kicad_sym '), { ok: true, name: 'Switches' })
  assert.deepEqual(normalizeSymbolLibraryName(' Mixed.KICAD_SYM '), { ok: true, name: 'Mixed' })
  for (const value of ['', '../Device', 'a/Device', 'a\\Device', 'CON', 'a.kicad_sym.kicad_sym', 'a.KICAD_SYM.kicad_sym', 'x.',
    `x${String.fromCodePoint(127)}`, '元'.repeat(82)])
    assert.equal(normalizeSymbolLibraryName(value).ok, false, value)
  assert.equal(normalizeSymbolLibraryName('元'.repeat(81)).ok, true, '253 UTF8 bytes including extension fits')
  const catalog = parseSymbolLibraries({ ok: true, can_create: true,
    libraries: [{ name: 'Device', path: 'Device.kicad_sym' }, { name: 'Device', path: 'nested/Device.kicad_sym' }] })
  assert.equal(validateSymbolLibraryChoice(null, catalog).ok, false)
  assert.equal(validateSymbolLibraryChoice({ mode: 'existing', path: 'nested/Device.kicad_sym' }, catalog).ok, true)
  assert.equal(validateSymbolLibraryChoice({ mode: 'existing', path: 'missing.kicad_sym' }, catalog).reason, 'gone')
  assert.equal(validateSymbolLibraryChoice({ mode: 'new', name: 'Device' }, catalog).reason, 'exists')
  assert.equal(validateSymbolLibraryChoice({ mode: 'new', name: 'Device' }, catalog, { replay: true }).ok, true)
  assert.equal(validateSymbolLibraryChoice({ mode: 'new', name: 'Switches' }, { ...catalog, canCreate: false }).reason, 'cannotCreate')
  assert.equal(symbolLibraryPath({ mode: 'new', name: 'Switches' }), 'Switches.kicad_sym')
})

test('提交体：显式子库选择独立于包与封装，旧 helper 调用仍可兼容', () => {
  for (const symbolLibraryChoice of [{ mode: 'existing', path: 'nested/Device.kicad_sym' }, { mode: 'new', name: 'Switches' }]) {
    const body = buildImportBody({ lcscId: 'C1', targetComponentLibraryId: 'p', choice: { mode: 'new' }, symbolLibraryChoice })
    assert.deepEqual(body.symbol_library_choice, symbolLibraryChoice)
    assert.equal('target_library_id' in body, false)
  }
  assert.equal('symbol_library_choice' in buildImportBody({ lcscId: 'C1', targetComponentLibraryId: 'p', choice: { mode: 'new' } }), false)
})

test('提交体：复用发送候选 UUID 与元件库包 ID，不带旧符号库字段', () => {
  const body = buildImportBody({
    lcscId: 'C37593', targetComponentLibraryId: 'pkg-1',
    choice: candidateChoice({ footprint_id: 'u9', library: 'main', name: 'SOT-223' }),
    operationId: 'lcsc-op-1',
  })
  assert.equal(body.action, 'import')
  assert.equal(body.lcsc_id, 'C37593')
  assert.equal(body.target_component_library_id, 'pkg-1')
  assert.equal(body.footprint_id, 'u9')
  assert.deepEqual(body.footprint_choice, { mode: 'reuse', id: 'u9', library: 'main', name: 'SOT-223' })
  assert.equal('target_library' in body, false)
  assert.equal('target_library_id' in body, false)
  assert.equal('target_library_path' in body, false)
  assert.equal(body.operation_id, 'lcsc-op-1')
})

test('提交体：新建清除旧 footprint_id', () => {
  const body = buildImportBody({
    lcscId: 'C37593', targetComponentLibraryId: 'pkg-1',
    choice: { mode: 'new' }, operationId: 'lcsc-op-2',
  })
  assert.deepEqual(body.footprint_choice, { mode: 'new' })
  assert.equal('footprint_id' in body, false)
})

test('操作 ID：稳定、安全字符集，只有显式重新生成才变化', () => {
  const a = newOperationId()
  const b = newOperationId()
  assert.match(a, /^lcsc-[A-Za-z0-9_-]+$/)
  assert.ok(a.length <= 64)
  assert.notEqual(a, b, 'a fresh intent gets a fresh id')
})

test('相似度显示：0-1 小数转百分比，百分数与非数字原样处理', () => {
  assert.equal(formatScore(0.94), '94%')
  assert.equal(formatScore(1), '100%')
  assert.equal(formatScore(87), '87%')
  assert.equal(formatScore('n/a'), '')
})

// ---------- 导入后的刷新结果（真实 useRequest(...).load 的契约） ----------
test('刷新结果：null（useRequest 失败）必须算失败，不能当成功关闭', () => {
  assert.equal(refreshOutcome('symbols', null).ok, false)
  assert.equal(refreshOutcome('libraries', null).ok, false)
  assert.equal(refreshOutcome('status', null).ok, false)
  // 失败原因要能定位到具体是哪一项刷新（界面据此显示对应文案）
  assert.equal(refreshOutcome('symbols', null).reason, 'symbols')
  assert.equal(refreshOutcome('libraries', null).reason, 'libraries')
  assert.equal(refreshOutcome('status', null).reason, 'status')
})

test('刷新结果：形状不符（undefined / 非数组 / 空响应）也算失败', () => {
  assert.equal(refreshOutcome('symbols', undefined).ok, false)
  assert.equal(refreshOutcome('libraries', {}).ok, false)
  assert.equal(refreshOutcome('status', []).ok, false)
  assert.equal(refreshOutcome('status', 'ok').ok, false)
})

test('刷新结果：真实的成功返回值（数组 / 状态对象）才算成功', () => {
  assert.equal(refreshOutcome('symbols', []).ok, true)
  assert.equal(refreshOutcome('symbols', [{ id: 's1' }]).ok, true)
  assert.equal(refreshOutcome('libraries', []).ok, true)
  assert.equal(refreshOutcome('status', { symbols: 0, footprints: 0 }).ok, true)
})

// ---------- runner 的资源纪律（webui/tests/browser/run-lcsc-browser.mjs） ----------
// import runner 不得有任何副作用：profile 只能由 main 在本次临时根里创建
const profilesBeforeImport = readdirSync(tmpdir()).filter(n => n.startsWith('kforge-lcsc-edge-'))
const runner = await import('../browser/run-lcsc-browser.mjs')

test('runner：import 不创建 profile，profile 只落在本次临时根里', () => {
  const added = readdirSync(tmpdir())
    .filter(n => n.startsWith(runner.PROFILE_PREFIX) && !profilesBeforeImport.includes(n))
  assert.deepEqual(added, [], 'importing the runner must not create profile dirs')

  const tempRoot = runner.createTempRoot()
  try {
    assert.ok(path.basename(tempRoot).startsWith(runner.TEMP_ROOT_PREFIX), '每次运行一个唯一临时根')
    const profile = runner.createProfileDir(tempRoot)
    assert.ok(path.basename(profile).startsWith(runner.PROFILE_PREFIX), 'profile 带专用前缀')
    assert.equal(path.dirname(path.resolve(profile)), path.resolve(tempRoot), 'profile 落在本次临时根里')
    runner.removeProfileDir(profile, tempRoot)
    assert.equal(readdirSync(tempRoot).length, 0, 'profile 真的被删掉了')
  } finally {
    runner.removeTempRoot(tempRoot)
  }
})

test('runner：删除前拒绝父目录/根/临时根自身/越界路径', () => {
  const tempRoot = runner.createTempRoot()
  try {
    const profile = runner.createProfileDir(tempRoot)
    assert.throws(() => runner.removeProfileDir(tempRoot, tempRoot), /refusing/, '临时根自身不能删')
    assert.throws(() => runner.removeProfileDir(tmpdir(), tempRoot), /refusing/, '越界的父目录不能删')
    assert.throws(() => runner.removeProfileDir(path.parse(profile).root, tempRoot), /refusing/, '根目录不能删')
    assert.throws(() => runner.removeProfileDir(path.join(tempRoot, 'other-dir'), tempRoot), /refusing/, '前缀不符不能删')
    assert.throws(() => runner.removeProfileDir('', tempRoot), /refusing/, '空路径不能删')
    assert.throws(() => runner.removeProfileDir(profile, ''), /tempRoot/, '没有临时根不能删')
    runner.removeProfileDir(profile, tempRoot)   // 合法目标照常删掉
  } finally {
    runner.removeTempRoot(tempRoot)
  }
})

// ---------- runner：临时目录删除不能顺着 junction/symlink 删到别处 ----------
// 用 Node 官方 fs API 在系统 tmpdir 下真造同级链接（Windows 用 junction，不需要管理员）：
// 只断言「拒绝删除 + 目标原样保留」，绝不拿链接去试真删目标。
const symlinkDir = (target, linkPath) => symlinkSync(
  target, linkPath, process.platform === 'win32' ? 'junction' : 'dir')

test('runner：临时根是 junction 时拒绝删除，链接指向的目录与文件原样保留', () => {
  const decoy = runner.createTempRoot()          // 本次测试自己造的目标目录（无关的同级目录）
  const keep = path.join(decoy, 'keep.txt')
  writeFileSync(keep, 'must survive')
  const linkRoot = path.join(tmpdir(), `${runner.TEMP_ROOT_PREFIX}link-${process.pid}-${Date.now()}`)
  try {
    symlinkDir(decoy, linkRoot)
    assert.equal(lstatSync(linkRoot).isSymbolicLink(), true, '夹具确实是链接（Windows 上是 junction）')
    assert.throws(() => runner.removeTempRoot(linkRoot), /symlink or junction/,
      '不能顺着链接把根删掉')
    assert.throws(() => runner.removeProfileDir(linkRoot, linkRoot), /symlink or junction/,
      'profile 清理也要先确认本次临时根没有被换成链接')
    assert.throws(() => runner.removeProfileDir(path.join(linkRoot, `${runner.PROFILE_PREFIX}x`), linkRoot),
      /symlink or junction/, 'profile 路径经过链接根时同样拒绝')
    assert.equal(existsSync(decoy), true, '链接指向的目录还在')
    assert.equal(readFileSync(keep, 'utf8'), 'must survive', '目标里的文件必须原样保留')
  } finally {
    rmSync(linkRoot, { force: true, maxRetries: 5 })   // 只摘掉测试用的链接本身
  }
  assert.equal(existsSync(linkRoot), false, '测试链接自己清掉了')
  assert.equal(readFileSync(keep, 'utf8'), 'must survive', '清链接不会碰目标里的文件')
  runner.removeTempRoot(decoy)                    // 真实目录照常走安全 guard 正常清理
  assert.equal(existsSync(decoy), false, '非链接的临时根仍能被清理')
})

test('runner：profile 本身是链接时拒绝删除，指向的目录与文件原样保留', () => {
  const tempRoot = runner.createTempRoot()
  const decoy = runner.createTempRoot()
  const keep = path.join(decoy, 'keep.txt')
  writeFileSync(keep, 'must survive')
  const linkProfile = path.join(tempRoot, `${runner.PROFILE_PREFIX}link`)
  try {
    symlinkDir(decoy, linkProfile)
    assert.throws(() => runner.removeProfileDir(linkProfile, tempRoot), /refusing/,
      '解析后不在本次临时根里的 profile 不能删')
    assert.equal(existsSync(decoy), true, '链接指向的目录还在')
    assert.equal(readFileSync(keep, 'utf8'), 'must survive', '目标里的文件必须原样保留')
  } finally {
    rmSync(linkProfile, { force: true, maxRetries: 5 })
    runner.removeTempRoot(decoy)
    runner.removeTempRoot(tempRoot)
  }
  assert.equal(existsSync(decoy), false, '测试自己的目录照常清理')
})

test('runner：CDP 命令有超时，断线立刻让在途命令失败', async () => {
  class FakeSocket {
    constructor() { this.listeners = {} }
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn) }
    emit(type, event) { for (const fn of this.listeners[type] || []) fn(event) }
    send() { /* 只记录不回包：模拟对端不响应 */ }
  }

  const silent = new FakeSocket()
  const cdp = new runner.Cdp(silent, { timeoutMs: 30 })
  await assert.rejects(() => cdp.send('Runtime.evaluate', {}), /timed out/, '命令不能无限等下去')

  const closing = new FakeSocket()
  const cdp2 = new runner.Cdp(closing, { timeoutMs: 5000 })
  const pending = cdp2.send('Runtime.evaluate', {})
  closing.emit('close', {})
  await assert.rejects(() => pending, /cdp socket closed/, 'socket 断开必须 reject 在途命令')
  await assert.rejects(() => cdp2.send('Runtime.enable', {}), /cdp socket closed/, '断线后不再发新命令')
})

test('runner：0/0 或缺失结果不是通过，mode 不符也要拒绝', () => {
  assert.throws(() => runner.validateOutcome(null, 'new'), /no outcome/)
  assert.throws(() => runner.validateOutcome({ mode: 'new', results: [] }, 'new'), /0\/0/)
  assert.throws(() => runner.validateOutcome({ mode: 'new' }, 'new'), /no scenario results/)
  assert.throws(() => runner.validateOutcome({ mode: 'legacy', results: [{ name: 'a', ok: true }] }, 'new'), /mode/)
  assert.throws(() => runner.validateOutcome({ mode: 'new', results: [null] }, 'new'), /malformed/)
  const ok = runner.validateOutcome({ mode: 'new', results: [{ name: 'a', ok: true }] }, 'new')
  assert.equal(ok.length, 1)
})

test('runner：原生场景同样不许 0/0，view 覆盖不全/结果畸形都要拒绝', () => {
  assert.throws(() => runner.validateNativeOutcome([]), /0\/0/, '一条原生场景都没跑不算通过')
  assert.throws(() => runner.validateNativeOutcome(null), /0\/0/)
  assert.throws(() => runner.validateNativeOutcome([{ name: 'a', ok: true }]), /malformed/,
    '缺少 viewport 记录的原生结果算畸形')
  assert.throws(
    () => runner.validateNativeOutcome([{ name: 'a', ok: true, viewport: '1280x900' }]),
    /viewport 960x640/, '两个 viewport 没覆盖满要拒绝')
  const ok = runner.validateNativeOutcome([
    { name: 'a', ok: true, viewport: '1280x900' },
    { name: 'b', ok: false, viewport: '960x640', error: 'boom' },
  ])
  assert.equal(ok.length, 2, '合法结果原样返回（ok=false 由调用方计数）')
})

test('runner：有界等待对被等的挂起 Promise 也生效', async () => {
  await assert.rejects(
    () => runner.withTimeout(new Promise(() => {}), 30, 'hang'),
    /timed out/, '挂住的 Promise 不能无视 deadline')
  assert.equal(await runner.withTimeout(Promise.resolve('v'), 1000, 'fast'), 'v')
})

// ---------- runner：stopEdge 只停自己启动的那个子进程 ----------
// 下面每个用例都注入 spawnKill：既能真的判定「有没有走终止路径」，也保证测试
// 绝不会对夹具里那些假 PID 真的调用 taskkill（注入的替身不会被调用时就是空记录）。
function fakeProc({ pid = 4242, exitCode = null, signalCode = null } = {}) {
  const listeners = { exit: [], error: [] }
  const proc = {
    pid, exitCode, signalCode, killed: false,
    on(type, fn) { (listeners[type] ||= []).push(fn); return proc },
    once(type, fn) { return proc.on(type, fn) },
    emit(type, ...args) { for (const fn of (listeners[type] || []).splice(0)) fn(...args) },
    kill() { proc.killed = true },
  }
  return proc
}
const isWindows = process.platform === 'win32'
const nextTick = () => new Promise(r => setImmediate(r))

test('runner：已退出的子进程直接返回，绝不再对旧 PID 发 taskkill', async () => {
  const attempted = []
  const spawnKill = (pid) => { attempted.push(pid); return fakeProc() }
  for (const dead of [
    fakeProc({ pid: 4294967290, exitCode: 0 }),                            // 正常退出
    fakeProc({ pid: 4294967290, exitCode: null, signalCode: 'SIGKILL' }),  // 被信号结束
  ]) {
    const returned = await runner.withTimeout(
      runner.stopEdge(dead, { spawnKill }), 1000, 'stopEdge(exited)')
    assert.equal(returned, undefined, '已退出的子进程必须直接返回')
    assert.equal(dead.killed, false, '兜底 child.kill 也不能碰')
  }
  assert.deepEqual(attempted, [],
    '旧 PID 可能已被系统复用给无关进程：一次 taskkill 都不能发出去')
})

test('runner：仍存活的子进程先 taskkill 本次 PID 树并等它退出，不抢跑兜底 kill', async () => {
  const attempted = []
  const child = fakeProc({ pid: 4242 })
  const killer = fakeProc()          // 替身：真实实现是 spawn('taskkill', ['/pid', pid, '/T', '/F'])
  const pending = runner.stopEdge(child, {
    spawnKill: (pid) => { attempted.push(pid); return killer },
    exitTimeoutMs: 300,
  })
  await nextTick()
  assert.deepEqual(attempted, isWindows ? [4242] : [],
    isWindows ? '只对本次自己的 PID 发 taskkill（/T 杀整棵树）' : '非 Windows 平台不发 taskkill')
  assert.equal(child.killed, false, '进程树还没退，兜底 kill 不能抢在前面')
  if (isWindows) { killer.emit('exit', 0); await nextTick() }
  assert.equal(child.killed, false, '还在有界等待它自己退出')

  child.exitCode = 0
  child.emit('exit')                 // 进程树真的退干净了
  await runner.withTimeout(pending, 2000, 'stopEdge(alive)')
  assert.equal(child.killed, false, '自己退干净了就不再补刀')
})

test('runner：taskkill 之后进程树仍不退 → 有界等待后用 child.kill 补刀', async () => {
  const child = fakeProc({ pid: 4243 })
  child.kill = () => { child.killed = true; child.exitCode = 0; child.emit('exit') }
  await runner.withTimeout(runner.stopEdge(child, {
    spawnKill: () => { const killer = fakeProc(); setImmediate(() => killer.emit('exit', 0)); return killer },
    exitTimeoutMs: 60,
  }), 3000, 'stopEdge(fallback)')
  assert.equal(child.killed, true, '兜底只杀自己启动的那个子进程')
})

test('runner：taskkill 期间子进程自己退出 → 不误报清理失败', async () => {
  const child = fakeProc({ pid: 4244 })
  // 退出由子进程自身触发；非 Windows 不调用 spawnKill，同样必须发出退出事件。
  // Windows 下该事件先于 taskkill 回执，保留“检查后、回执前退出”的竞态。
  setImmediate(() => {
    child.exitCode = 0
    child.emit('exit')
  })
  await runner.withTimeout(runner.stopEdge(child, {
    spawnKill: () => {
      const killer = fakeProc()
      setImmediate(() => {
        killer.emit('exit', 128)      // taskkill 此时只会报“找不到进程”
      })
      return killer
    },
    exitTimeoutMs: 60,
  }), 3000, 'stopEdge(race)')
  assert.equal(child.killed, false, '它自己退了，不需要补刀')
})

test('runner：进程树怎么都不退 → 有界报错，不无限等', async () => {
  const child = fakeProc({ pid: 4245 })   // kill() 什么都不做：模拟杀不掉
  await assert.rejects(
    () => runner.withTimeout(runner.stopEdge(child, {
      spawnKill: () => { const killer = fakeProc(); setImmediate(() => killer.emit('exit', 0)); return killer },
      exitTimeoutMs: 60,
    }), 3000, 'stopEdge(stuck)'),
    /did not exit/, '清理失败必须报出来，不能吞掉')
  assert.equal(child.killed, true, '补刀尝试过了')
})
