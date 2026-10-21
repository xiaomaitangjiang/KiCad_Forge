/* eslint-disable react/only-export-components -- 测试页：组件只在本文件内挂载，无需导出 */
// U03 元件类型真实组件测试页：由 run-lcsc-browser.mjs --types 通过 Vite + Edge(CDP)
// 打开（独立页面/独立夹具，不复制 runner 的资源生命周期）。
//
// 页内挂载**真实** App / 真实 TypeModal，只把 window.fetch 换成受控实现（api.js 的
// 请求包装、错误分类、JSON 解析全都真实执行）。夹具提供的是一个受控的假后端：
//   * 类型表存在内存里，写请求按协议改它（不做界面该做的事）；
//   * 失败/挂起由场景用一次性 responder 编排（HTTP 码、ok:false、迟到回包）；
// 夹具不自己算类型映射、不替组件刷新、也不预设未来实现会叫什么 props。
//
// 场景：
//   绿控制 —— 证明页面/夹具/真实组件接线有效
//   红断言 —— U03 合同要求的行为（启动加载类型表、Sidebar/Inspector 用配置图标、
//             条目编辑入口）；失败信息必须能读出「期望什么、实际什么」
//   S01 结构拆分 —— 真实 Sidebar 的既有行为（工作库范围、三项导航、类型筛选、
//             符号库列表、设置/插件/语言/主题）拆分前后必须一致
//   S02 接口收敛 —— Sidebar 只接六组职责对象；区块薄夹具证明「发什么意图」，
//             实际 App 场景证明 App 的 handler 落地后的状态后果

import { act, useState } from 'react'
import { createRoot } from 'react-dom/client'

globalThis.IS_REACT_ACT_ENVIRONMENT = true
import i18n from '../src/i18n'
import '../src/App.css'
import App from '../src/App'
import { ImportingModal, SettingsModal, TypeModal } from '../src/components/Modals'
import { setLoadingCallback } from '../src/api'
import { useComponentTypes } from '../src/hooks/useComponentTypes'
import Sidebar from '../src/components/Sidebar'
import ActivityBar from '../src/workbench/ActivityBar'
import { buildTypeMap } from '../src/lib/typeMetadata'

// ============================================================
// 固定合成数据（全是测试字符串，不读用户库 / 数据库 / 真实配置）
// ============================================================
const RELAY_ID = 'custom:relay'
// 自定义类型 + 非 ○ 的图标：内置表「碰巧一致」导致的假绿被排除
const RELAY = { id: RELAY_ID, name: '继电器', icon: '↯', color: '#123456', builtin: false }
const RESISTOR = { id: 'Resistor', name: 'Resistor', icon: '⊟', color: '#34C759', builtin: true }
const SYMBOL = {
  id: 'sym-type-test', library_id: 'lib-type-test',
  name: 'K_TEST', type_id: RELAY_ID, type: '继电器',
  pins: 2, value: 'K_TEST', mpn: '', footprint: '',
  footprint_reference: '', footprint_status: 'unassigned',
  footprint_reason: '', model_status: 'unassigned',
  locked: false,
}
// 历史 ID：配置里已经不存在这个类型，界面必须原样显示 ID，不能改成 Unknown
const LEGACY_ID = 'custom:legacy-gone'
const LEGACY_SYMBOL = {
  id: 'sym-legacy-test', library_id: 'lib-type-test',
  name: 'K_LEGACY', type_id: LEGACY_ID, type: LEGACY_ID,
  pins: 2, value: 'K_LEGACY', mpn: '', footprint: '',
  footprint_reference: '', footprint_status: 'unassigned',
  footprint_reason: '', model_status: 'unassigned',
  locked: false,
}
const LIBRARY = { id: 'lib-type-test', name: 'Type_Test', symbol_count: 2, component_library_id: '' }
// 历史显示字段：这些值按今天的新写入规则都不合法，但它们早就在用户的配置文件里。
// 加载必须照收；只改名称的补丁既不能被它们挡住，也不能顺手把它们改掉。
const LONG_ICON = 'x'.repeat(65)   // 超过 64 字节上限的历史图标
const LEGACY_COLOR = { id: 'custom:legacy-color', name: '历史色', icon: 'L', color: 'red', builtin: false }
const LEGACY_LONG_ICON = {
  id: 'custom:legacy-icon', name: '历史图标', icon: LONG_ICON, color: '#123456', builtin: false,
}
const LEGACY_BOTH = { id: 'custom:legacy-both', name: '历史混合', icon: LONG_ICON, color: 'red', builtin: false }
// U02 之后封装/3D 只认显式 status：夹具给合法状态，不靠旧布尔兜底
const BINDING = {
  footprint_status: 'unassigned', footprint_reference: '', footprint_reason: '',
  model_status: 'unassigned', footprint_id: '', model_id: '',
}
const PKG_TYPES = [{ id: 'SMD_0603', name: 'SMD_0603', category: 'SMD Chip', builtin: true }]

// ============================================================
// 受控假后端 + 按 pathname 分发的 fetch
// ============================================================
const realFetch = window.fetch.bind(window)

const clone = (value) => JSON.parse(JSON.stringify(value))
function makeResponse(payload, status = 200) {
  const body = clone(payload)
  return { ok: status >= 200 && status < 300, status, json: async () => clone(body) }
}

const backend = {
  types: [],        // GET /api/component-types 的真实来源（写请求会改它）
  pkgTypes: [],
  addSeq: 0,
}

/// 写请求的默认实现：按协议改内存表并如实回 ok/id/error。
/// 界面该做的事（图标取哪份配置、草稿怎么保留）这里一概不做。
function applyComponentWrite(call) {
  const body = call.body || {}
  const action = body.action
  if (action === 'add') {
    if (!body.name) return { status: 200, payload: { ok: false, error: 'Type name must not be empty' } }
    if (backend.types.some((t) => t.name === body.name))
      return { status: 200, payload: { ok: false, error: `Type name already exists: ${body.name}` } }
    backend.addSeq += 1
    const id = `custom:add-${backend.addSeq}`
    backend.types.push({
      id, name: body.name,
      icon: body.icon || '', color: body.color || '', builtin: false,
    })
    return { status: 200, payload: { ok: true, id } }
  }
  const target = backend.types.find((t) => t.id === body.id)
  if (!target) return { status: 200, payload: { ok: false, error: `Type not found: ${body.id}` } }
  if (action === 'update') {
    if (body.name !== undefined) {
      if (!body.name) return { status: 200, payload: { ok: false, error: 'Type name must not be empty' } }
      if (backend.types.some((t) => t.name === body.name && t.id !== body.id))
        return { status: 200, payload: { ok: false, error: `Type name already exists: ${body.name}` } }
      target.name = body.name
    }
    if (body.icon !== undefined) target.icon = body.icon
    if (body.color !== undefined) target.color = body.color
    return { status: 200, payload: { ok: true, id: body.id } }
  }
  if (action === 'remove') {
    if (target.builtin) return { status: 200, payload: { ok: false, error: 'Built-in types cannot be deleted' } }
    if (target.references) {
      return { status: 200, payload: { ok: false, error: 'Type is still referenced; reclassify first', references: target.references } }
    }
    backend.types.splice(backend.types.indexOf(target), 1)
    return { status: 200, payload: { ok: true, id: body.id } }
  }
  return { status: 200, payload: { ok: false, error: 'Invalid' } }
}

function applyPackageWrite(call) {
  const body = call.body || {}
  if (body.action === 'add') {
    if (!body.name) return { status: 200, payload: { ok: false, error: 'Type name must not be empty' } }
    const id = `custom:pkg-${backend.pkgTypes.length + 1}`
    backend.pkgTypes.push({ id, name: body.name, category: body.category || 'Other', builtin: false })
    return { status: 200, payload: { ok: true, id } }
  }
  const target = backend.pkgTypes.find((t) => t.id === body.id)
  if (!target) return { status: 200, payload: { ok: false, error: `Type not found: ${body.id}` } }
  if (body.action === 'remove') {
    if (target.builtin) return { status: 200, payload: { ok: false, error: 'Built-in types cannot be deleted' } }
    backend.pkgTypes.splice(backend.pkgTypes.indexOf(target), 1)
    return { status: 200, payload: { ok: true, id: body.id } }
  }
  return { status: 200, payload: { ok: false, error: 'Invalid' } }
}

const net = {
  calls: [],        // {path, method, body, search}
  unknown: [],      // 没 mock 的接口：显式拒绝并让场景失败（暴露夹具缺口，不悄悄回空）
  handlers: new Map(),   // path → [{method, respond}] 一次性 responder
  overrides: new Map(),  // path → respond：整场都回这份数据（会有多次刷新的接口用）
  pending: [],      // 受控但尚未兑现的响应（收尾必须全部放行）
}

const isThenable = (value) => value && typeof value.then === 'function'

/// 下一次该路径的请求交给 fn：返回 {status,payload} 或 Promise<Response>
function respondOnce(path, fn, method = null) {
  const queue = net.handlers.get(path) || []
  queue.push({ respond: fn, method })
  net.handlers.set(path, queue)
}

/// 整场固定应答：mount 期间该接口可能被刷新多次（例如导入完成后的刷新），
/// 用一次性 responder 会被第二次请求绕过去，读回默认表
function stubPath(path, fn) { net.overrides.set(path, fn) }

/// 挂起下一次请求（迟到/在飞场景）：返回手动兑现的句柄。
/// entry.call 是挂起时的真实请求（场景可以让假后端先按协议处理，再兑现回包）。
function holdNext(path, method = null) {
  let release = null
  const entry = { done: false, call: null }
  const promise = new Promise((resolve) => { release = resolve })
  entry.resolve = (payload, status = 200) => { entry.done = true; release(makeResponse(payload, status)) }
  /// 先交给假后端处理（写入照常生效），再用它的回包兑现
  entry.resolveThrough = (apply) => {
    const routed = apply(entry.call)
    entry.resolve(routed.payload, routed.status)
  }
  net.pending.push(entry)
  respondOnce(path, (call) => { entry.call = call; return promise }, method)
  return entry
}

function payloadFor(method, path, call) {
  if (method === 'POST' && path === '/api/component-types') return applyComponentWrite(call)
  if (method === 'POST' && path === '/api/package-types') return applyPackageWrite(call)
  if (path.endsWith('/binding')) return { status: 200, payload: BINDING }
  const table = {
    '/api/status': { symbols: 2, footprints: 0, models_3d: 0 },
    '/api/symbols': [SYMBOL, LEGACY_SYMBOL],
    '/api/libraries': [LIBRARY],
    '/api/component-libraries': [],           // 不切具体工作库，避免按归属过滤掉符号
    '/api/config': { active_component_library: '' },
    '/api/plugins': [],
    '/api/rules': [],
    '/api/check': [],
    '/api/automatch': [],
    '/api/footprints/search': [],
    '/api/models/search': [],
    '/api/component-types': backend.types,
    '/api/package-types': backend.pkgTypes,
    '/api/import-status': { importing: false, symbols: 1, footprints: 0 },
  }
  if (!(path in table)) return null
  return { status: 200, payload: table[path] }
}

function installFetch() {
  window.fetch = (input, init = {}) => {
    const raw = typeof input === 'string' ? input : String(input?.url || input)
    const url = new URL(raw, location.origin)
    const path = url.pathname
    const method = (init.method || 'GET').toUpperCase()
    let body = null
    try { body = init.body ? JSON.parse(init.body) : null } catch { body = init.body }
    // search 一并记录：查询状态（关键词 / 目标库）只体现在 query string 上
    const call = { path, method, body, search: url.search }
    net.calls.push(call)

    const queue = net.handlers.get(path)
    if (queue && queue.length) {
      const index = queue.findIndex((h) => !h.method || h.method === method)
      if (index >= 0) {
        const [handler] = queue.splice(index, 1)
        let out
        try { out = handler.respond(call) } catch (cause) { return Promise.reject(cause) }
        if (isThenable(out)) return out          // 已经是 Response（含被挂起的迟到回包）
        return new Promise((resolve) => setTimeout(() => resolve(makeResponse(out.payload, out.status)), 0))
      }
    }

    const stub = net.overrides.get(path)
    if (stub) {
      const out = stub(call)
      return new Promise((resolve) => setTimeout(() => resolve(makeResponse(out.payload, out.status)), 0))
    }

    const routed = payloadFor(method, path, call)
    if (!routed) {
      net.unknown.push(`${method} ${path}`)
      return Promise.reject(new Error(`测试夹具缺少接口：${method} ${path}`))
    }
    return new Promise((resolve) => setTimeout(() => resolve(makeResponse(routed.payload, routed.status)), 0))
  }
}

/// 放行**全部**挂起请求（含错误实现多发出来的重复请求），不能只放行第一个
function releaseGates() {
  for (const entry of net.pending.splice(0)) {
    if (!entry.done) entry.resolve({ ok: true })
  }
}

// ============================================================
// DOM / 挂载
// ============================================================
const $ = (sel) => document.querySelector(sel)
const $$ = (sel) => [...document.querySelectorAll(sel)]
function countCalls(path) { return net.calls.filter((c) => c.path === path).length }
function countPosts(path) { return net.calls.filter((c) => c.path === path && c.method === 'POST').length }
function lastPost(path) { return net.calls.filter((c) => c.path === path && c.method === 'POST').at(-1) }
function assert(cond, message) { if (!cond) throw new Error(message) }
function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`)
  }
}
function click(node) {
  if (!node) throw new Error('click: 节点不存在')
  act(() => { node.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true })) })
}

/// React 受控输入：走原生 setter + input 事件（否则 value tracker 会把直接赋值吞掉）
function setInput(node, value) {
  if (!node) throw new Error('setInput: 节点不存在')
  const proto = node instanceof HTMLTextAreaElement ? HTMLTextAreaElement : HTMLInputElement
  act(() => {
    Object.getOwnPropertyDescriptor(proto.prototype, 'value').set.call(node, value)
    node.dispatchEvent(new Event('input', { bubbles: true }))
  })
}
/// select 在 React 里走 change 事件（input 事件不会触发 onChange）
function setSelect(node, value) {
  if (!node) throw new Error('setSelect: 节点不存在')
  act(() => {
    Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set.call(node, value)
    node.dispatchEvent(new Event('change', { bubbles: true }))
  })
}
async function settle(times = 3) {
  for (let i = 0; i < times; i += 1) {
    await act(async () => { await new Promise((r) => setTimeout(r, 0)) })
  }
}

/// toast 自己会在 ~2.8s 后清空（真实定时器）。在场景结束前把这段时间留在 act 里，
/// 让那次状态更新落在受控窗口内 —— 否则它会变成测试噪声，掩盖真正的报错。
async function flushToast() {
  await act(async () => { await new Promise((r) => setTimeout(r, 3000)) })
}

let root = null
function unmount() {
  let failure = null
  if (root) {
    const current = root
    root = null            // 渲染期就崩掉的树：先复位，失败只记在本条场景上
    try { act(() => current.unmount()) } catch (cause) { failure = cause }
  }
  const host = $('#root')
  if (host) host.innerHTML = ''
  if (failure) throw failure
}
function mount(element) {
  unmount()
  root = createRoot($('#root'))
  act(() => { root.render(element) })
}

/// 有界轮询：只等真实的网络/定时器结果落地，不替组件改状态
async function waitFor(predicate, ms, step = 25) {
  const deadline = Date.now() + ms
  for (;;) {
    await act(async () => { await new Promise((r) => setTimeout(r, step)) })
    if (predicate()) return true
    if (Date.now() > deadline) return false
  }
}
async function waitUntil(predicate, ms, label) {
  if (!await waitFor(predicate, ms)) throw new Error(`等待超时（${ms}ms）：${label}`)
}

function symbolCard(name) {
  return $$('.symbol-card').find((card) => card.querySelector('.symbol-name')?.textContent === name)
}

/// 浏览器对某个颜色写法的规范表示：不同引擎/主题下 rgb() 的写法不同，
/// 所以不写死字符串，而是让浏览器自己算一遍作对照。
function canonicalColor(cssColor) {
  const probe = document.createElement('span')
  probe.style.cssText = `position:absolute;visibility:hidden;color:${cssColor}`
  document.body.appendChild(probe)
  const computed = getComputedStyle(probe).color
  probe.remove()
  return computed
}

/// 类型条目里真正的图标节点（颜色写在它自己的行内样式上，不在父行上）
function typeIconNode(id) { return typeRow(id)?.querySelector('.type-icon') }

/// 挂真实 App，并等 ImportingModal 的 2s 轮询拿到 importing:false、遮罩撤掉
async function mountApp() {
  mount(<App />)
  await waitUntil(() => countCalls('/api/import-status') > 0 && !$('.modal-overlay'), 6000,
    'ImportingModal 在首次 2s 轮询后撤掉遮罩（GET /api/import-status）')
  await settle(2)
}

// ---- 真实 useComponentTypes 的薄外壳 ----
// 只挂真实 hook 并把当前 state 与 mutate/reload 暴露出来：generation、stale、
// 写保护全是被测实现自己的事，外壳不复制任何规则，也不人工造状态。
let probeState = null
function HookProbe() {
  probeState = useComponentTypes()
  return <div data-testid="hook-probe" />
}
async function mountProbe({ settleTimes = 2 } = {}) {
  mount(<HookProbe />)
  await waitUntil(() => probeState && countCalls('/api/component-types') > 0, 4000,
    '真实类型 hook 发出首次读取')
  await settle(settleTimes)
}
/// 启动一次真实 hook 调用：await 之前的同步状态更新（占位、loading）必须在 act 里，
/// 但 act 只包住同步那一段 —— 长期占着 act 会让后面的 settle/等待变成 act 重叠。
function probeStart(fn) {
  let out
  act(() => { out = fn() })
  return out
}
/// 调用真实 hook 并等它自己收尾：回包（POST/GET）由 settle 的 act 窗口消化，
/// 不能在没有 act 窗口的裸 await 里落状态。
async function probeRun(fn, times = 3) {
  const out = probeStart(fn)
  await settle(times)
  return out
}
/// 放行挂起的回包后取回调用结果：先给 settle 窗口让回包落地（否则状态更新
/// 会落在 act 之外），再取已经结算的 promise。
async function probeFinish(promise, times = 2) {
  await settle(times)
  const out = await promise
  await settle(1)
  return out
}
function probeTypeName(id) {
  return (probeState?.types || []).find((t) => t.id === id)?.name
}

// ============================================================
// 类型管理的真实入口：Sidebar 设置 → 设置弹窗「工具」→ 元件类型 / 封装类型
// ============================================================
function modals() { return $$('.modal') }
function lastModal() { return modals().at(-1) }
function inLast(sel) { return lastModal()?.querySelector(sel) }
function byTestId(id) { return $$(`[data-testid="${id}"]`).at(-1) }

async function openToolsModal(label) {
  const settings = activityBarItem('设置')
  assert(settings, '夹具无效：活动栏里没有「设置」入口')
  click(settings)
  await waitUntil(() => $('.settings-page'), 4000, '设置页打开')
  const utilsTab = $$('.settings-page div').find((el) =>
    (el.textContent || '').trim().startsWith('🔧') && !el.querySelector('div'))
  assert(utilsTab, '夹具无效：设置页里没有「工具」页签')
  click(utilsTab)
  await settle(1)
  const entry = $$('.settings-page button').find((b) => (b.textContent || '').trim() === label)
  assert(entry, `夹具无效：工具页里没有「${label}」入口`)
  click(entry)
  if (label === '元件类型') {
    await waitUntil(() => $('.types-page'), 4000, '「元件类型」中心页打开')
  } else {
    await waitUntil(() => (lastModal()?.querySelector('h3')?.textContent || '') === label, 4000,
      `「${label}」弹窗打开`)
  }
  await settle(1)
}

function openTypeManager() { return openToolsModal('元件类型') }
function closeTypeManager() {
  const close = $('[data-center-tab-close="kf:types"]')
  assert(close, '夹具无效：类型中心页没有关闭入口')
  click(close)
}
function typeRow(id) { return $(`[data-testid="type-row-${id}"]`) }
function typeRowText(id) { return typeRow(id)?.querySelector('.rule-name')?.textContent }
function rowEditButton(id) {
  const row = typeRow(id)
  return row && [...row.querySelectorAll('button')].find((b) => /编辑|edit/i.test(
    `${b.getAttribute('aria-label') || ''} ${b.getAttribute('title') || ''} ${b.textContent || ''}`))
}
function rowDeleteButton(id) {
  const row = typeRow(id)
  return row && [...row.querySelectorAll('button')].find((b) => (b.textContent || '').includes('×'))
}
function openEditor(id) {
  const row = typeRow(id)
  assert(row, `夹具无效：类型表里没有 ${id} 这一行`)
  const edit = rowEditButton(id)
  assert(edit, `夹具无效：${id} 这一行没有编辑入口`)
  click(edit)
}
/// 真实的用户关闭路径之一（Modal 的 Escape 监听挂在 document 上）
function pressEscape() {
  act(() => {
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
  })
}
async function saveEditor() {
  click(byTestId('type-save'))
  await settle(3)
}
function expandSidebarTypes() {
  const title = $$('.sidebar .section-title').find((el) => (el.textContent || '').includes('元件类型'))
  assert(title, '夹具无效：Sidebar 没有「元件类型」分组标题')
  if (!$$('.sidebar .type-dot').length) click(title)
  return $$('.sidebar .nav-item').find((el) =>
    el.querySelector('.type-dot') && (el.textContent || '').includes('继电器'))
}

// ============================================================
// 场景
// ============================================================
// 绿控制：真实 App 能挂载、卡片能点、Inspector 能显示选中符号（不检查配置图标）
async function controlAppMounts() {
  await mountApp()
  const card = symbolCard('K_TEST')
  assert(card, '夹具无效：symbols 回包后没有 K_TEST 的 .symbol-card')
  click(card)
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, 'Inspector 显示选中的 K_TEST')
  await waitUntil(() => countCalls(`/api/symbols/${SYMBOL.id}/binding`) > 0, 4000, 'Inspector 拉取符号绑定')
  assertEq($('.inspector h4').textContent, 'K_TEST', '绿控制：Inspector 标题是选中的符号名')
}

// 搜索浮层走统一浮层栈：Escape 只关它，并把焦点还给打开它的字段。
async function inspectorPopoverClosesOnEscape() {
  await mountApp()
  click(symbolCard('K_TEST'))
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, 'Inspector 显示选中的 K_TEST')
  await waitUntil(() => countCalls(`/api/symbols/${SYMBOL.id}/binding`) > 0, 4000, 'Inspector 拉取符号绑定')
  const field = $('[data-testid="fp-state-unassigned"]')
  assert(field, '夹具无效：未分配封装应渲染 p-state 字段')
  click(field)
  await waitUntil(() => $('.search-popover'), 4000, '点封装字段打开搜索浮层')
  assert(document.activeElement?.classList.contains('search-popover-input'),
    `搜索浮层打开后焦点在输入框（实际：${document.activeElement?.tagName}.${document.activeElement?.className}）`)
  pressEscape()
  await waitUntil(() => !$('.search-popover'), 4000, 'Escape 关闭搜索浮层')
  assertEq(document.activeElement, field, '关闭后焦点回到打开浮层的字段')
}

// 红：Sidebar 类型行应显示类型配置里的 icon（当前走内置 TYPE_ICONS[id] || '○'）
async function sidebarUsesConfiguredIcon() {
  await mountApp()
  const row = expandSidebarTypes()
  assert(row, `夹具无效：展开类型分组后没有 ${RELAY_ID}（继电器）行`)
  const dot = row.querySelector('.type-dot')
  assertEq(dot.textContent, '↯', 'Sidebar .type-dot 应取类型配置的 icon（继电器 → ↯）')
}

// 红：Inspector 头部图标应显示同一份配置的 icon（当前按展示名 selected.type 查内置表）
async function inspectorUsesConfiguredIcon() {
  await mountApp()
  const card = symbolCard('K_TEST')
  assert(card, '夹具无效：找不到 K_TEST 的 .symbol-card')
  click(card)
  await waitUntil(() => countCalls(`/api/symbols/${SYMBOL.id}/binding`) > 0, 4000, 'Inspector 拉取符号绑定')
  const icon = $('.inspector-icon')
  assert(icon, '夹具无效：选中符号后没有 .inspector-icon')
  assertEq(icon.textContent, '↯', 'Inspector .inspector-icon 应取类型配置的 icon（继电器 → ↯）')
}

// 红：App 启动就该加载元件类型表（当前只在打开类型管理弹窗时才请求）
async function appLoadsComponentTypesOnStart() {
  await mountApp()
  const loaded = await waitFor(() => net.calls.some((c) => c.path === '/api/component-types'), 2500)
  assert(loaded, 'App 启动未加载元件类型表：等 2.5s 仍没有 GET /api/component-types')
}

// 真实 TypeModal 的宿主：onClose 与 App 一致 —— 更新 show 状态、真正卸载弹窗
let modalClosed = false
function TypeModalHost({ items, onAdd, onRemove, onSave }) {
  const [show, setShow] = useState(true)
  if (!show) return <div id="type-modal-closed" />
  return (
    <TypeModal title="元件类型" items={items} showIcon
      onClose={() => { modalClosed = true; setShow(false) }}
      onAdd={onAdd} onRemove={onRemove} onSave={onSave} />
  )
}

function findTypeRow(name) {
  return $$('.rule-item').find((el) => el.querySelector('.rule-name')?.textContent === name)
}

// 绿控制：真实 TypeModal 已经能显示条目 icon，且 onClose 后确实消失
async function typeModalShowsConfiguredIcon() {
  modalClosed = false
  const seen = { add: [], remove: [] }
  mount(<TypeModalHost items={[RELAY]} onAdd={(payload) => seen.add.push(payload)}
    onRemove={(item) => seen.remove.push(item)} />)
  const row = findTypeRow('继电器')
  assert(row, `夹具无效：TypeModal 没有渲染 ${RELAY_ID} 条目`)
  assert((row.textContent || '').includes('↯'),
    `绿控制：TypeModal 条目应显示 items 里的 icon ↯，实际 ${JSON.stringify(row.textContent)}`)
  click($('.modal-close'))
  await settle(1)
  assert(modalClosed, '绿控制：点击关闭按钮应触发 onClose')
  assert(!$('.modal-overlay'), '绿控制：onClose 之后弹窗应真正消失')
  assertEq(seen.add.length, 0, '绿控制：没有输入触发 onAdd 时不得调用 onAdd')
}

// 红：已有类型条目要有可访问的编辑入口（当前条目唯一的按钮是 × 删除）
async function typeModalHasEditableEntry() {
  modalClosed = false
  mount(<TypeModalHost items={[RELAY]} onAdd={() => {}} onRemove={() => {}} />)
  const row = findTypeRow('继电器')
  assert(row, `夹具无效：TypeModal 没有渲染 ${RELAY_ID} 条目`)
  const buttons = [...row.querySelectorAll('button')]
  const edit = buttons.find((b) => !b.disabled && /编辑|edit/i.test(
    `${b.getAttribute('aria-label') || ''} ${b.getAttribute('title') || ''} ${b.textContent || ''}`))
  assert(edit, '元件类型条目缺少可访问的编辑入口：现有条目的按钮只有'
    + ` [${buttons.map((b) => (b.textContent || '').trim()).join(', ')}]，没有「编辑/Edit」按钮或其 aria-label`)
}

// U03-1 新增：编辑器提交 name/icon/color，使用后端返回的 ID
async function addTypeCarriesFields() {
  await mountApp()
  await openTypeManager()
  click(byTestId('type-add-open'))
  await settle(1)
  setInput(inLast('#type-editor-icon'), '⇄')
  setInput(inLast('#type-editor-color'), '#abcdef')
  setInput(byTestId('type-name'), '新建类型A')
  await saveEditor()
  const post = lastPost('/api/component-types')
  assert(post, '新增类型必须发出 POST /api/component-types')
  assertEq(post.body.action, 'add', '新增请求的 action')
  assertEq(post.body.name, '新建类型A', '新增请求要带 name')
  assertEq(post.body.icon, '⇄', '新增请求要带 icon')
  assertEq(post.body.color, '#abcdef', '新增请求要带 color')
  const added = backend.types.find((t) => t.name === '新建类型A')
  assert(added && added.id.startsWith('custom:'), '新类型的 ID 必须由后端生成（custom: 前缀）')
  await waitUntil(() => typeRow(added.id), 4000, '刷新后新类型出现在列表里')
  assertEq(typeRow(added.id).querySelector('.rule-name').textContent, '新建类型A', '列表显示新类型名')
  assert((typeRow(added.id).textContent || '').includes('⇄'), '列表显示新类型的图标')
  await flushToast()   // 新增成功的 toast 定时器在 act 里走完
}

// U03-2 改名：提交稳定 ID，只带变化字段；Sidebar/Inspector 立刻用新配置
async function renameKeepsStableId() {
  await mountApp()
  const card = symbolCard('K_TEST')
  click(card)
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, 'Inspector 显示选中的 K_TEST')
  await openTypeManager()
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器B')
  await saveEditor()

  const post = lastPost('/api/component-types')
  assertEq(post.body.action, 'update', '编辑请求的 action')
  assertEq(post.body.id, RELAY_ID, '编辑按稳定 id 提交，不是按展示名')
  assertEq(post.body.name, '继电器B', '改名只提交 name')
  assert(!('icon' in post.body), `只改名称不得带 icon 字段：${JSON.stringify(post.body)}`)
  assert(!('color' in post.body), `只改名称不得带 color 字段：${JSON.stringify(post.body)}`)

  await waitUntil(() => typeRowText(RELAY_ID) === '继电器B', 4000, '类型列表刷新出新的显示名')
  // Inspector 没有重新点击/刷新 symbols，也必须用最新配置
  await waitUntil(() => $('.inspector-type')?.textContent === '继电器B', 4000,
    'Inspector 类型文字随配置实时更新')
  assertEq($('.inspector-icon').textContent, '↯', '改名不影响图标（仍然取同一份配置）')
  // 符号列表的类型文字也来自同一份配置：symbols 回包里还是旧展示名，界面必须显示新名字
  assertEq(symbolCard('K_TEST').querySelector('.type-tag').textContent, '继电器B',
    'SymbolTable 类型文字与配置同源，不靠重新获取 symbols')

  // 筛选按 ID：改名后点类型行仍然命中同一个符号
  closeTypeManager()
  await settle(1)
  const row = expandSidebarTypes()
  assert(row, 'Sidebar 类型行仍在（按 ID 归组）')
  click(row)
  await settle(2)
  assert($('.nav-item.active')?.querySelector('.type-dot'), 'Sidebar 类型行进入筛选态')
  assert(symbolCard('K_TEST'), '改名后按类型筛选仍然命中同一个符号（筛选用的是 ID）')
  assert((row.textContent || '').includes('继电器B'), 'Sidebar 类型行显示刷新后的配置名')
}

// U03-3 颜色大小写 / 多字节图标 / 清空图标与颜色
async function colorAndIconEdits() {
  await mountApp()
  await openTypeManager()
  openEditor(RELAY_ID)
  await settle(1)
  setInput(inLast('#type-editor-icon'), '继电器')
  await saveEditor()
  assertEq(lastPost('/api/component-types').body.icon, '继电器', '多字节中文图标按原文提交')
  await waitUntil(() => typeRow(RELAY_ID)?.querySelector('.type-icon')?.textContent === '继电器',
    4000, '列表按配置显示中文图标')

  openEditor(RELAY_ID)
  await settle(1)
  setInput(inLast('#type-editor-color'), '#ABCDEF')
  await saveEditor()
  const color = lastPost('/api/component-types').body.color
  assert(/^#[0-9a-fA-F]{6}$/.test(color), `颜色提交必须是 #RRGGBB，实际 ${JSON.stringify(color)}`)
  assertEq(color.toLowerCase(), '#abcdef', '大写颜色按同一颜色提交')
  await waitUntil(() => typeIconNode(RELAY_ID)?.style.color, 4000, '列表用配置颜色渲染图标')
  assertEq(canonicalColor(typeIconNode(RELAY_ID).style.color), canonicalColor('#abcdef'),
    `图标节点要按配置颜色渲染，实际内联 ${JSON.stringify(typeIconNode(RELAY_ID).style.color)}`
    + `/ computed ${JSON.stringify(getComputedStyle(typeIconNode(RELAY_ID)).color)}`)

  // 清空图标与颜色：显式空串，落到列表就是通用占位 + 默认文字颜色
  openEditor(RELAY_ID)
  await settle(1)
  click(byTestId('type-clear-color'))
  setInput(inLast('#type-editor-icon'), '')
  await saveEditor()
  const cleared = lastPost('/api/component-types').body
  assertEq(cleared.icon, '', '清空图标要提交显式空串')
  assertEq(cleared.color, '', '清除颜色要提交显式空串')
  await waitUntil(() => (typeRow(RELAY_ID).textContent || '').includes('○'), 4000,
    '没有图标时显示共享通用占位 ○')
  await waitUntil(() => typeIconNode(RELAY_ID) && !typeIconNode(RELAY_ID).style.color, 4000,
    '清除颜色后图标节点不再带行内颜色')
  // 继承默认文字颜色：与同一行里没有任何颜色样式的名称节点一致
  assertEq(getComputedStyle(typeIconNode(RELAY_ID)).color,
    getComputedStyle(typeRow(RELAY_ID).querySelector('.rule-name')).color,
    '没有颜色时沿用默认文字颜色（继承，不是写死一个值）')
}

// U03-4 客户端校验：空名称 / 超长图标在提交前就被挡住，且保留草稿
async function editorValidation() {
  await mountApp()
  await openTypeManager()
  openEditor(RELAY_ID)
  await settle(1)
  const nameInput = byTestId('type-name')
  setInput(nameInput, '   ')
  await saveEditor()
  assert(byTestId('type-editor-error'), '空名称必须显示错误')
  assertEq(countPosts('/api/component-types'), 0, '空名称不发请求')
  assert(lastModal(), '校验失败后编辑器保持打开')
  assertEq(nameInput.value, '   ', '校验失败不能清空输入')

  setInput(nameInput, '继电器')
  setInput(inLast('#type-editor-icon'), 'x'.repeat(65))
  await saveEditor()
  const error = byTestId('type-editor-error')?.textContent || ''
  assert(error.includes('64'), `超长图标要给出可读原因（含上限 64），实际 ${JSON.stringify(error)}`)
  assertEq(countPosts('/api/component-types'), 0, '超长图标不发请求')
  assertEq(inLast('#type-editor-icon').value.length, 65, '校验失败保留输入内容')

  // 重名：前端不拦，交服务端判定，失败原因如实展示
  setInput(nameInput, 'Resistor')
  setInput(inLast('#type-editor-icon'), '↯')
  await saveEditor()
  assertEq(countPosts('/api/component-types'), 1, '重名要真的提交给服务端')
  const dupError = byTestId('type-editor-error')?.textContent || ''
  assert(/already exists/i.test(dupError), `重名要展示服务端原因，实际 ${JSON.stringify(dupError)}`)
  assertEq(typeRowText(RELAY_ID), '继电器', '重名失败后列表保持旧值')
  assertEq(byTestId('type-name').value, 'Resistor', '重名失败保留草稿')

  // 新增是另一套策略：全部字段都要过校验（这里没有任何「没动所以跳过」的余地）
  click(byTestId('type-editor-cancel'))
  await settle(1)
  click(byTestId('type-add-open'))
  await settle(1)
  setInput(byTestId('type-name'), '新增超长图标')
  setInput(inLast('#type-editor-icon'), 'x'.repeat(65))
  await saveEditor()
  const addError = byTestId('type-editor-error')?.textContent || ''
  assert(addError.includes('64'), `新增的超长图标也要给出可读原因（含 64），实际 ${JSON.stringify(addError)}`)
  assertEq(countPosts('/api/component-types'), 1, '新增校验不过不发请求')
  click(byTestId('type-editor-cancel'))
  await settle(1)
}

// U03-5 保存失败：保留草稿与旧显示，展示服务端原因
async function saveFailureKeepsDraft() {
  await mountApp()
  await openTypeManager()
  respondOnce('/api/component-types', () => ({ status: 200, payload: { ok: false, error: 'SERVER-REJECT' } }), 'POST')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '不该生效')
  await saveEditor()
  assertEq(countPosts('/api/component-types'), 1, '只发一次保存请求')
  const error = byTestId('type-editor-error')?.textContent || ''
  assert(error.includes('SERVER-REJECT'), `失败原因要如实展示，实际 ${JSON.stringify(error)}`)
  assertEq(byTestId('type-name').value, '不该生效', '失败保留草稿')
  assertEq(typeRowText(RELAY_ID), '继电器', '失败后类型列表保持旧值')
  assertEq(typeRow(RELAY_ID).querySelector('.type-icon').textContent, '↯', '失败后旧图标不变')
}

// U03-6 重复保存：只发一次请求（占位在 await 之前生效）
async function doubleSubmitSingleRequest() {
  await mountApp()
  await openTypeManager()
  const gate = holdNext('/api/component-types', 'POST')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器C')
  click(byTestId('type-save'))
  await settle(1)
  assertEq(countPosts('/api/component-types'), 1, '第一次点击已发出写入请求')
  assert(byTestId('type-save')?.disabled === true, '写入在飞期间保存键必须冻结')
  click(byTestId('type-save'))     // 冻结状态下再点一次：不能有第二次写入
  await settle(1)
  assertEq(countPosts('/api/component-types'), 1, '双击保存只允许一次写入请求')
  gate.resolveThrough(applyComponentWrite)
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器C', 4000, '保存成功后列表刷新')
  assertEq(countPosts('/api/component-types'), 1, '整个保存过程只有一次写入请求')
}

// U03-7 已保存但刷新失败：显示「已保存」+ 只重试读取，绝不再写一次
async function savedButRefreshFailed() {
  await mountApp()
  await openTypeManager()
  respondOnce('/api/component-types', () => ({ status: 500, payload: { error: 'REFRESH-DOWN' } }), 'GET')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器D')
  await saveEditor()
  const banner = byTestId('types-stale')
  assert(banner, '保存成功但刷新失败必须给出「已保存」提示')
  const text = banner.textContent || ''
  assert(text.includes('已保存'), `提示必须说明已经保存，实际 ${JSON.stringify(text)}`)
  assert(text.includes('REFRESH-DOWN'), `提示要带上真实刷新失败原因，实际 ${JSON.stringify(text)}`)
  assertEq(countPosts('/api/component-types'), 1, '刷新失败不能变成再来一次写入')
  assertEq(typeRowText(RELAY_ID), '继电器', '刷新失败期间显示的是上一次有效快照')

  click(byTestId('types-retry-load'))
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器D', 4000, '重试读取后显示新快照')
  assert(!byTestId('types-stale'), '读取成功后提示消失')
  assertEq(countPosts('/api/component-types'), 1, '重试只读，不再发写入请求')
}

// U03-8 读取失败：保留旧快照、给原因、可重试，不把失败当空表
async function loadFailureKeepsSnapshot() {
  await mountApp()
  await openTypeManager()
  assert(typeRow(RELAY_ID), '夹具无效：首次读取后应有继电器条目')
  closeTypeManager()
  await settle(1)
  respondOnce('/api/component-types', () => ({ status: 500, payload: { error: 'READ-DOWN' } }), 'GET')
  await openTypeManager()   // 打开类型管理会重新读取一次
  const banner = byTestId('types-load-failed')
  assert(banner, '读取失败必须提示，不能当成空类型表')
  assert((banner.textContent || '').includes('READ-DOWN'),
    `读取失败要展示真实原因，实际 ${JSON.stringify(banner.textContent)}`)
  assert(typeRow(RELAY_ID), '读取失败时保留上一次有效快照')
  click(byTestId('types-retry-load'))
  await waitUntil(() => !byTestId('types-load-failed'), 4000, '重试成功后提示消失')
  assert(typeRow(RELAY_ID), '重试后条目仍在')
}

// U03-9 迟到的旧 GET 不能覆盖保存后的新快照
// 顺序必须是：慢的旧读取先开始 → 保存与新读取先完成 → 旧回包最后到，且被丢弃
async function staleReadCannotOverwrite() {
  await mountApp()
  const slow = holdNext('/api/component-types', 'GET')   // 打开类型管理触发的读取被挂起
  await openTypeManager()
  await settle(1)
  assert(!slow.done, '夹具无效：这次的读取应当仍被挂起')
  assertEq(typeRowText(RELAY_ID), '继电器', '慢读取未返回时显示的是上一次有效快照')

  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器E')
  await saveEditor()
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器E', 4000, '保存后的新快照先到位')

  // 旧请求现在才回来，带着保存前的旧表
  slow.resolve(clone([RELAY, RESISTOR]))
  await settle(4)
  assertEq(typeRowText(RELAY_ID), '继电器E', '迟到的旧读取不得覆盖保存后的新快照')
  assertEq(backend.types.find((t) => t.id === RELAY_ID).name, '继电器E', '写入本身没有被迟到回包影响')
}

// U03-10 草稿不串：取消不请求，关闭重开是新会话
async function draftsDoNotLeak() {
  await mountApp()
  await openTypeManager()
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), 'TEMP-DRAFT')
  click(byTestId('type-editor-cancel'))
  await settle(1)
  assert(!byTestId('type-save'), '取消后编辑器关闭')
  assertEq(countPosts('/api/component-types'), 0, '取消不发任何请求')
  assertEq(typeRowText(RELAY_ID), '继电器', '取消不改列表')

  openEditor(RELAY_ID)
  await settle(1)
  assertEq(byTestId('type-name').value, '继电器',
    '重新打开显示的是条目当前值，不是上一次的草稿')
  click(byTestId('type-editor-cancel'))
  await settle(1)

  closeTypeManager()
  await settle(1)
  await openTypeManager()
  assertEq(typeRowText(RELAY_ID), '继电器', '关闭重开后列表仍是原值')
}

// U03-11 内置条目：展示字段可改、ID 不变、删键禁用；被引用的自定义类型仍然拒删
async function deleteProtection() {
  await mountApp()
  await openTypeManager()
  // 内置类型的展示名可以改，提交的仍是稳定 ID
  openEditor(RESISTOR.id)
  await settle(1)
  setInput(byTestId('type-name'), 'Resistor2')
  await saveEditor()
  const editPost = lastPost('/api/component-types')
  assertEq(editPost.body.id, RESISTOR.id, '内置类型按稳定 ID 提交')
  assertEq(editPost.body.name, 'Resistor2', '内置类型允许改展示名')
  assert(!('icon' in editPost.body) && !('color' in editPost.body), '只改名称不带图标/颜色')
  await waitUntil(() => typeRowText(RESISTOR.id) === 'Resistor2', 4000, '内置类型改名后列表更新')

  const builtinRow = typeRow(RESISTOR.id)
  assert(builtinRow, '夹具无效：内置类型 Resistor 应在列表里')
  const builtinDelete = [...builtinRow.querySelectorAll('button')].find((b) => (b.textContent || '').includes('×'))
  assert(builtinDelete?.disabled === true, '内置类型的删除键必须禁用')
  assert(/不可删除/.test(builtinDelete.getAttribute('title') || ''),
    `内置类型的删除键要说明原因，实际 title=${JSON.stringify(builtinDelete.getAttribute('title'))}`)
  // 真正拒删的判定仍在服务端（C++ 侧覆盖）：界面这里不得把内置项放成可点

  backend.types.find((t) => t.id === RELAY_ID).references = 2
  const relayRow = typeRow(RELAY_ID)
  const relayDelete = [...relayRow.querySelectorAll('button')].find((b) => (b.textContent || '').includes('×'))
  assert(relayDelete && !relayDelete.disabled, '自定义类型的删除键可用')
  click(relayDelete)
  await waitUntil(() => (($('#toast')?.textContent) || '').includes('2'), 4000, '被引用类型拒删要回报引用数')
  assert(typeRow(RELAY_ID), '拒删后条目仍在列表里')
  assertEq(lastPost('/api/component-types').body.action, 'remove', '删除请求的 action')
  assertEq(lastPost('/api/component-types').body.id, RELAY_ID, '删除按 id 提交')
  // 拒删之后操作解冻：同一个删除键可以再试（失败可重试）
  const retryDelete = [...typeRow(RELAY_ID).querySelectorAll('button')].find((b) => (b.textContent || '').includes('×'))
  assert(retryDelete && !retryDelete.disabled, '拒删后删除键要恢复可用')
  delete backend.types.find((t) => t.id === RELAY_ID).references
  await flushToast()
}

// U03-12 未知历史 ID：原样显示 ID，不回退成 Unknown，也不能凭展示名猜配置
async function unknownHistoricalId() {
  await mountApp()
  const legacyCard = symbolCard('K_LEGACY')
  assert(legacyCard, '夹具无效：找不到 K_LEGACY 卡片')
  assertEq(legacyCard.querySelector('.type-tag').textContent, LEGACY_ID,
    'SymbolTable 对认不出的历史类型 ID 要原样显示')

  const title = $$('.sidebar .section-title').find((el) => (el.textContent || '').includes('元件类型'))
  assert(title, '夹具无效：Sidebar 没有「元件类型」分组标题')
  if (!$$('.sidebar .type-dot').length) click(title)
  const legacyRow = $$('.sidebar .nav-item').find((el) =>
    el.querySelector('.type-dot') && (el.textContent || '').includes(LEGACY_ID))
  assert(legacyRow, 'Sidebar 类型行按 ID 归组，认不出的历史 ID 也要有一行')
  assertEq(legacyRow.querySelector('.type-dot').textContent, '○',
    '历史 ID 没有配置 → 显示共享通用占位 ○')

  click(legacyCard)
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_LEGACY', 4000, 'Inspector 显示 K_LEGACY')
  assertEq($('.inspector-type').textContent, LEGACY_ID, 'Inspector 原样显示认不出的历史类型 ID')
  assertEq($('.inspector-icon').textContent, '○', 'Inspector 没有配置图标时用通用占位')

  click(legacyRow)
  await settle(2)
  assert(!symbolCard('K_TEST'), '按历史 ID 筛选只留下该类型的符号')
  assert(symbolCard('K_LEGACY'), '按历史 ID 筛选命中该符号')
}

// U03-13 封装类型：新增/删除 smoke，证明没有被塞进元件类型字段
async function packageTypesSmoke() {
  await mountApp()
  await openToolsModal('封装类型')
  const row = $$('.rule-item').find((el) => el.querySelector('.rule-name')?.textContent === 'SMD_0603')
  assert(row, '夹具无效：封装类型列表没有 SMD_0603')
  const nameInput = lastModal().querySelector('input[placeholder]')
  setInput(nameInput, 'QFN-99')
  setSelect(lastModal().querySelector('select'), 'SMD IC')
  const add = $$('.modal button').find((b) => (b.textContent || '').trim() === '添加')
  click(add)
  await waitUntil(() => $$('.rule-item').some((el) => el.querySelector('.rule-name')?.textContent === 'QFN-99'),
    4000, '封装类型新增后出现在列表里')
  const post = lastPost('/api/package-types')
  assertEq(post.body.action, 'add', '封装类型新增的 action')
  assertEq(post.body.name, 'QFN-99', '封装类型新增带 name')
  assertEq(post.body.category, 'SMD IC', '封装类型新增带 category')
  assert(!('icon' in post.body) && !('color' in post.body),
    `封装类型请求不得夹带元件类型字段：${JSON.stringify(post.body)}`)

  const added = $$('.rule-item').find((el) => el.querySelector('.rule-name')?.textContent === 'QFN-99')
  const removeBtn = [...added.querySelectorAll('button')].find((b) => (b.textContent || '').includes('×'))
  click(removeBtn)
  await waitUntil(() => !$$('.rule-item').some((el) => el.querySelector('.rule-name')?.textContent === 'QFN-99'),
    4000, '封装类型删除后从列表消失')
  assertEq(lastPost('/api/package-types').body.action, 'remove', '封装类型删除的 action')
  await flushToast()
}

// U03-A 历史显示字段不合法（非 hex 颜色 / 超长图标）也不阻挡「只改名称」
async function legacyNameOnlyEditKeepsFields() {
  backend.types = [clone(LEGACY_COLOR), clone(LEGACY_LONG_ICON)]
  await mountApp()
  await openTypeManager()
  assertEq(typeRowText(LEGACY_COLOR.id), '历史色', '夹具无效：历史颜色条目应能加载进列表')

  openEditor(LEGACY_COLOR.id)
  await settle(1)
  setInput(byTestId('type-name'), '历史色B')
  click(byTestId('type-save'))
  await settle(3)
  const post = lastPost('/api/component-types')
  assert(post, '只改名称必须真的提交：历史颜色不是合法 #RRGGBB，但不该把这次改名挡住')
  assertEq(post.body.action, 'update', '编辑请求的 action')
  assertEq(post.body.id, LEGACY_COLOR.id, '按稳定 id 提交')
  assertEq(post.body.name, '历史色B', '只提交改动的名称')
  assert(!('icon' in post.body), `没动的图标不得提交：${JSON.stringify(post.body)}`)
  assert(!('color' in post.body), `没动的历史颜色不得提交：${JSON.stringify(post.body)}`)
  await waitUntil(() => typeRowText(LEGACY_COLOR.id) === '历史色B', 4000, '列表刷新出新的显示名')
  assertEq(backend.types.find((t) => t.id === LEGACY_COLOR.id).color, 'red', '历史颜色值原样保留')
  assertEq(backend.types.find((t) => t.id === LEGACY_COLOR.id).icon, 'L', '图标原样保留')

  openEditor(LEGACY_LONG_ICON.id)
  await settle(1)
  setInput(byTestId('type-name'), '历史图标B')
  click(byTestId('type-save'))
  await settle(3)
  const post2 = lastPost('/api/component-types')
  assert(post2, '超长历史图标同样不能挡住「只改名称」')
  assertEq(post2.body.id, LEGACY_LONG_ICON.id, '按稳定 id 提交')
  assert(!('icon' in post2.body), `没动的超长图标不得提交：${JSON.stringify(post2.body)}`)
  await waitUntil(() => typeRowText(LEGACY_LONG_ICON.id) === '历史图标B', 4000, '超长图标条目改名成功')
  assertEq(backend.types.find((t) => t.id === LEGACY_LONG_ICON.id).icon, LONG_ICON, '超长历史图标原样保留')
}

// U03-A 绿控制：历史值可以显式修（清空 / 换成合法值 / 清掉超长图标）
async function legacyValueExplicitFixes() {
  backend.types = [clone(LEGACY_COLOR), clone(LEGACY_LONG_ICON)]
  await mountApp()
  await openTypeManager()

  openEditor(LEGACY_COLOR.id)
  await settle(1)
  click(byTestId('type-clear-color'))
  click(byTestId('type-save'))
  await settle(3)
  const cleared = lastPost('/api/component-types')
  assert(cleared, '显式清除历史颜色必须提交')
  assertEq(cleared.body.id, LEGACY_COLOR.id, '按稳定 id 提交')
  assertEq(cleared.body.color, '', '清除颜色提交显式空串')
  assert(!('name' in cleared.body), `名称没动就不提交：${JSON.stringify(cleared.body)}`)
  assert(!('icon' in cleared.body), `图标没动就不提交：${JSON.stringify(cleared.body)}`)

  openEditor(LEGACY_COLOR.id)
  await settle(1)
  setInput(inLast('#type-editor-color'), '#123456')
  click(byTestId('type-save'))
  await settle(3)
  const recolored = lastPost('/api/component-types')
  assertEq(recolored.body.color, '#123456', '历史颜色可以显式换成合法值')
  await waitUntil(() => typeIconNode(LEGACY_COLOR.id)?.style.color, 4000, '列表按新颜色渲染图标')
  assertEq(canonicalColor(typeIconNode(LEGACY_COLOR.id).style.color), canonicalColor('#123456'),
    '列表图标用刚改的颜色')

  // 旧超长图标 → 显式清空：只提交图标
  openEditor(LEGACY_LONG_ICON.id)
  await settle(1)
  setInput(inLast('#type-editor-icon'), '')
  click(byTestId('type-save'))
  await settle(3)
  const iconCleared = lastPost('/api/component-types')
  assert(iconCleared, '显式清空历史超长图标必须提交')
  assertEq(iconCleared.body.id, LEGACY_LONG_ICON.id, '按稳定 id 提交')
  assertEq(iconCleared.body.icon, '', '清空图标提交显式空串')
  assert(!('color' in iconCleared.body), `颜色没动就不提交：${JSON.stringify(iconCleared.body)}`)
}

// U03-A 红：旧色 + 超长图标同时存在时，修一个字段不能被没动的那一个挡住
async function legacyMixedFieldsCanBeFixedOneAtATime() {
  backend.types = [clone(LEGACY_BOTH)]
  await mountApp()
  await openTypeManager()
  openEditor(LEGACY_BOTH.id)
  await settle(1)
  click(byTestId('type-clear-color'))
  click(byTestId('type-save'))
  await settle(3)
  const fixed = lastPost('/api/component-types')
  assert(fixed, '旧色 + 超长图标同时存在时，修其中一个字段仍要提交')
  assertEq(fixed.body.id, LEGACY_BOTH.id, '按稳定 id 提交')
  assertEq(fixed.body.color, '', '只提交被修的颜色')
  assert(!('icon' in fixed.body), `没动的超长图标不得提交：${JSON.stringify(fixed.body)}`)
  assertEq(backend.types.find((t) => t.id === LEGACY_BOTH.id).icon, LONG_ICON, '超长图标原样保留')
}

// U03-B 已保存 + 刷新失败：所有写入入口都必须真的停下，只剩「重读」
async function staleFreezesWriteEntries() {
  await mountApp()
  await openTypeManager()
  respondOnce('/api/component-types', () => ({ status: 500, payload: { error: 'REFRESH-DOWN' } }), 'GET')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器F')
  await saveEditor()
  assert(byTestId('types-stale'), '保存成功但刷新失败要显示「已保存」')
  assertEq(typeRowText(RELAY_ID), '继电器', '刷新失败期间保留上一次有效快照')
  const posts = countPosts('/api/component-types')
  assertEq(posts, 1, '写入只有一次')

  // 编辑入口：能打开就真的试一次保存；被冻结也算通过 —— 但都不能多出写入
  if (rowEditButton(RELAY_ID) && rowEditButton(RELAY_ID).disabled !== true) {
    openEditor(RELAY_ID)
    await settle(1)
    if (byTestId('type-name')) {
      setInput(byTestId('type-name'), 'STALE-EDIT')
      click(byTestId('type-save'))
      await settle(2)
      if (byTestId('type-name')) {
        assertEq(byTestId('type-name').value, 'STALE-EDIT', '被拦下的保存不能顺手清掉用户输入')
      }
      if (byTestId('type-editor-cancel')) { click(byTestId('type-editor-cancel')); await settle(1) }
    }
  }
  assertEq(countPosts('/api/component-types'), posts, '待刷新期间编辑入口不得再发写入')

  const addOpen = byTestId('type-add-open')
  if (addOpen && !addOpen.disabled) {
    click(addOpen)
    await settle(1)
    if (byTestId('type-name')) {
      setInput(byTestId('type-name'), 'STALE-ADD')
      const iconInput = inLast('#type-editor-icon')
      if (iconInput) setInput(iconInput, 'S')
      click(byTestId('type-save'))
      await settle(2)
      if (byTestId('type-editor-cancel')) { click(byTestId('type-editor-cancel')); await settle(1) }
    }
  }
  assertEq(countPosts('/api/component-types'), posts, '待刷新期间新增入口不得再发写入')
  assert(!backend.types.some((t) => t.name === 'STALE-ADD'), '待刷新期间不得把新类型写进后端')

  const del = rowDeleteButton(RELAY_ID)
  if (del && !del.disabled) { click(del); await settle(2) }
  assertEq(countPosts('/api/component-types'), posts, '待刷新期间删除入口不得再发写入')
  assert(typeRow(RELAY_ID), '待刷新期间条目不能被删掉')

  // 只有「重读」是允许的：读成功后解冻，可以正常再写
  click(byTestId('types-retry-load'))
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器F', 4000, '重读后显示新快照')
  assert(!byTestId('types-stale'), '读取成功后待刷新提示消失')
  assertEq(countPosts('/api/component-types'), posts, '重读不发写入')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器G')
  await saveEditor()
  assertEq(countPosts('/api/component-types'), posts + 1, '解冻后可以正常再写一次')
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器G', 4000, '解冻后的写入生效')
}

// U03-B 真实 hook：待刷新（已保存但刷新失败）期间，直接调用 mutate 也不能绕过写保护
async function staleFreezesHookMutations() {
  await mountProbe()
  assertEq(probeState.types.length, 2, '夹具无效：首次加载应拿到两条类型')

  respondOnce('/api/component-types', () => ({ status: 500, payload: { error: 'REFRESH-DOWN' } }), 'GET')
  const saved = await probeRun(() => probeState.mutate({ action: 'update', id: RELAY_ID, name: '继电器H' }))
  assertEq(countPosts('/api/component-types'), 1, '第一次写入发出了 POST')
  assert(saved && saved.ok === true, `写入本身是成功的：${JSON.stringify(saved)}`)
  assert(saved.refreshError, '写入成功但刷新失败要如实回报（不能包装成保存失败）')
  assertEq(probeState.stale, true, '快照没刷新到位就要标记待刷新')
  assertEq(probeState.types.length, 2, '待刷新期间保留上一次有效快照')

  const posts = countPosts('/api/component-types')
  const blocked = await probeRun(() => probeState.mutate({ action: 'add', name: 'HOOK-ADD', icon: 'H', color: '' }))
  assert(blocked?.ok !== true, `待刷新期间不得假装写入成功：${JSON.stringify(blocked)}`)
  assertEq(countPosts('/api/component-types'), posts, '待刷新期间真实 mutate 也不能再发写入')
  assert(!backend.types.some((t) => t.name === 'HOOK-ADD'), '待刷新期间不得把类型写进后端')

  await probeRun(() => probeState.reload())
  assertEq(probeState.stale, false, '重读成功后待刷新标记消失')
  assertEq(countPosts('/api/component-types'), posts, '重读只发 GET')
  const after = await probeRun(() => probeState.mutate({ action: 'add', name: 'HOOK-ADD', icon: 'H', color: '' }))
  assertEq(after?.ok, true, '解冻后写入可用')
  assertEq(countPosts('/api/component-types'), posts + 1, '解冻后的写入真的发出去了')
  assert(backend.types.some((t) => t.name === 'HOOK-ADD'), '新类型写进后端')
}

// U03-B 首次读取就失败：没有有效表，不能当「已知为空」盲写
async function failedInitialLoadDoesNotAllowBlindWrites() {
  respondOnce('/api/component-types', () => ({ status: 500, payload: { error: 'INIT-DOWN' } }), 'GET')
  await mountProbe()
  assertEq(probeState.types.length, 0, '读取失败时没有有效快照（不是「已知为空」）')
  assert(String(probeState.error || '').includes('INIT-DOWN'),
    `首次读取失败要保留真实原因，实际 ${JSON.stringify(probeState.error)}`)

  const posts = countPosts('/api/component-types')
  const blind = await probeRun(() => probeState.mutate({ action: 'add', name: 'BLIND', icon: 'B', color: '' }))
  assert(blind?.ok !== true, `没有有效类型表时不得假装写入成功：${JSON.stringify(blind)}`)
  assertEq(countPosts('/api/component-types'), posts, '没有有效类型表时不得盲写')
  assert(!backend.types.some((t) => t.name === 'BLIND'), '没有有效类型表时不得把类型写进后端')

  await probeRun(() => probeState.reload())
  assert(!probeState.error, `重读成功后不该再有错误，实际 ${JSON.stringify(probeState.error)}`)
  assertEq(probeState.types.length, 2, '重读拿回有效快照')
  const ok = await probeRun(() => probeState.mutate({ action: 'add', name: 'BLIND', icon: 'B', color: '' }))
  assertEq(ok?.ok, true, '拿回有效快照后写入可用')
}

// U03-C 被取代的旧读结束，不能把已经刷新的新快照又标成待刷新
async function supersededOldReadDoesNotMarkStale() {
  await mountProbe()
  assertEq(probeState.types.length, 2, '夹具无效：首次加载应有两条类型')

  const readA = holdNext('/api/component-types', 'GET')   // 写入后的内部刷新读
  const mutation = probeStart(() => probeState.mutate({ action: 'update', id: RELAY_ID, name: '继电器A' }))
  await waitUntil(() => readA.call, 4000, '写入成功后发出了内部刷新读 A')
  assertEq(countPosts('/api/component-types'), 1, '写入只有一次 POST')

  const readB = holdNext('/api/component-types', 'GET')   // 真实 reload 发起的较新读
  const reloading = probeStart(() => probeState.reload())
  await waitUntil(() => readB.call, 4000, 'reload 发出了较新的读 B')

  readB.resolve([{ ...clone(RELAY), name: '继电器B' }, clone(RESISTOR)])
  await waitUntil(() => probeTypeName(RELAY_ID) === '继电器B', 4000, '较新的读 B 先发布了最新快照')
  assertEq(probeState.stale, false, 'B 已经把快照刷新到位，不是待刷新状态')

  readA.resolve(clone([RELAY, RESISTOR]))   // 迟到的旧读，带着保存前的旧表
  await probeFinish(mutation)
  await probeFinish(reloading)
  await settle(2)
  assertEq(probeTypeName(RELAY_ID), '继电器B', '迟到的旧读不能覆盖较新的快照')
  assert(!probeState.error, `被取代的旧读不是失败：不能留下错误，实际 ${JSON.stringify(probeState.error)}`)
  assertEq(probeState.stale, false, '被取代的旧读不能把已刷新的快照标成待刷新')
  assertEq(probeState.loading, false, '两条读都结束后不处于加载中')
  assertEq(countPosts('/api/component-types'), 1, '整个过程只有一次写入')
}

// U03-C 较新的读还没落地：不能因为旧读先结束就提前解冻写入
async function newerPendingReadKeepsWriteProtection() {
  await mountProbe()
  const readA = holdNext('/api/component-types', 'GET')
  const mutation = probeStart(() => probeState.mutate({ action: 'update', id: RELAY_ID, name: '继电器C' }))
  await waitUntil(() => readA.call, 4000, '写入成功后发出了内部刷新读 A')

  const readB = holdNext('/api/component-types', 'GET')
  const reloading = probeStart(() => probeState.reload())
  await waitUntil(() => readB.call, 4000, 'reload 发出了较新的读 B')

  // 被取代的旧读先结束：此时较新的 B 仍未落地 —— 写保护必须还在。
  // 不在这里 await 原 mutation：正确实现完全可以等到最新读 B 落地才收尾。
  readA.resolve(clone([RELAY, RESISTOR]))
  await settle(2)
  const posts = countPosts('/api/component-types')
  const blocked = await probeRun(() => probeState.mutate({ action: 'add', name: 'C2-ADD', icon: 'C', color: '' }))
  assert(blocked?.ok !== true, `最新读取还没落地时不得假装写入成功：${JSON.stringify(blocked)}`)
  assertEq(countPosts('/api/component-types'), posts, '最新读取还没落地时不得再发写入')
  assert(!backend.types.some((t) => t.name === 'C2-ADD'), '最新读取还没落地时不得把类型写进后端')
  assertEq(probeTypeName(RELAY_ID), '继电器', '旧读结束后快照仍是保存前那一次有效值')

  readB.resolve([{ ...clone(RELAY), name: '继电器C' }, clone(RESISTOR)])
  await probeFinish(mutation)
  await probeFinish(reloading)
  await waitUntil(() => probeTypeName(RELAY_ID) === '继电器C' && probeState.loading === false, 4000,
    'B 成功落地后才解冻')
  assertEq(probeState.stale, false, 'B 成功刷新出新有效表后不再处于待刷新')
  const ok = await probeRun(() => probeState.mutate({ action: 'add', name: 'C2-ADD', icon: 'C', color: '' }))
  assertEq(ok?.ok, true, 'B 成功刷新出新有效表后才允许新写入')
  assert(backend.types.some((t) => t.name === 'C2-ADD'), '解冻后的新增真的写进去了')
}

// U03-D 损坏的类型列表：保留最后一份有效快照、明确报错，不假装成功也不崩。
// 每个形状各跑一条（崩掉的那条不能顺带带走后面的用例）。
async function corruptListCase(label, payload) {
  await mountApp()
  await openTypeManager()
  assert(typeRow(RELAY_ID), '夹具无效：首次读取后应有继电器条目')
  closeTypeManager()
  await settle(1)

  respondOnce('/api/component-types', () => ({ status: 200, payload }), 'GET')
  try {
    await openTypeManager()
  } catch (cause) {
    throw new Error(`损坏列表（${label}）把界面弄崩了：${cause?.message || cause}`)
  }
  const banner = byTestId('types-load-failed')
  assert(banner, `损坏列表（${label}）必须报读取失败，不能当成一次成功读取`)
  assertEq(typeRowText(RELAY_ID), '继电器', `损坏列表（${label}）不能顶掉上一次有效快照`)
  assertEq(typeIconNode(RELAY_ID)?.textContent, '↯', `损坏列表（${label}）后旧图标仍在`)
  click(byTestId('types-retry-load'))
  await waitUntil(() => !byTestId('types-load-failed'), 4000, `${label}：重读成功后提示消失`)
  assert(typeRow(RELAY_ID), `${label}：重读后条目仍在`)
}

// U03-D HTTP 200 但应用层失败：真实原因要透到界面，不能被通用文案吃掉
async function appLevelFailureKeepsReason() {
  await mountApp()
  await openTypeManager()
  assert(typeRow(RELAY_ID), '夹具无效：首次读取后应有继电器条目')
  closeTypeManager()
  await settle(1)

  respondOnce('/api/component-types', () => ({ status: 200, payload: { ok: false, error: 'DB-DOWN' } }), 'GET')
  await openTypeManager()
  const banner = byTestId('types-load-failed')
  assert(banner, '应用层 ok:false 必须报读取失败')
  assert((banner.textContent || '').includes('DB-DOWN'),
    `要显示服务端给出的真实原因，实际 ${JSON.stringify(banner.textContent)}`)
  assertEq(typeRowText(RELAY_ID), '继电器', '应用层失败时保留上一次有效快照')
  click(byTestId('types-retry-load'))
  await waitUntil(() => !byTestId('types-load-failed'), 4000, '重读成功后提示消失')
  assert(typeRow(RELAY_ID), '重读后条目仍在')
  assertEq(countPosts('/api/component-types'), 0, '整条路径只有读取，没有任何写入')
}

// U03-E 写入在飞期间关闭/重开：旧写入的成功回包不能关掉新编辑器的草稿
async function closingDuringSaveDoesNotKillNewDraft() {
  await mountApp()
  await openTypeManager()
  const gateA = holdNext('/api/component-types', 'POST')
  openEditor(RELAY_ID)
  await settle(1)
  setInput(byTestId('type-name'), '继电器I')
  click(byTestId('type-save'))
  await settle(1)
  assertEq(countPosts('/api/component-types'), 1, '写入在飞期间只有一次 POST')
  assert(!gateA.done, '夹具无效：这次写入应当仍被挂起')

  // 真实用户关闭路径：编辑器自己的关闭键（不是被禁用的取消键）
  const editorClose = lastModal()?.querySelector('.modal-close')
  assert(editorClose, '夹具无效：编辑器没有关闭键')
  click(editorClose)
  await settle(1)

  if (byTestId('type-save')) {
    // 策略一：写入期间关闭被冻结 —— 编辑器必须还在，Escape 也不能弄丢它
    pressEscape()
    await settle(1)
    assert(byTestId('type-save'), '写入在飞期间编辑器不应被关掉（冻结策略下 Escape 也不行）')
    gateA.resolveThrough(applyComponentWrite)
    await waitUntil(() => typeRowText(RELAY_ID) === '继电器I', 4000, '放行后写入完成并刷新')
    assertEq(countPosts('/api/component-types'), 1, '整个过程只有一次写入')
    // 冻结只针对写入期间：写完之后入口要恢复（不能一直锁着）
    if (byTestId('type-editor-cancel')) { click(byTestId('type-editor-cancel')); await settle(1) }
    assert(!byTestId('type-save'), '写入完成后编辑器可以正常关闭')
    return
  }

  // 策略二：允许关闭 —— 重开一个不同条目的编辑草稿，旧写入完成不能把它清掉
  if (!typeRow(RELAY_ID)) await openTypeManager()
  openEditor(RESISTOR.id)
  await settle(1)
  assertEq(byTestId('type-name')?.value, 'Resistor', '夹具无效：新编辑器应显示该条目当前值')
  setInput(byTestId('type-name'), 'DRAFT-B')
  gateA.resolveThrough(applyComponentWrite)
  await waitUntil(() => typeRowText(RELAY_ID) === '继电器I', 4000, '放行的写入完成并刷新')
  await settle(2)
  assert(byTestId('type-save'), '旧写入的成功回包不能关掉新打开的编辑器')
  assertEq(byTestId('type-name')?.value, 'DRAFT-B', '旧写入的回包不能清掉新编辑器的草稿')
  assertEq(countPosts('/api/component-types'), 1, '旧写入完成后不得再发一次写入')
  click(byTestId('type-save'))
  await settle(3)
  assertEq(lastPost('/api/component-types').body.id, RESISTOR.id, '新草稿保存时按自己的稳定 id 提交')
  assertEq(countPosts('/api/component-types'), 2, '新草稿保存是第二次（也是最后一次）写入')
}

// U03-E 图标是纯文本：HTML 字面量只以文本出现，预览也不注入
async function iconStaysPlainText() {
  await mountApp()
  await openTypeManager()
  openEditor(RELAY_ID)
  await settle(1)
  setInput(inLast('#type-editor-icon'), '<b>i</b>')
  await saveEditor()
  await waitUntil(() => (typeRow(RELAY_ID)?.textContent || '').includes('<b>i</b>'), 4000,
    '图标按文本原样显示在列表里')
  assertEq(typeIconNode(RELAY_ID).querySelector('*'), null,
    '图标节点里不能出现真实元素：图标不得走 innerHTML')
  openEditor(RELAY_ID)
  await settle(1)
  const preview = lastModal()?.querySelector('.type-icon-preview')
  assert(preview, '夹具无效：编辑器没有图标预览节点')
  assertEq(preview.querySelector('*'), null, '图标预览不得把图标注入成 HTML')
  assertEq(preview.textContent, '<b>i</b>', '预览显示的是图标原文')
  click(byTestId('type-editor-cancel'))
  await settle(1)
}

// ============================================================
// S01：真实 Sidebar 薄夹具（拆分后仍要保住的既有行为）
// 只挂真实组件、记录它发出去的意图与参数顺序；不复制 Sidebar 的筛选/渲染规则，
// 也不按组件个数或源码行数断言。受控数据全是本文件自己的合成对象。
// ============================================================
const WS_A = { id: 'ws-a', name: '工作库A', locked: false }
const WS_B = { id: 'ws-b', name: '工作库B', locked: true }
const LIB_A = { id: 'lib-a', name: 'Alpha', locked: false, component_library_id: 'ws-a' }
const LIB_B = { id: 'lib-b', name: 'Beta', locked: true, component_library_id: 'ws-b' }
const LIB_NO_WS = { id: 'lib-nows', name: 'Orphan', locked: false, component_library_id: '' }
const SIDEBAR_LIBS = [LIB_A, LIB_B, LIB_NO_WS]
// 图标链最终落到 <img src>：用本页真实存在的静态资源，不制造加载失败
const SIDEBAR_RELAY = { id: RELAY_ID, name: '继电器', icon: '↯', color: '#123456' }
const SIDEBAR_LEGACY = { id: LEGACY_ID, name: '历史类型', icon: '', color: '' }
/// 记录「哪个 callback、按什么顺序、带了什么参数」——调用顺序本身就是要保的行为
function makeRecorder() {
  const calls = []
  const rec = (name) => (...args) => { calls.push({ name, args }) }
  return { calls, rec, names: () => calls.map((c) => c.name) }
}

// 六组职责对象各自的受控默认值：场景只覆盖自己关心的那一组，
// 组与组之间不串数据。形状与 App 调用处一致。
const wsGroup = (o = {}) => ({ items: [], selectedId: '', onSelect: () => {}, ...o })
const navGroup = (o = {}) => ({ activeAll: false, activeIssues: false, allCount: 0, issuesCount: 0,
  matchesCount: 0, onSelectAll: null, onSelectIssues: () => {}, onAutoMatch: () => {}, ...o })
const typesGroup = (o = {}) => ({ items: [], metadata: buildTypeMap([]), selectedId: '', onSelect: () => {}, ...o })
const libsGroup = (o = {}) => ({ items: [], workspaceId: '', selectedId: '',
  onSelect: () => {}, onDeleteRequest: () => {}, ...o })
function sidebarProps(overrides = {}) {
  return {
    workspace: wsGroup(), navigation: navGroup(), types: typesGroup(),
    libraries: libsGroup(),
    ...overrides,
  }
}
function mountSidebar(overrides) { mount(<Sidebar {...sidebarProps(overrides)} />) }
function rerenderSidebar(overrides) {
  if (!root) throw new Error('rerenderSidebar: 还没有挂载过 Sidebar')
  act(() => { root.render(<Sidebar {...sidebarProps(overrides)} />) })
}
function sidebarNavItem(text) {
  return $$('.sidebar .nav-item').find((el) => (el.textContent || '').includes(text))
}
function activityBarItem(text) {
  return $$('.activitybar .nav-item').find((el) => (el.getAttribute('title') || '').includes(text))
}
function sidebarTypeRows() {
  return $$('.sidebar .nav-item').filter((el) => el.querySelector('.type-dot'))
}
function sidebarTypeTitle() {
  return $$('.sidebar .section-title').find((el) => (el.textContent || '').includes('元件类型'))
}
function expandTypeFilter() {
  const title = sidebarTypeTitle()
  assert(title, '夹具无效：Sidebar 没有「元件类型」分组标题')
  if (!sidebarTypeRows().length) click(title)
  return title
}
function sidebarLibraryRows() {
  return $$('.sidebar .nav-item').filter((el) => el.querySelector('.dot.dot-ok'))
}
function sidebarLibraryNames() {
  return sidebarLibraryRows().map((el) => (el.firstElementChild?.textContent || '').trim())
}
// S01 场景：真实 Sidebar 的既有行为（拆分前后必须一致）
// ============================================================
// 工作库范围：空 activeLib 列全部库，选定工作库只列归属匹配的库，没有归属的自己不算
async function sidebarScopesLibrariesByWorkspace() {
  const c = makeRecorder()
  mountSidebar({
    libraries: libsGroup({ items: SIDEBAR_LIBS }),
    workspace: wsGroup({ items: [WS_A, WS_B], onSelect: c.rec('onSelect') }),
  })
  await settle(1)
  const sel = $('.sidebar select')
  assert(sel, '有工作库时要画工作库选择器')
  assertEq(sel.value, '', '未限定工作库时选择器停在「全部工作库」')
  assertEq([...sel.options].map((o) => o.textContent).join('|'), '全部工作库|工作库A|🔒 工作库B',
    '工作库选项按原顺序、锁定标记原样')
  assertEq(sidebarLibraryNames().join('|'), 'Alpha|🔒 Beta|Orphan', '不限工作库时列出全部符号库（锁定标记原样）')

  // 切换工作库只是一个意图：值怎么变由 App 决定
  setSelect(sel, 'ws-b')
  await settle(1)
  assertEq(c.names().join('|'), 'onSelect', '切换工作库只发选择意图')
  assertEq(c.calls[0].args[0], 'ws-b', '选择回调带工作库 ID')

  rerenderSidebar({ libraries: libsGroup({ items: SIDEBAR_LIBS, workspaceId: 'ws-a' }),
    workspace: wsGroup({ items: [WS_A, WS_B], selectedId: 'ws-a' }) })
  await settle(1)
  assertEq($('.sidebar select').value, 'ws-a', 'selectedId 是唯一来源，选择器照实显示')
  assertEq(sidebarLibraryNames().join('|'), 'Alpha', '只列归属该工作库的符号库')

  rerenderSidebar({ libraries: libsGroup({ items: SIDEBAR_LIBS, workspaceId: 'ws-b' }),
    workspace: wsGroup({ items: [WS_A, WS_B], selectedId: 'ws-b' }) })
  await settle(1)
  assertEq(sidebarLibraryNames().join('|'), '🔒 Beta', 'component_library_id 不匹配的库不出现')

  // 空工作库：给空状态文案，而不是把别的库塞进来
  rerenderSidebar({ libraries: libsGroup({ items: SIDEBAR_LIBS, workspaceId: 'ws-none' }),
    workspace: wsGroup({ items: [WS_A, WS_B], selectedId: 'ws-none' }) })
  await settle(1)
  assertEq(sidebarLibraryRows().length, 0, '归属为空的库不属于任何工作库')
  assert(($('.sidebar').textContent || '').includes('该工作库下暂无符号库'), '空工作库要给空状态文案')

  // 没有工作库时不画选择器
  rerenderSidebar({ libraries: libsGroup({ items: SIDEBAR_LIBS }), workspace: wsGroup() })
  await settle(1)
  assertEq($('.sidebar select'), null, '没有工作库时不画工作库选择器')
  assertEq(sidebarLibraryNames().join('|'), 'Alpha|🔒 Beta|Orphan', '没有工作库时仍列出全部符号库')
}

// 类型筛选：按稳定 ID 提交，图标/名称/颜色来自共享 metadata，历史 ID 原样显示
async function sidebarTypeFilterUsesStableIds() {
  const c = makeRecorder()
  mountSidebar({
    types: typesGroup({
      items: [[RELAY_ID, '服务端展示名', 3], [LEGACY_ID, '历史类型', 1]],
      metadata: buildTypeMap([SIDEBAR_RELAY, SIDEBAR_LEGACY]),
      onSelect: c.rec('onSelect'),
    }),
  })
  await settle(1)
  assertEq(sidebarTypeRows().length, 0, '未展开时不渲染类型行')
  expandTypeFilter()
  await settle(1)
  const rows = sidebarTypeRows()
  assertEq(rows.length, 2, '展开后每个类型一行')

  const relayDot = rows[0].querySelector('.type-dot')
  assertEq(relayDot.textContent, '↯', '类型行图标取自类型配置')
  assertEq(canonicalColor(relayDot.style.color), canonicalColor('#123456'), '类型行颜色取自同一份配置')
  assert((rows[0].textContent || '').includes('继电器'), '登记的展示名优先于服务端给的展示名')

  const legacyDot = rows[1].querySelector('.type-dot')
  assertEq(legacyDot.textContent, '○', '没有图标的历史类型用共享通用占位')
  assertEq(legacyDot.getAttribute('style'), null, '没有配置颜色时不写死颜色')
  assert((rows[1].textContent || '').includes('历史类型'), '认不出的历史 ID 原样显示展示名')

  click(rows[0])
  await settle(1)
  assertEq(c.names().join('|'), 'onSelect', '点击类型只发一个选择意图（清选中归 App 的 handler）')
  assertEq(c.calls[0].args[0], RELAY_ID, '选择按稳定 ID 提交，不按展示名反猜')

  // 当前筛选命中时该行高亮（按 ID 比较）
  rerenderSidebar({
    types: typesGroup({
      items: [[RELAY_ID, '服务端展示名', 3], [LEGACY_ID, '历史类型', 1]],
      metadata: buildTypeMap([SIDEBAR_RELAY, SIDEBAR_LEGACY]),
      selectedId: RELAY_ID, onSelect: c.rec('onSelect'),
    }),
  })
  await settle(1)
  assert(sidebarTypeRows()[0].classList.contains('active'), '当前筛选的类型行要高亮')
  assert(!sidebarTypeRows()[1].classList.contains('active'), '别的类型行不能高亮')
}

// 类型表是活快照：metadata 变化即时反映，展开状态不被类型表更新或工作库切换重置
async function sidebarTypeMetadataUpdatesLive() {
  const items = [[RELAY_ID, '服务端展示名', 3]]
  mountSidebar({
    types: typesGroup({ items, metadata: buildTypeMap([SIDEBAR_RELAY]) }),
    workspace: wsGroup({ items: [WS_A] }),
  })
  await settle(1)
  expandTypeFilter()
  await settle(1)
  assertEq(sidebarTypeRows().length, 1, '展开后有类型行')

  rerenderSidebar({
    types: typesGroup({ items, metadata: buildTypeMap([{ ...SIDEBAR_RELAY, name: '继电器v2', icon: '⟳' }]) }),
    workspace: wsGroup({ items: [WS_A] }),
  })
  await settle(1)
  assertEq(sidebarTypeRows().length, 1, '类型表更新不能让展开状态被重置（没有 key 变化）')
  assertEq(sidebarTypeRows()[0].querySelector('.type-dot').textContent, '⟳', '图标按最新配置即时更新')
  assert((sidebarTypeRows()[0].textContent || '').includes('继电器v2'), '名称按最新配置即时更新')

  rerenderSidebar({
    types: typesGroup({ items, metadata: buildTypeMap([{ ...SIDEBAR_RELAY, name: '继电器v2', icon: '⟳' }]) }),
    workspace: wsGroup({ items: [WS_A], selectedId: 'ws-a' }),
    libraries: libsGroup({ workspaceId: 'ws-a' }),
  })
  await settle(1)
  assertEq(sidebarTypeRows().length, 1, '切工作库不能让类型展开状态重置')

  click(sidebarTypeTitle())
  await settle(1)
  assertEq(sidebarTypeRows().length, 0, '再点一次标题要能收起')
  click(sidebarTypeTitle())
  await settle(1)
  assertEq(sidebarTypeRows().length, 1, '收起后还能再展开')
}

// 符号库行：点击带 UUID 选中；删除只请求删除，绝不连带触发选中
async function sidebarLibraryRowSelectAndDelete() {
  const c = makeRecorder()
  mountSidebar({
    libraries: libsGroup({ items: SIDEBAR_LIBS, selectedId: 'lib-b',
      onSelect: c.rec('onSelect'), onDeleteRequest: c.rec('onDeleteRequest') }),
  })
  await settle(1)
  const rows = sidebarLibraryRows()
  assertEq(rows.length, 3, '三个库各一行')
  assert(rows[1].classList.contains('active'), '当前目标库那一行要高亮')
  assert(!rows[0].classList.contains('active'), '别的库不能高亮')

  click(rows[0])
  await settle(1)
  assertEq(c.names().join('|'), 'onSelect', '点库行只发选中意图')
  assertEq(c.calls[0].args[0], 'lib-a', '选中回调带的是库 ID')

  const del = rows[2].lastElementChild
  assertEq((del.textContent || '').trim(), '×', '删除入口在行尾')
  click(del)
  await settle(1)
  assertEq(c.names().join('|'), 'onSelect|onDeleteRequest', '删除只额外发一次请求删除，不能触发选中')
  const req = c.calls[1].args[0]
  assertEq(req.kind, 'symbol', '删除请求针对符号库')
  assertEq(req.lib, LIB_NO_WS, '删除请求带的是原库对象（不是副本），由 App 决定后续')
}

// 三项导航：意图顺序、可选 onSelectAll、徽标计数
async function sidebarNavigationIntents() {
  const c = makeRecorder()
  mountSidebar({
    navigation: navGroup({
      activeAll: true, allCount: 7, issuesCount: 3, matchesCount: 2,
      onSelectAll: c.rec('onSelectAll'), onSelectIssues: c.rec('onSelectIssues'),
      onAutoMatch: c.rec('onAutoMatch'),
    }),
  })
  await settle(1)
  assertEq(sidebarNavItem('全部元件').querySelector('.badge').textContent, '7', '全部徽标显示元件总数')
  assertEq(sidebarNavItem('问题').querySelector('.badge').textContent, '3', '问题徽标显示问题数')
  assertEq(sidebarNavItem('匹配').querySelector('.badge').textContent, '2', '匹配徽标显示匹配数')
  assert(sidebarNavItem('全部元件').classList.contains('active'),
    '没有筛选也没有问题筛选时「全部」高亮')
  assert(!sidebarNavItem('问题').classList.contains('active'), '未开问题筛选时问题行不高亮')

  click(sidebarNavItem('全部元件'))
  await settle(1)
  assertEq(c.names().join('|'), 'onSelectAll', '「全部」只把意图交回 App（清筛选/清选中都在那里完成）')

  click(sidebarNavItem('问题'))
  await settle(1)
  assertEq(c.names().slice(1).join('|'), 'onSelectIssues', '「问题」只发一个意图')

  click(sidebarNavItem('匹配'))
  await settle(1)
  assertEq(c.names().slice(2).join('|'), 'onAutoMatch', '「匹配」只发一个意图')

  // onSelectAll 可缺省：没有它时其余两个意图照发，不能崩
  rerenderSidebar({
    navigation: navGroup({ onSelectIssues: c.rec('onSelectIssues'), onAutoMatch: c.rec('onAutoMatch') }),
  })
  await settle(1)
  click(sidebarNavItem('全部元件'))
  await settle(1)
  assertEq(c.names().slice(3).join('|'), '', '没有 onSelectAll 时点击仍不报错，也不发别的意图')

  // 问题筛选打开时问题行高亮（沿用原有 active 条件，本轮只做结构拆分）
  rerenderSidebar({ navigation: navGroup({ activeIssues: true, issuesCount: 1 }) })
  await settle(1)
  assert(sidebarNavItem('问题').classList.contains('active'), '开了问题筛选后问题行高亮')
  assert(!sidebarNavItem('全部元件').classList.contains('active'), '问题筛选打开时「全部」沿用原条件不高亮')
}

// 设置入口：活动栏底部只发「打开」意图（先读配置再开弹窗的顺序见真实 App 场景）
async function sidebarSettingsEntry() {
  const c = makeRecorder()
  mount(<ActivityBar activities={[]} activeId="kf:workspace" onSelect={() => {}}
    onOpenSettings={c.rec('onOpen')} />)
  await settle(1)
  const row = activityBarItem('设置')
  assert(row, '活动栏底部要有设置入口')
  click(row)
  await settle(1)
  assertEq(c.names().join('|'), 'onOpen', '设置入口只发打开意图（先读配置再开弹窗归 App）')
}

// 空数据：没有工作库/插件/符号库/类型时正常渲染，不崩也不多画
async function sidebarEmptyDataRenders() {
  mountSidebar({ libraries: libsGroup({ items: SIDEBAR_LIBS }) })
  await settle(1)
  assert($('.sidebar'), 'Sidebar 根节点要在')
  assertEq($('.sidebar select'), null, '没有工作库时不画工作库选择器')
  assertEq(sidebarLibraryRows().length, 3, '有符号库时照常列出')
  rerenderSidebar({ libraries: libsGroup() })
  await settle(1)
  assertEq(sidebarLibraryRows().length, 0, '没有符号库时不画库行')
  assert(($('.sidebar').textContent || '').includes('该工作库下暂无符号库'), '没有符号库时给空状态文案')
  assertEq(sidebarTypeRows().length, 0, '没有类型时不画类型行')
  assertEq(sidebarNavItem('全部元件').querySelector('.badge').textContent, '0', '空数据的徽标是 0')

}

// 语言与主题：只在设置弹窗里；语言走真实 i18n，主题只发切换意图（finally 恢复 zh）
async function settingsLanguageAndTheme() {
  const c = makeRecorder()
  try {
    mount(<SettingsModal settings={{}} setSettings={() => {}} compLibraries={[]}
      loadCompLibraries={() => {}} onClose={() => {}} refreshSymbols={() => {}}
      loadStatus={() => {}} toastMsg={() => {}} showConfirm={async () => false}
      onRequestDelete={() => {}} dark={false} onToggleTheme={c.rec('onToggleTheme')} />)
    await settle(1)
    settingsTab(true)

    const darkLabel = $$('.modal label').find((node) => node.textContent.includes('深色模式'))
    assert(darkLabel, '设置里要有深色模式开关')
    click(darkLabel.querySelector('input[type="checkbox"]'))
    await settle(1)
    assertEq(c.names().join('|'), 'onToggleTheme', '主题开关只发切换意图')

    const language = $$('.modal select').find((node) => [...node.options].some((option) => option.value === 'en'))
    assert(language, '设置里要有语言选择器')
    await act(async () => {
      language.value = 'en'
      language.dispatchEvent(new Event('change', { bubbles: true }))
      await new Promise((resolve) => setTimeout(resolve, 30))
    })
    assertEq(i18n.language, 'en', '语言选择器真的切到 en')
    assert($$('.modal label').some((node) => node.textContent.includes('Dark Mode')),
      '设置界面文案跟着语言走')
  } finally {
    await act(async () => { await i18n.changeLanguage('zh') })
  }
  await settle(1)
  assertEq(i18n.language, 'zh', '场景收尾必须把语言恢复成 zh')
}

// ============================================================
// S02 实际 App 场景：六组意图交给 App 的 handler 之后的真实后果
// 上面的薄夹具只证明「区块发了什么意图」；这里用真实 App 证明状态后果
// （清选中 / 清目标库 / 预填表单 / 打开设置），不复制 App 的算法。
// ============================================================
const symbolRequests = () => net.calls.filter((c) => c.path === '/api/symbols')

/// 点类型行：先清掉详情，再按稳定 ID 过滤（行名与符号自己的展示名故意不同）
async function appTypeRowClearsInspectorAndFiltersById() {
  const renamed = { ...clone(SYMBOL), id: 'sym-renamed', name: 'K_RENAMED', type: '继电器旧名' }
  const decoy = { ...clone(SYMBOL), id: 'sym-decoy', name: 'K_DECOY', type_id: 'custom:other', type: '继电器' }
  // 配置里这个类型叫「继电器X」：按展示名筛选不可能命中，按稳定 ID 才能
  stubPath('/api/component-types',
    () => ({ status: 200, payload: [{ ...clone(RELAY), name: '继电器X' }, clone(RESISTOR)] }))
  stubPath('/api/symbols', () => ({ status: 200, payload: [renamed, decoy] }))
  await mountApp()

  click(symbolCard('K_RENAMED'))
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_RENAMED', 4000, '先选中 K_RENAMED，详情已打开')
  expandTypeFilter()
  await settle(1)
  const row = sidebarTypeRows().find((el) => (el.textContent || '').includes('继电器X'))
  assert(row, '夹具无效：类型行按配置里的名字显示')

  click(row)
  await waitUntil(() => !$('.inspector h4'), 4000, '点类型行清掉选中，详情回到空态')
  assert($('.inspector-empty'), '清掉选中后 Inspector 显示空态')
  assert(row.classList.contains('active'), '点过的类型行进入筛选态')
  assert(symbolCard('K_RENAMED'), '按稳定 ID 命中 type_id 匹配的符号（展示名与行名不同）')
  assertEq(symbolCard('K_DECOY'), undefined, '展示名相同、ID 不同的干扰符号不能命中')
}

/// 「全部」：保留关键词、清掉当前符号库/类型筛选/问题筛选/选中，只正常刷新一次
async function appAllKeepsKeywordAndClearsLibrary() {
  await mountApp()
  const search = $('.toolbar input')
  assert(search, '夹具无效：Toolbar 没有搜索框')

  // 先进入「有目标库 + 有关键词 + 有类型筛选 + 有问题筛选 + 有选中」的状态
  click(sidebarLibraryRows()[0])
  await waitUntil(() => (symbolRequests().at(-1)?.search || '').includes('library=lib-type-test'), 4000,
    '选中符号库后按库查询')
  setInput(search, 'K_')
  await waitUntil(() => (symbolRequests().at(-1)?.search || '').includes('q=K_'), 4000, '输入关键词后按关键词查询')

  expandTypeFilter()
  await settle(1)
  const relayRow = sidebarTypeRows().find((el) => (el.textContent || '').includes('继电器'))
  assert(relayRow, '夹具无效：类型筛选里没有继电器行')
  click(relayRow)
  await settle(1)
  click(symbolCard('K_TEST'))
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, 'Inspector 显示选中的 K_TEST')
  click(sidebarNavItem('问题'))
  await settle(1)
  assert(sidebarNavItem('问题').classList.contains('active'), '问题筛选已打开（「全部」要清掉它）')

  const before = symbolRequests().length
  click(sidebarNavItem('全部元件'))
  await settle(3)

  assertEq(symbolRequests().length - before, 1, '「全部」只正常刷新一次（原重复 clear 已合并，不重复发 GET）')
  const last = symbolRequests().at(-1)
  assert(last.search.includes('q=K_'), `「全部」保留关键词：${last.search}`)
  assert(!last.search.includes('library='), `「全部」清掉当前符号库：${last.search}`)
  assertEq(search.value, 'K_', '关键词仍留在输入框里')
  assertEq($('.inspector h4'), null, '选中被清掉，详情回到空态')
  assert(!sidebarNavItem('问题').classList.contains('active'), '问题筛选被清掉')
  assert(sidebarNavItem('全部元件').classList.contains('active'), '「全部」回到高亮（类型筛选也清掉了）')
  assertEq($$('.symbol-card').length, 2, '筛选全部清掉后两张卡片都回来')
}

/// 「问题」/「匹配」：切换问题筛选、真的发起自动匹配，两者都要清掉选中
async function appIssuesAndAutoMatchClearSelection() {
  await mountApp()
  click(symbolCard('K_TEST'))
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, 'Inspector 显示选中的 K_TEST')

  click(sidebarNavItem('问题'))
  await settle(1)
  assertEq($('.inspector h4'), null, '「问题」清掉选中')
  assert(sidebarNavItem('问题').classList.contains('active'), '「问题」打开了问题筛选')

  click(symbolCard('K_TEST'))
  await waitUntil(() => $('.inspector h4')?.textContent === 'K_TEST', 4000, '重新选中 K_TEST')
  const matchesBefore = countCalls('/api/automatch')
  click(sidebarNavItem('匹配'))
  await waitUntil(() => countCalls('/api/automatch') > matchesBefore, 4000, '「匹配」真的发起了自动匹配')
  assertEq($('.inspector h4'), null, '「匹配」清掉选中')
}

/// 自动匹配是全量启发式计算：首屏不能无条件发起，只有用户点「匹配」才发
async function appAutoMatchIsLazyOnMount() {
  await mountApp()
  assertEq(countCalls('/api/automatch'), 0, '首屏不自动发起自动匹配')
  click(sidebarNavItem('匹配'))
  await waitUntil(() => countCalls('/api/automatch') === 1, 4000, '点击「匹配」才发起一次自动匹配')
}

/// 设置入口：先读配置，再开弹窗（顺序与语义都在 App 的 handler 里）
async function appSettingsEntryLoadsConfigThenOpens() {
  await mountApp()
  const before = countCalls('/api/config')
  click(activityBarItem('设置'))
  await waitUntil(() => $('.settings-page'), 4000, '设置中心页打开')
  assertEq(countCalls('/api/config'), before + 1, '打开设置前先读一次配置')
  const close = $('[data-center-tab-close="kf:settings"]')
  assert(close, '设置页签要有可关闭入口')
  click(close)
  await waitUntil(() => !$('.settings-page'), 4000, '关闭设置页签')
}

// ============================================================
// 场景 runner：每条独立、finally 收尾（真实 unmount + 放行全部挂起 gate + 还原 fetch）
// ============================================================
// U04：固定同一份类型表，只改变 GETsymbols 里的实际 type_id；不重新选卡片。
const retyped = symbol => ({ ...clone(symbol), type_id: 'Resistor', type: 'Resistor' })
async function selectedRefreshSetup() {
  await mountApp()
  click(symbolCard(SYMBOL.name))
  await waitUntil(() => $('.inspector-type')?.textContent === RELAY.name, 4000, '初始已选类型')
}
async function selectedRefreshPublishesNewType() {
  await selectedRefreshSetup()
  respondOnce('/api/symbols', () => ({ status: 200, payload: [retyped(SYMBOL), clone(LEGACY_SYMBOL)] }), 'GET')
  setInput($('.toolbar input'), 'refresh-A')
  await waitUntil(() => symbolCard(SYMBOL.name)?.querySelector('.type-tag')?.textContent === RESISTOR.name,
    4000, '卡片已使用新列表里的实际类型')
  expandTypeFilter()
  assert(sidebarTypeRows().some(row => row.textContent.includes(RESISTOR.name)), 'Sidebar 已按新类型归组')
  assertEq($('.inspector-type').textContent, RESISTOR.name, '同 UUID 刷新后 Inspector 应同步最新 type_id')
  assertEq($('.inspector-icon').textContent, RESISTOR.icon, '同 UUID 刷新后 Inspector 图标同步')
  assertEq(getComputedStyle($('.inspector-icon')).color, canonicalColor(RESISTOR.color), 'Inspector 颜色同步')
  assert(symbolCard(SYMBOL.name).classList.contains('selected'), 'A 仍被选中')
  assertEq($('.inspector h4').textContent, SYMBOL.name, '选择标题不变')
}
async function selectedRefreshKeepsLatestSelection() {
  await selectedRefreshSetup()
  const pending = holdNext('/api/symbols', 'GET')
  setInput($('.toolbar input'), 'refresh-B')
  await waitUntil(() => pending.call, 4000, '新 GET 已挂起')
  click(symbolCard(LEGACY_SYMBOL.name))
  await waitUntil(() => $('.inspector h4')?.textContent === LEGACY_SYMBOL.name, 4000, '等待期间真实选 B')
  pending.resolve([retyped(SYMBOL), retyped(LEGACY_SYMBOL)])
  await waitUntil(() => symbolCard(LEGACY_SYMBOL.name)?.querySelector('.type-tag')?.textContent === RESISTOR.name,
    4000, 'B 新卡片已发布')
  assertEq($('.inspector h4').textContent, LEGACY_SYMBOL.name, '响应不能选回捕获的 A')
  assertEq($('.inspector-type').textContent, RESISTOR.name, '当前 B 应获得自己最新类型')
  assert(symbolCard(LEGACY_SYMBOL.name).classList.contains('selected'), '高亮保持 B')
}
async function selectedRefreshDoesNotRestoreCleared() {
  await selectedRefreshSetup()
  const pending = holdNext('/api/symbols', 'GET')
  setInput($('.toolbar input'), 'refresh-clear')
  await waitUntil(() => pending.call, 4000, '清选前 GET 已挂起')
  click(sidebarNavItem('问题'))
  assert($('.inspector-empty'), '真实导航已经清选')
  pending.resolve([retyped(SYMBOL), clone(LEGACY_SYMBOL)])
  await settle(3)
  assert($('.inspector-empty'), '刷新完成不能恢复旧选择')
}
async function selectedRefreshFailurePreservesSelection() {
  await selectedRefreshSetup()
  respondOnce('/api/symbols', () => ({ status: 503, payload: { error: 'selection-refresh-failed' } }), 'GET')
  setInput($('.toolbar input'), 'refresh-failed')
  await waitUntil(() => $('.statusbar')?.textContent.includes('selection-refresh-failed'), 4000, 'GET 失败已显示')
  assertEq($('.inspector h4').textContent, SYMBOL.name, '失败保持选择')
  assertEq($('.inspector-type').textContent, RELAY.name, '失败保持有效旧对象')
}
async function selectedRefreshIgnoresOlderResult() {
  await selectedRefreshSetup()
  const old = holdNext('/api/symbols', 'GET')
  setInput($('.toolbar input'), 'old')
  await waitUntil(() => old.call, 4000, '旧 GET 已挂起')
  respondOnce('/api/symbols', () => ({ status: 200, payload: [retyped(SYMBOL), clone(LEGACY_SYMBOL)] }), 'GET')
  setInput($('.toolbar input'), 'new')
  await waitUntil(() => symbolCard(SYMBOL.name)?.querySelector('.type-tag')?.textContent === RESISTOR.name, 4000, '新 GET 已发布')
  old.resolve([clone(SYMBOL), clone(LEGACY_SYMBOL)])
  await settle(3)
  assertEq(symbolCard(SYMBOL.name).querySelector('.type-tag').textContent, RESISTOR.name, '旧 GET 不回滚列表')
  assertEq($('.inspector-type').textContent, RESISTOR.name, '旧 GET 不回滚详情')
}

// ---- 设置中的手动导入：真实组件 + 延迟/失败响应，不读取用户库 ----
const componentLibraryPath = '/api/component-libraries'
const importButtons = () => $$('.modal button').filter((button) =>
  /保存并导入|^重新导入$|^导入中…$|Save.*Import|Re-import|Importing…/.test(button.textContent.trim()))

function mountImportSettings() {
  const calls = { close: 0, refresh: 0, status: 0, libraries: 0, toasts: [] }
  mount(<SettingsModal settings={{}} setSettings={() => {}} compLibraries={[]}
    loadCompLibraries={() => { calls.libraries += 1 }}
    onClose={() => { calls.close += 1 }} refreshSymbols={() => { calls.refresh += 1 }}
    loadStatus={() => { calls.status += 1 }} toastMsg={(message) => { calls.toasts.push(message) }}
    showConfirm={async () => false} onRequestDelete={() => {}} />)
  return calls
}

function settingsTab(utils) {
  const tab = $$('.modal div').find((node) => !node.querySelector('div') &&
    node.textContent.trim().startsWith(utils ? '🔧' : '📁'))
  assert(tab, '设置页签必须存在')
  click(tab)
}

async function settingsImportIsBusyAndShared() {
  const request = holdNext(componentLibraryPath, 'POST')
  const calls = mountImportSettings()
  const button = importButtons()[0]
  // 同一批事件还没重绘，disabled 属性来不及生效；同步 ref 也必须挡住第二次。
  act(() => {
    button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  assertEq(countPosts(componentLibraryPath), 1, '连续点击只允许一个导入请求')
  assert(button.disabled, '路径页导入按钮必须禁用')
  assert($('[role="status"]')?.textContent.includes('导入'), '慢导入期间应持续显示忙态')
  settingsTab(true)
  const other = importButtons()[0]
  assert(other.disabled, '工具页重新导入入口与路径页共享忙态')
  click(other)
  assertEq(countPosts(componentLibraryPath), 1, '切换页签不能再发一次导入')
  assertEq(calls.refresh, 0, '响应之前不能按成功刷新列表')
  request.resolve({ ok: true, imported_symbols: 3, imported_footprints: 2 })
  await settle(4)
  assertEq(calls.close, 1, '成功只关闭一次')
  assertEq(calls.refresh, 1, '成功只调用一次既有列表刷新')
  assertEq(calls.status, 1, '成功只刷新一次统计')
}

async function settingsImportFailureRecovers(kind) {
  const reason = kind === 'http' ? '封装库路径不能使用 .kicad_sym 文件' : '测试导入失败'
  if (kind === 'network') respondOnce(componentLibraryPath, () => { throw new Error('offline') }, 'POST')
  else if (kind === 'json') respondOnce(componentLibraryPath, () => Promise.resolve({
    ok: true, status: 200, json: async () => { throw new SyntaxError('broken JSON') },
  }), 'POST')
  else if (kind === 'shape') respondOnce(componentLibraryPath, () => ({ payload: { ok: 'true' } }), 'POST')
  else respondOnce(componentLibraryPath, () => ({ status: kind === 'http' ? 400 : 200,
    payload: { ok: false, error: reason } }), 'POST')
  const calls = mountImportSettings()
  click(importButtons()[0])
  await settle(4)
  assert($('[role="alert"]')?.textContent.trim(), `${kind} 失败必须在设置窗口持续显示错误`)
  if (kind === 'http' || kind === 'rejected')
    assert($('[role="alert"]').textContent.includes(reason), '后端错误细节应保留')
  assertEq(calls.close, 0, '失败不能关闭设置窗口')
  assertEq(calls.refresh, 0, '失败不能刷新符号列表冒充成功')
  assertEq(calls.status, 0, '失败不能刷新统计冒充成功')
  assert(!importButtons()[0].disabled, '失败后恢复导入入口')
  const retry = holdNext(componentLibraryPath, 'POST')
  click(importButtons()[0])
  assertEq(countPosts(componentLibraryPath), 2, '错误恢复后可以明确重试')
  assert(!$('[role="alert"]'), '新操作清除上一轮错误')
  retry.resolve({ ok: true, imported_symbols: 0, imported_footprints: 0 })
  await settle(4)
  assertEq(calls.refresh, 1, '成功重试只刷新一次列表')
}

async function settingsImportCanCloseWhilePending() {
  const request = holdNext(componentLibraryPath, 'POST')
  const calls = mountImportSettings()
  click(importButtons()[0])
  click($('.modal-close'))
  pressEscape()
  click($('.modal-overlay'))
  assertEq(calls.close, 3, '后台导入期间仍可通过关闭按钮、Escape、遮罩关闭设置')
  unmount()
  request.resolve({ ok: false, error: '拒绝导入' })
  await settle(3)
  assertEq(calls.close, 3, '卸载后的迟到响应不能再关闭之后打开的窗口')
  assertEq(calls.refresh, 0, '卸载后的失败响应不能按成功刷新')
}

async function settingsImportSuccessAfterCloseRefreshes() {
  const request = holdNext(componentLibraryPath, 'POST')
  const old = mountImportSettings()
  click(importButtons()[0])
  click($('.modal-close'))
  const current = mountImportSettings()
  request.resolve({ ok: true, imported_symbols: 1, imported_footprints: 0 })
  await settle(4)
  assertEq(old.refresh, 1, '关闭后完成的后台导入仍刷新当前列表')
  assertEq(old.status, 1, '关闭后完成的后台导入仍刷新统计')
  assertEq(old.close, 1, '迟到成功不能再关闭设置')
  assertEq(current.close, 0, '新打开的设置不能被旧回包关闭')
  assert($('.modal'), '新设置窗口保持打开')
}

async function settingsSavePathErrorKeepsDraft() {
  const calls = mountImportSettings()
  click($$('.modal button').find((node) => node.textContent.includes('添加元件库')))
  setInput($('.modal input'), '测试元件库')
  const inputs = $$('.modal input')
  setInput(inputs[1], 'C:/test/symbols.kicad_sym')
  setInput(inputs[2], 'C:/test/wrong.kicad_sym')
  respondOnce(componentLibraryPath, () => ({ status: 400, payload: {
    ok: false, error: '封装库路径不能使用 .kicad_sym 符号库文件',
    error_code: 'invalid_library_path', field: 'footprint_path',
  } }), 'POST')
  click($$('.modal button').find((node) => node.textContent.trim() === '+ 添加'))
  await settle(4)
  assert($('[role="alert"]')?.textContent.includes('.kicad_sym'), '配置路径错误必须持续可见')
  assert($('[role="alert"]').textContent.includes('.pretty'), '路径错误提示应说明要填写封装库目录')
  assert($('[role="alert"]').textContent.includes('符号库路径'), '路径错误提示应说明符号文件应填哪个字段')
  assert($('[role="alert"]').textContent.includes('封装库路径不能使用 .kicad_sym 符号库文件'), '本地化提示不能吞掉宿主具体错误')
  assertEq($$('.modal input')[2]?.value, 'C:/test/wrong.kicad_sym', '保存失败保留路径草稿')
  assertEq(calls.libraries, 1, '保存失败不能加载列表冒充成功')
  assertEq(calls.close, 0, '保存失败不能关闭设置窗口')
  setInput($$('.modal input')[2], 'C:/test/footprints')
  respondOnce(componentLibraryPath, () => ({ payload: { ok: true } }), 'POST')
  click($$('.modal button').find((node) => node.textContent.trim() === '+ 添加'))
  await settle(4)
  assertEq(calls.libraries, 2, '修正路径后可保存并刷新')
  assertEq($$('.modal input').length, 0, '保存成功后才收起编辑表单')
}

async function settingsPathHintsExplainAcceptedSources() {
  try {
    for (const language of ['zh', 'en']) {
      await i18n.changeLanguage(language)
      mountImportSettings()
      const text = $('.modal').textContent
      assert(text.includes(i18n.t('settings.symbolPathHint')), '符号源支持目录或单文件的提示应可见')
      assert(text.includes(i18n.t('settings.footprintPathHint')), '封装源只接受目录的提示应可见')
      assert(text.includes('.pretty') && text.includes('.kicad_sym'), '两种路径的具体格式必须在设置中可见')
      unmount()
    }
  } finally {
    await i18n.changeLanguage('zh')
  }
}

// ---- 导入进度：真实弹窗 + 可控 HTTP 响应，不读取用户库 ----
const importStatusPath = '/api/import-status'
const progressText = () => $('.import-progress')?.textContent || ''
const importSnapshot = (overrides = {}) => ({
  importing: true,
  progress: { phase: 'symbols_parse', completed: 0, total: 6 },
  ...overrides,
})

async function importingShowsRealFileProgress() {
  respondOnce(importStatusPath, () => ({ payload: importSnapshot() }))
  const next = holdNext(importStatusPath)
  const footprintPhase = holdNext(importStatusPath)
  mount(<ImportingModal />)
  assert(progressText().includes('等待导入进度'), '首次响应前等待任务进度')
  assert(!progressText().includes('0 / 0'), '未知进度不能伪造零值')
  await waitUntil(() => progressText().includes('0 / 6'), 3000, '首份任务文件进度')
  assert(progressText().includes('解析符号库'), '解析阶段必须可见')
  assert(!progressText().includes('数据库'), '弹窗不显示全库统计')
  await waitUntil(() => next.call, 3000, '下一次串行轮询')
  next.resolve(importSnapshot({ progress: { phase: 'symbols_parse', completed: 3, total: 6 } }))
  await waitUntil(() => progressText().includes('3 / 6'), 2000, '解析文件进度递增')
  await waitUntil(() => footprintPhase.call, 3000, '切换封装解析阶段')
  footprintPhase.resolve(importSnapshot({ progress: { phase: 'footprints_parse', completed: 2, total: 9 } }))
  await waitUntil(() => progressText().includes('解析封装'), 2000, '封装解析阶段可见')
  assert(progressText().includes('2 / 9'), '封装解析显示本阶段文件进度')
  assert(!progressText().includes('数据库'), '阶段切换后仍仅显示任务进度')
}

async function importingRetainsOmittedProgress() {
  respondOnce(importStatusPath, () => ({ payload: importSnapshot({
    progress: { phase: 'symbols_parse', completed: 3, total: 6 },
  }) }))
  respondOnce(importStatusPath, () => ({ payload: {
    importing: true, busy: true, counts_available: false, counts_stale: true,
  } }))
  const last = holdNext(importStatusPath)
  mount(<ImportingModal />)
  await waitUntil(() => progressText().includes('3 / 6'), 3000, '初始有效文件进度')
  await waitUntil(() => countCalls(importStatusPath) >= 2, 3000, '省略任务进度的兼容响应')
  await settle()
  assert(progressText().includes('3 / 6'), '省略进度时保留最后有效文件数')
  assert(progressText().includes('解析符号库'), '省略进度时保留最后有效阶段')
  assert(!progressText().includes('数据库'), '旧响应DB字段不进入任务界面')
  await waitUntil(() => last.call, 3000, '恢复读取请求')
  last.resolve(importSnapshot({ progress: { phase: 'symbols_parse', completed: 4, total: 6 } }))
  await waitUntil(() => progressText().includes('4 / 6'), 2000, '新快照正常推进文件进度')
}

async function importingErrorsRetryWithoutCompleting() {
  let done = 0
  respondOnce(importStatusPath, () => { throw new Error('synthetic network error') })
  respondOnce(importStatusPath, () => ({ status: 500, payload: { importing: false } }))
  respondOnce(importStatusPath, () => ({ payload: { error: 'synthetic failure' } }))
  respondOnce(importStatusPath, () => ({ payload: { importing: 'false' } }))
  respondOnce(importStatusPath, () => ({ payload: importSnapshot({
    progress: { phase: 'symbols_parse', completed: -1, total: 6 },
  }) }))
  respondOnce(importStatusPath, () => ({ payload: importSnapshot({
    progress: { phase: 'symbols_parse', completed: '3', total: 6 },
  }) }))
  const recovery = holdNext(importStatusPath)
  mount(<ImportingModal onDone={() => { done += 1 }} />)
  await waitUntil(() => $('[role="alert"]'), 3000, '首次网络失败可见')
  assert(progressText().includes('等待导入进度'), '首次失败不伪造文件进度')
  await waitUntil(() => countCalls(importStatusPath) >= 2, 3000, 'HTTP500 自动重试')
  await settle()
  assertEq(done, 0, 'HTTP500 不能被 JSON 中 importing:false 误判完成')
  for (let n = 3; n <= 6; n += 1) {
    await waitUntil(() => countCalls(importStatusPath) >= n, 3000, `无效响应 ${n} 自动重试`)
    await settle()
    assertEq(done, 0, '无效响应不能回调完成')
    assert($('[role="alert"]'), '无效响应保持可见错误')
  }
  await waitUntil(() => recovery.call, 3000, '错误后仍然轮询')
  recovery.resolve(importSnapshot({ progress: { phase: 'symbols_parse', completed: 3, total: 6 } }))
  await waitUntil(() => progressText().includes('3 / 6'), 2000, '恢复后的有效响应')
  assert(!$('[role="alert"]'), '恢复后清除错误提示')
  assertEq(done, 0, '仍在导入，不提前完成')
}

async function importingFailureKeepsLastProgress() {
  respondOnce(importStatusPath, () => ({ payload: importSnapshot({
    progress: { phase: 'symbols_parse', completed: 3, total: 6 },
  }) }))
  respondOnce(importStatusPath, () => { throw new Error('synthetic second read failed') })
  const pending = holdNext(importStatusPath)
  mount(<ImportingModal />)
  await waitUntil(() => progressText().includes('3 / 6'), 3000, '已显示文件进度')
  await waitUntil(() => $('[role="alert"]'), 3000, '后续读取失败可见')
  assert(progressText().includes('3 / 6'), '失败保留文件进度')
  assert(progressText().includes('解析符号库'), '失败保留当前阶段')
  await waitUntil(() => pending.call, 3000, '失败继续重试')
}

async function importingSlowRequestIsSerial() {
  let done = 0
  const pending = holdNext(importStatusPath)
  mount(<ImportingModal onDone={() => { done += 1 }} />)
  await waitUntil(() => pending.call, 3000, '请求已经发出')
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 4200)) })
  assertEq(countCalls(importStatusPath), 1, '慢请求未完成前不能重叠发出下一次请求')
  pending.resolve({ importing: false })
  await waitUntil(() => done === 1, 2000, '合法完成响应调用一次')
  assert(!$('.modal-overlay'), '完成撤掉弹窗')
}

async function importingCloseKeepsMonitoring() {
  let done = 0
  let oldDone = 0
  respondOnce(importStatusPath, () => ({ payload: importSnapshot() }))
  const completion = holdNext(importStatusPath)
  mount(<ImportingModal onDone={() => { oldDone += 1 }} />)
  await waitUntil(() => progressText().includes('0 / 6'), 3000, '初始响应')
  act(() => { root.render(<ImportingModal onDone={() => { done += 1 }} />) })
  assertEq(countCalls(importStatusPath), 1, '更换完成回调不重启查询')
  click($('.modal-close'))
  assert(!$('.modal-overlay'), '关闭只隐藏弹窗')
  await waitUntil(() => completion.call, 3000, '隐藏后继续查询')
  completion.resolve({ importing: false })
  await waitUntil(() => done === 1, 2000, '隐藏后通知应用完成')
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 2200)) })
  assertEq(done, 1, '完成回调只能触发一次')
  assertEq(oldDone, 0, '完成通知使用最新回调')
  assertEq(countCalls(importStatusPath), 2, '完成后停止轮询')
}

async function importingUnmountIgnoresInflightResponse() {
  let done = 0
  const pending = holdNext(importStatusPath)
  mount(<ImportingModal onDone={() => { done += 1 }} />)
  await waitUntil(() => pending.call, 3000, '在途请求')
  unmount()
  pending.resolve({ importing: false })
  await settle(4)
  assertEq(done, 0, '卸载后的响应不能回调完成')
  await act(async () => { await new Promise((resolve) => setTimeout(resolve, 2200)) })
  assertEq(countCalls(importStatusPath), 1, '卸载后不能重新排定请求')
}

async function importingLabelsFollowLanguage() {
  stubPath(importStatusPath, () => ({ payload: importSnapshot({
    progress: { phase: 'footprints', completed: 2, total: 5 },
  }) }))
  try {
    mount(<ImportingModal />)
    await waitUntil(() => progressText().includes('2 / 5'), 3000, '中文初始值')
    assert(progressText().includes('导入封装'), '中文阶段标签')
    const before = countCalls(importStatusPath)
    await act(async () => { await i18n.changeLanguage('en') })
    assert(progressText().includes('Importing footprints'), '英文阶段标签')
    assert(progressText().includes('2 / 5 files'), '英文文件进度标签')
    assert(!progressText().includes('Database'), '英文界面也不显示数据库统计')
    assertEq(countCalls(importStatusPath), before, '切换语言不能重启轮询')
  } finally {
    await act(async () => { await i18n.changeLanguage('zh') })
  }
}

async function importingLegacyResponseStillWorks() {
  let done = 0
  respondOnce(importStatusPath, () => ({ payload: { importing: true, symbols: 0 } }))
  const completion = holdNext(importStatusPath)
  mount(<ImportingModal onDone={() => { done += 1 }} />)
  await waitUntil(() => countCalls(importStatusPath) >= 1, 3000, '兼容旧响应的请求')
  await settle()
  assert(progressText().includes('等待导入进度'), '旧响应没有任务进度时保持等待')
  assert(!progressText().includes('数据库'), '不采用旧响应的全库统计')
  assert(!$('[role="alert"]'), '旧统计响应仍兼容')
  await waitUntil(() => completion.call, 3000, '旧接口第二次读取')
  completion.resolve({ importing: false, symbols: 'ignored legacy count', footprints: -1,
    counts_available: 'ignored legacy flag' })
  await waitUntil(() => done === 1, 2000, '旧响应正常完成')
}

async function importingUnknownTotalIsNotZeroTotal() {
  stubPath(importStatusPath, () => ({ payload: {
    importing: true,
    progress: { phase: 'scanning', completed: 7, total: 0 },
  } }))
  mount(<ImportingModal />)
  await waitUntil(() => progressText().includes('已处理 7 个文件'), 3000, '扫描未知总量仍显示已处理文件数')
  assert(progressText().includes('总数尚未确定'), '明确区分未知总量与零文件')
  assert(!progressText().includes('/ 0'), '不能把未知总量写成/0')
  assert(!progressText().includes('数据库'), '未知总量也只显示任务进度')
  assert(!$('[role="alert"]'), '扫描已处理数大于未知总量0并非非法响应')
}

const results = []

async function finish() {
  const cleanup = []
  try { unmount() } catch (cause) { cleanup.push(`unmount: ${cause?.message || cause}`) }
  try { releaseGates() } catch (cause) { cleanup.push(`release gates: ${cause?.message || cause}`) }
  try { setLoadingCallback(null) } catch (cause) { cleanup.push(`reset loading callback: ${cause?.message || cause}`) }
  try { window.fetch = realFetch } catch (cause) { cleanup.push(`restore fetch: ${cause?.message || cause}`) }
  try { await settle(3) } catch (cause) { cleanup.push(`settle: ${cause?.message || cause}`) }
  return cleanup
}

function resetBackend() {
  backend.types = [clone(RELAY), clone(RESISTOR)]
  backend.pkgTypes = clone(PKG_TYPES)
  backend.addSeq = 0
}

async function scenario(name, fn) {
  net.calls.length = 0
  net.unknown.length = 0
  net.handlers.clear()
  net.overrides.clear()
  net.pending.length = 0
  resetBackend()
  installFetch()
  let error = null
  try {
    await fn()
  } catch (cause) {
    error = String(cause?.message || cause)   // 业务断言照实记为红，不吞
  }
  for (const note of await finish()) {
    error = error ? `${error} ｜ 收尾：${note}` : `收尾失败：${note}`
  }
  const gaps = [...new Set(net.unknown)]
  if (gaps.length) {
    error = `${error ? `${error} ｜ ` : ''}夹具缺口（未 mock 的接口已显式拒绝）：${gaps.join(', ')}`
  }
  results.push(error ? { name, ok: false, error } : { name, ok: true })
}

// ============================================================
// 入口
// ============================================================
const harness = {
  async run(mode, { onlyImportProgress = false, onlySettingsImport = false } = {}) {
    if (mode !== 'types') throw new Error(`type-management-browser 只支持 mode=types，收到 ${String(mode)}`)
    results.length = 0
    await i18n.changeLanguage('zh')   // 选择器按中文文案定位，不随系统语言漂移
    if (!onlyImportProgress) {
      await scenario('设置导入：同批双击与跨页入口共享单次导入和持续忙态', settingsImportIsBusyAndShared)
      for (const kind of ['network', 'http', 'json', 'shape', 'rejected'])
        await scenario(`设置导入：${kind} 失败持续显示原因并可重试`, () => settingsImportFailureRecovers(kind))
      await scenario('设置导入：后台任务等待时可关闭，卸载后忽略迟到响应', settingsImportCanCloseWhilePending)
      await scenario('设置导入：关闭后后台成功刷新当前列表，不关闭新窗口', settingsImportSuccessAfterCloseRefreshes)
      await scenario('设置导入：保存路径错误保留草稿，修正后可重试', settingsSavePathErrorKeepsDraft)
      await scenario('设置导入：中英文均显示符号单文件与封装目录的路径提示', settingsPathHintsExplainAcceptedSources)
    }
    if (onlySettingsImport) return { mode: 'types', results }
    await scenario('导入进度：初次等待、解析文件数递增及符号到封装阶段切换', importingShowsRealFileProgress)
    await scenario('导入进度：省略任务进度保留阶段和文件数，忽略旧数据库字段', importingRetainsOmittedProgress)
    await scenario('导入进度：网络/HTTP500/错误结构不误报完成，可自动恢复', importingErrorsRetryWithoutCompleting)
    await scenario('导入进度：读取失败保留最后一次有效任务进度', importingFailureKeepsLastProgress)
    await scenario('导入进度：慢请求不重叠', importingSlowRequestIsSerial)
    await scenario('导入进度：关闭后继续监控，完成只通知一次', importingCloseKeepsMonitoring)
    await scenario('导入进度：卸载后忽略在途响应，不再请求', importingUnmountIgnoresInflightResponse)
    await scenario('导入进度：切换中英文保留文件进度且不重启请求', importingLabelsFollowLanguage)
    await scenario('导入进度：兼容旧接口且忽略全库统计', importingLegacyResponseStillWorks)
    await scenario('导入进度：未知扫描总量不显示/0，只显示已处理文件数', importingUnknownTotalIsNotZeroTotal)
    if (onlyImportProgress) return { mode: 'types', results }
    await scenario('绿控制：真实 App 挂载并显示 K_TEST 卡片与 Inspector', controlAppMounts)
    await scenario('Inspector：搜索浮层 Escape 关闭并把焦点还给字段', inspectorPopoverClosesOnEscape)
    await scenario('红：Sidebar 类型行应使用类型配置图标', sidebarUsesConfiguredIcon)
    await scenario('红：Inspector 图标应使用类型配置图标', inspectorUsesConfiguredIcon)
    await scenario('红：App 启动应加载元件类型表', appLoadsComponentTypesOnStart)
    await scenario('绿控制：真实 TypeModal 显示条目 icon 并可按 onClose 卸载', typeModalShowsConfiguredIcon)
    await scenario('红：类型条目应有可访问的编辑入口', typeModalHasEditableEntry)
    await scenario('类型管理：新增带 name/icon/color，使用后端生成的 ID', addTypeCarriesFields)
    await scenario('类型管理：改名按稳定 ID 提交，Inspector/Sidebar 实时更新且筛选仍命中', renameKeepsStableId)
    await scenario('类型管理：颜色大小写、多字节图标、清空图标与颜色', colorAndIconEdits)
    await scenario('类型管理：空名称/超长图标提交前挡住，重名由服务端拒绝并保留草稿', editorValidation)
    await scenario('类型管理：保存失败保留草稿与旧显示并展示服务端原因', saveFailureKeepsDraft)
    await scenario('类型管理：双击保存只发一次请求', doubleSubmitSingleRequest)
    await scenario('类型管理：已保存但刷新失败只提示并重试读取，不再写一次', savedButRefreshFailed)
    await scenario('类型管理：读取失败保留旧快照并可重试', loadFailureKeepsSnapshot)
    await scenario('类型管理：迟到的慢读取不能覆盖保存后的新快照', staleReadCannotOverwrite)
    await scenario('类型管理：取消不发请求，草稿不串到别的条目/下一次打开', draftsDoNotLeak)
    await scenario('类型管理：内置可改展示名但不可删，被引用类型拒删并回报引用数', deleteProtection)
    await scenario('类型管理：未知历史 ID 原样显示且用通用占位', unknownHistoricalId)
    await scenario('封装类型：新增/删除 smoke 未退化且不夹带元件类型字段', packageTypesSmoke)
    await scenario('类型管理：历史非 hex 颜色/超长图标不阻挡「只改名称」', legacyNameOnlyEditKeepsFields)
    await scenario('类型管理：历史颜色可显式清空/换合法值，超长图标可显式清空', legacyValueExplicitFixes)
    await scenario('类型管理：旧色+超长图标同时存在时仍能只修一个字段', legacyMixedFieldsCanBeFixedOneAtATime)
    await scenario('类型管理：已保存但刷新失败后所有写入入口冻结，只允许重读', staleFreezesWriteEntries)
    await scenario('真实 hook：待刷新期间直接 mutate 也不能绕过写保护', staleFreezesHookMutations)
    await scenario('真实 hook：首次读取失败不得当作空表盲写', failedInitialLoadDoesNotAllowBlindWrites)
    await scenario('真实 hook：被取代的旧读不能把已刷新的快照标成待刷新', supersededOldReadDoesNotMarkStale)
    await scenario('真实 hook：较新的读还没落地时不得提前解冻写入', newerPendingReadKeepsWriteProtection)
    await scenario('类型管理：损坏列表 [null] 保留旧快照并明确报错',
      () => corruptListCase('[null]', [null]))
    await scenario('类型管理：损坏列表 id 不是字符串时保留旧快照并明确报错',
      () => corruptListCase('id 不是字符串', [{ id: 42, name: '坏ID' }, clone(RELAY)]))
    await scenario('类型管理：损坏列表重复 id 时保留旧快照并明确报错',
      () => corruptListCase('重复 id', [clone(RELAY), { ...clone(RELAY), name: '冒名' }]))
    await scenario('类型管理：应用层 ok:false 的真实原因要透到界面', appLevelFailureKeepsReason)
    await scenario('类型管理：写入在飞期间关闭/重开不丢新草稿、不重复写入', closingDuringSaveDoesNotKillNewDraft)
    await scenario('类型管理：图标按纯文本渲染，不走 innerHTML', iconStaysPlainText)
    await scenario('Sidebar：工作库为空列全部库，选定工作库只列归属匹配的库，空归属给空状态', sidebarScopesLibrariesByWorkspace)
    await scenario('Sidebar：类型行按稳定 ID 筛选，图标/名称/颜色取自共享配置，历史 ID 原样显示', sidebarTypeFilterUsesStableIds)
    await scenario('Sidebar：类型表更新即时生效，展开状态不被类型表/工作库切换重置', sidebarTypeMetadataUpdatesLive)
    await scenario('Sidebar：库行点击选中，删除只请求删除且不触发选中', sidebarLibraryRowSelectAndDelete)
    await scenario('Sidebar：全部/问题/匹配各发一个意图，可选 onSelectAll 与徽标', sidebarNavigationIntents)
    await scenario('活动栏：设置入口只发打开意图', sidebarSettingsEntry)
    await scenario('Sidebar：没有工作库/插件/符号库/类型时正常渲染', sidebarEmptyDataRenders)
    await scenario('设置：语言走真实 i18n、深色模式按切换意图，收尾恢复 zh', settingsLanguageAndTheme)
    await scenario('真实 App：点类型行清掉详情并按稳定 ID 过滤', appTypeRowClearsInspectorAndFiltersById)
    await scenario('真实 App：全部保留关键词、清掉目标库/筛选/选中且只刷新一次', appAllKeepsKeywordAndClearsLibrary)
    await scenario('真实 App：问题/匹配都清掉选中，匹配真的发起自动匹配', appIssuesAndAutoMatchClearSelection)
    await scenario('真实 App：自动匹配不在首屏发起，点击「匹配」才请求', appAutoMatchIsLazyOnMount)
    await scenario('真实 App：设置入口先读配置再开弹窗', appSettingsEntryLoadsConfigThenOpens)
    await scenario('U04 真实 App：同 UUID 实际类型刷新同步卡片、Sidebar 和 Inspector', selectedRefreshPublishesNewType)
    await scenario('U04 真实 App：等待期间选 B，刷新保留 B 并更新 B 的类型', selectedRefreshKeepsLatestSelection)
    await scenario('U04 真实 App：等待期间清选，刷新不能恢复 A', selectedRefreshDoesNotRestoreCleared)
    await scenario('U04 真实 App：GET 失败保持有效选择', selectedRefreshFailurePreservesSelection)
    await scenario('U04 真实 App：逆序 GET 只同步已发布的新列表', selectedRefreshIgnoresOlderResult)
    unmount()
    return { mode: 'types', results }
  },
}
window.__harness = harness
window.__ready = true
