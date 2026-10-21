// U03 测试基础设施：runner 的显式模式选择（--types / --legacy / 默认）与 types 页面入口，
// 以及共享类型元数据纯函数（索引/取值/校验/补丁）。
// 界面行为由真实浏览器页 type-management-browser.jsx 验证 —— 这里不镜像组件逻辑。
import test from 'node:test'
import assert from 'node:assert/strict'
import { existsSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { resolveMode, Cdp } from './run-lcsc-browser.mjs'
import {
  TYPE_ICON_PLACEHOLDER, TYPE_ICON_MAX_BYTES, buildTypeMap, typeMetaOf, typeIconOf, typeColorOf,
  typeLabelOf, iconByteLength, validateIconInput, normalizeColor, validateColorInput,
  buildTypePatch, patchHasChanges, buildAddPayload,
} from '../src/lib/typeMetadata.js'

const here = path.dirname(fileURLToPath(import.meta.url))

test('resolveMode：默认跑专用流程', () => {
  assert.equal(resolveMode([], {}), 'new')
})

test('resolveMode：旧 --legacy 已删除，环境里的 legacy 残留不能改变默认流程', () => {
  assert.throws(() => resolveMode(['--legacy'], {}), /removed/)
  assert.equal(resolveMode([], { KFORGE_LCSC_MODE: 'legacy' }), 'new')
})

test('resolveMode：--types 选元件类型页，环境变量不能抢走显式参数', () => {
  assert.equal(resolveMode(['--types'], {}), 'types')
  assert.equal(resolveMode(['--types'], { KFORGE_LCSC_MODE: 'legacy' }), 'types')
})

test('resolveMode：导入进度定向模式使用类型夹具并拒绝混合模式', () => {
  assert.equal(resolveMode(['--import-progress'], { KFORGE_LCSC_MODE: 'legacy' }), 'types')
  for (const mode of ['--types', '--type-matching']) {
    assert.throws(() => resolveMode(['--import-progress', mode], {}), /不能同时使用/)
  }
})

test('resolveMode：已删除的 --legacy 不参与模式选择', () => {
  assert.throws(() => resolveMode(['--types', '--legacy'], {}), /removed/)
  assert.throws(() => resolveMode(['--legacy', '--types'], {}), /removed/)
})

test('types 模式的页面与入口（runner --types 的目标）都在', () => {
  assert.ok(existsSync(path.join(here, 'type-management-browser.html')), '缺少 type-management-browser.html')
  assert.ok(existsSync(path.join(here, 'type-management-browser.jsx')), '缺少 type-management-browser.jsx')
})

// ---------------- runner：CDP 单次调用的超时覆盖 ----------------
// types 页整批场景要跑几十秒，只能放宽那一次 harness.run 求值；其余命令（含同一
// 模式里的导航）必须保持默认 20s。这里用假 socket 测真实 Cdp 实现，只区分
// 「默认上限 / 显式覆盖 / 覆盖到期」三种可观察行为，不断言常量本身。

class FakeSocket {
  constructor() { this.handlers = new Map(); this.sent = [] }
  addEventListener(type, fn) {
    if (!this.handlers.has(type)) this.handlers.set(type, [])
    this.handlers.get(type).push(fn)
  }
  send(raw) { this.sent.push(JSON.parse(raw)) }
  emit(type, payload) { for (const fn of this.handlers.get(type) || []) fn(payload) }
  reply(message) { this.emit('message', { data: JSON.stringify(message) }) }
  lastSent() { return this.sent.at(-1) }
}

/// 延迟 ms 后回包；在回调里取「此刻最后一条已发出的命令」，避免时序耦合
function replyAfter(socket, delayMs, build) {
  return new Promise((resolve) => setTimeout(() => {
    socket.reply(build(socket.lastSent()))
    resolve()
  }, delayMs))
}

test('runner Cdp：没有覆盖时 send 与 eval 都按实例上限拒绝（不清零超时）', async () => {
  const socket = new FakeSocket()
  const cdp = new Cdp(socket, { timeoutMs: 60 })
  await assert.rejects(cdp.send('Page.enable'), /timed out after 60ms/)
  await assert.rejects(cdp.eval('1'), /Runtime\.evaluate timed out after 60ms/)
})

test('runner Cdp：显式较长的单次覆盖不会被较短的默认上限抢先拒绝', async () => {
  const socket = new FakeSocket()
  const cdp = new Cdp(socket, { timeoutMs: 60 })
  const replied = replyAfter(socket, 150, (msg) => ({ id: msg.id, result: {} }))
  // 覆盖被忽略的话，60ms 就会拒绝；150ms 才回包的结果只能靠覆盖活下来
  await cdp.send('Page.enable', {}, { timeoutMs: 800 })
  await replied
  assert.equal(socket.lastSent().method, 'Page.enable')
})

test('runner Cdp：单次覆盖到期仍然拒绝（覆盖不是把超时取消）', async () => {
  const socket = new FakeSocket()
  const cdp = new Cdp(socket, { timeoutMs: 5000 })
  await assert.rejects(cdp.send('Page.enable', {}, { timeoutMs: 60 }), /timed out after 60ms/)
  await assert.rejects(cdp.eval('window.__ready', { timeoutMs: 60 }), /timed out after 60ms/)
})

test('runner Cdp：eval 把单次覆盖透传给 Runtime.evaluate，回包照常解出', async () => {
  const socket = new FakeSocket()
  const cdp = new Cdp(socket, { timeoutMs: 5000 })
  await assert.rejects(cdp.eval('window.__ready', { timeoutMs: 60 }), /timed out after 60ms/)
  assert.equal(socket.lastSent().method, 'Runtime.evaluate')
  assert.deepEqual(socket.lastSent().params,
    { expression: 'window.__ready', awaitPromise: true, returnByValue: true })

  const okSocket = new FakeSocket()
  const okCdp = new Cdp(okSocket, { timeoutMs: 60 })
  const replied = replyAfter(okSocket, 150, (msg) => ({ id: msg.id, result: { result: { value: 42 } } }))
  assert.equal(await okCdp.eval('1 + 41', { timeoutMs: 800 }), 42)
  await replied
})

// ---------------- 共享类型元数据（纯函数） ----------------

const RELAY = { id: 'custom:relay', name: '继电器', icon: '↯', color: '#123456' }

test('buildTypeMap：按稳定 id 建索引，没有 id 的条目进不来', () => {
  const map = buildTypeMap([RELAY, { name: '没有ID' }, null, 'junk'])
  assert.equal(map.size, 1)
  assert.deepEqual(map.get('custom:relay'), RELAY)
  assert.equal(typeMetaOf(map, 'custom:relay').name, '继电器')
  assert.equal(typeMetaOf(map, 'custom:missing'), null)
  assert.equal(typeMetaOf(null, 'custom:relay'), null)
})

test('图标：配置优先，缺失一律共享占位 ○（不按内置名字猜表）', () => {
  const map = buildTypeMap([RELAY, { id: 'custom:plain', name: 'Plain' }])
  assert.equal(typeIconOf(map, 'custom:relay'), '↯')
  assert.equal(typeIconOf(map, 'custom:plain'), TYPE_ICON_PLACEHOLDER)
  assert.equal(typeIconOf(map, 'custom:unregistered'), TYPE_ICON_PLACEHOLDER)
  assert.equal(typeIconOf(null, 'Resistor'), TYPE_ICON_PLACEHOLDER)
})

test('颜色：缺失 = 空串（沿用默认文字颜色），不是写死一个值', () => {
  const map = buildTypeMap([RELAY, { id: 'custom:plain', name: 'Plain' }])
  assert.equal(typeColorOf(map, 'custom:relay'), '#123456')
  assert.equal(typeColorOf(map, 'custom:plain'), '')
  assert.equal(typeColorOf(map, 'custom:unregistered'), '')
})

test('展示名：登记名优先，认不出的历史 ID 原样显示（不改成 Unknown）', () => {
  const map = buildTypeMap([RELAY])
  assert.equal(typeLabelOf(map, 'custom:relay', '旧展示名'), '继电器')
  assert.equal(typeLabelOf(map, 'custom:gone', 'custom:gone'), 'custom:gone')
  assert.equal(typeLabelOf(map, 'custom:gone', ''), 'custom:gone')
  assert.equal(typeLabelOf(map, '', ''), '')
  // 展示名与服务端不一致时以配置为准（改名后界面立刻跟上）
  assert.equal(typeLabelOf(map, 'custom:relay', '继电器-旧'), '继电器')
})

test('图标长度按 UTF-8 字节算：ASCII/中文/emoji 各不相同', () => {
  assert.equal(iconByteLength('abc'), 3)
  assert.equal(iconByteLength('继'), 3)
  assert.equal(iconByteLength('↯'), 3)
  assert.equal(iconByteLength('😀'), 4)
  assert.equal(iconByteLength(''), 0)
  const atLimit = 'x'.repeat(TYPE_ICON_MAX_BYTES)
  assert.equal(validateIconInput(atLimit), null)
  assert.match(validateIconInput(atLimit + 'x'), /64/)
  assert.match(validateIconInput('继'.repeat(22)), /66/)
  assert.equal(validateIconInput(''), null, '空串 = 清空图标，合法')
})

test('颜色规范化：空串合法、#RRGGBB 大小写皆可、其它一律拒绝', () => {
  assert.equal(normalizeColor(''), '')
  assert.equal(normalizeColor('#AABBCC'), '#aabbcc')
  assert.equal(normalizeColor('#aabbcc'), '#aabbcc')
  for (const bad of ['#fff', '#12345', '#1234567', 'red', '123456', '#GGGGGG', '  ']) {
    assert.equal(normalizeColor(bad), null, `${bad} 不是合法颜色`)
    assert.notEqual(validateColorInput(bad), null)
  }
  assert.equal(validateColorInput('#123456'), null)
})

test('补丁只带真正变化的字段：只改名不会擦掉图标/颜色', () => {
  const entry = { id: 'custom:relay', name: '继电器', icon: '↯', color: '#123456' }
  assert.deepEqual(buildTypePatch(entry, { name: '继电器B', icon: '↯', color: '#123456' }),
    { action: 'update', id: 'custom:relay', name: '继电器B' })
  assert.deepEqual(buildTypePatch(entry, { name: '继电器', icon: '⇄', color: '#123456' }),
    { action: 'update', id: 'custom:relay', icon: '⇄' })
  assert.deepEqual(buildTypePatch(entry, { name: '继电器', icon: '↯', color: '#ABCDEF' }),
    { action: 'update', id: 'custom:relay', color: '#abcdef' })
  // 名称提交前 trim，图标/颜色保持原样
  assert.deepEqual(buildTypePatch(entry, { name: '  继电器B  ', icon: ' ↯ ', color: '#123456' }),
    { action: 'update', id: 'custom:relay', name: '继电器B', icon: ' ↯ ' })
})

test('补丁：历史颜色不是合法 #RRGGBB 时，改名不提交颜色也不改色', () => {
  const legacy = { id: 'Legacy', name: 'Legacy', icon: 'x', color: '#fff' }
  const patch = buildTypePatch(legacy, { name: 'Renamed', icon: 'x', color: '#fff' })
  assert.deepEqual(patch, { action: 'update', id: 'Legacy', name: 'Renamed' })
  assert.equal('color' in patch, false)
  // 用户显式换了合法颜色：照常提交
  assert.deepEqual(buildTypePatch(legacy, { name: 'Legacy', icon: 'x', color: '#AABBCC' }).color, '#aabbcc')
  // 用户显式清除：提交空串
  assert.deepEqual(buildTypePatch(legacy, { name: 'Legacy', icon: 'x', color: '' }).color, '')
  // 显式清空图标同样只提交真正变化的字段
  assert.deepEqual(buildTypePatch(legacy, { name: 'Legacy', icon: '', color: '#fff' }),
    { action: 'update', id: 'Legacy', icon: '' })
})

test('没有任何变化时不构造写入补丁', () => {
  const entry = { id: 'custom:relay', name: '继电器', icon: '↯', color: '#123456' }
  const patch = buildTypePatch(entry, { name: '继电器', icon: '↯', color: '#123456' })
  assert.equal(patchHasChanges(patch), false)
  assert.equal(patchHasChanges(buildTypePatch(entry, { name: 'X', icon: '↯', color: '#123456' })), true)
})

test('新增请求体：name/icon/color 都带，身份留给后端生成', () => {
  assert.deepEqual(buildAddPayload({ name: ' 新类型 ', icon: '⇄', color: '#AABBCC' }),
    { action: 'add', name: '新类型', icon: '⇄', color: '#aabbcc' })
  assert.deepEqual(buildAddPayload({ name: 'P' }),
    { action: 'add', name: 'P', icon: '', color: '' })
  // 历史非法颜色不会被当成合法值提交
  assert.deepEqual(buildAddPayload({ name: 'P', color: '#fff' }).color, '')
})

// ---------------- 历史配置的边界（省略 vs 空串、原型键） ----------------

test('索引：__proto__ / constructor / toString 都只是普通 id，不是原型成员', () => {
  const tricky = [
    { id: '__proto__', name: '原型', icon: 'P' },
    { id: 'constructor', name: '构造', icon: 'C' },
    { id: 'toString', name: '字符串', icon: 'T' },
  ]
  const map = buildTypeMap(tricky)
  assert.equal(map.size, 3)
  assert.equal(typeMetaOf(map, '__proto__').name, '原型')
  assert.equal(typeIconOf(map, 'constructor'), 'C')
  assert.equal(typeColorOf(map, 'toString'), '')
  assert.equal(typeLabelOf(map, 'toString', '旧名'), '字符串')
  // 没有登记的键仍然是「没有」：不能从 Object.prototype 上捡到东西
  assert.equal(typeMetaOf(map, 'valueOf'), null)
  assert.equal(typeIconOf(map, 'hasOwnProperty'), TYPE_ICON_PLACEHOLDER)
})

test('历史图标：只改名不动图标，空串与字段缺失都不是「变了」', () => {
  const longIcon = 'x'.repeat(65)                       // 今天写不进去，但历史上已存在
  const entry = { id: 'custom:legacy-icon', name: '旧', icon: longIcon, color: '#123456' }
  const patch = buildTypePatch(entry, { name: '新', icon: longIcon, color: '#123456' })
  assert.deepEqual(patch, { action: 'update', id: 'custom:legacy-icon', name: '新' })
  assert.equal('icon' in patch, false)
  assert.equal('color' in patch, false)
  // 显式清空才提交空串
  assert.equal(buildTypePatch(entry, { name: '旧', icon: '', color: '#123456' }).icon, '')
  // 本来就没有图标/颜色：草稿也是空 → 没有任何变化，不该凭空写一个字段
  const bare = { id: 'custom:bare', name: '裸' }
  assert.deepEqual(buildTypePatch(bare, { name: '裸', icon: '', color: '' }),
    { action: 'update', id: 'custom:bare' })
  assert.equal(patchHasChanges(buildTypePatch(bare, { name: '裸', icon: '', color: '' })), false)
  // 缺图标的条目在界面上就是共享占位，不是「某个内置类型的图标」
  assert.equal(typeIconOf(buildTypeMap([bare]), 'custom:bare'), TYPE_ICON_PLACEHOLDER)
})
