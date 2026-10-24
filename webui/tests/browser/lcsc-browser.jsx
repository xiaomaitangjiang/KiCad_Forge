/* eslint-disable react/only-export-components -- 测试页：组件只在本文件内挂载，无需导出 */
// LCSC 导入流程的真实组件测试页：由 run-lcsc-browser.mjs 通过 Vite + Edge(CDP)
// 打开，页面内用真实 DOM 事件（输入/点击/radio/Escape/遮罩/关闭按钮）驱动真实
// React 组件，只把 window.fetch 换成可控实现（api.js 的包装、错误分类、JSON 解析
// 全都真实执行）。
//
// 运行入口：专用 LcscImportModal 流程（`node ...run-lcsc-browser.mjs`）。
// 旧的 two_step/--legacy 兼容界面已删除。

import { act, StrictMode, useState } from 'react'
import { createRoot } from 'react-dom/client'

globalThis.IS_REACT_ACT_ENVIRONMENT = true
import i18n from '../../src/i18n'
import '../../src/App.css'
import { useRequest } from '../../src/hooks/useData'
import { api } from '../../src/api'
import { isProblemSymbol } from '../../src/lib/footprintStatus'

// ============================================================
// 可控 fetch
// ============================================================
function deferred() {
  let resolve, reject
  const promise = new Promise((res, rej) => { resolve = res; reject = rej })
  return { promise, resolve, reject }
}
const isThenable = v => !!v && typeof v.then === 'function'

const net = {
  calls: [],       // {url, method, body, respond?}
  overrides: {},   // 路径片段 → () => payload | Promise<payload>
  queue: [],       // /api/plugins/execute 的排队响应
  hold: false,     // true = 插件请求挂起，等测试 respond()
}

function makeResponse(payload, status = 200) {
  if (payload && payload.__status) status = payload.__status
  const body = payload && payload.__payload !== undefined ? payload.__payload : payload
  return { ok: status >= 200 && status < 300, status, json: async () => body }
}

// 默认：元件库列表返回真实形状的数据包（空列表会让「提交前校验」把成功场景
// 也挡掉，错误的场景要显式 override）
const WORKSPACES = [
  { id: 'pkg-ok', name: '主元件库', symbol_path: 'D:\\libs\\main\\symbols', footprint_path: 'D:\\libs\\main\\footprints', model_3d_path: '', enabled: true, sort_order: 0, locked: false, is_protected: false },
  { id: 'pkg-locked', name: '锁定库', symbol_path: 'D:\\libs\\locked\\symbols', footprint_path: 'D:\\libs\\locked\\footprints', model_3d_path: '', enabled: true, sort_order: 1, locked: true, is_protected: false },
  { id: 'pkg-paths', name: '缺路径库', symbol_path: '', footprint_path: '', model_3d_path: '', enabled: true, sort_order: 2, locked: false, is_protected: false },
]
const SYMBOL_LIBRARIES = { ok: true, can_create: true, libraries: [
  { id: 'sym-device', name: 'Device', path: 'Device.kicad_sym' },
  { id: 'sym-nested', name: 'Device', path: 'nested/Device.kicad_sym' },
] }
const DEFAULTS = [
  ['/api/symbols', () => []],
  ['/symbol-libraries', () => ({ ...SYMBOL_LIBRARIES, libraries: SYMBOL_LIBRARIES.libraries.slice() })],
  ['/api/component-libraries', () => WORKSPACES.slice()],
  ['/api/libraries', () => []],
  ['/api/status', () => ({ symbols: 0, footprints: 0 })],
  ['/api/footprints/search', () => []],
  ['/api/plugins', () => []],
]

function defaultPayload(url) {
  if (url.includes('/symbol-libraries')) {
    const exact = Object.keys(net.overrides).find(key => key.includes('/symbol-libraries') && url.includes(key))
    return exact ? net.overrides[exact]() : DEFAULTS.find(([key]) => key === '/symbol-libraries')[1]()
  }
  for (const [key, fn] of Object.entries(net.overrides)) if (url.includes(key)) return fn()
  for (const [key, fn] of DEFAULTS) if (url.includes(key)) return fn()
  return {}
}

function installFetch() {
  window.fetch = (input, init = {}) => {
    const url = typeof input === 'string' ? input : String(input?.url || input)
    const method = (init.method || 'GET').toUpperCase()
    let body = null
    try { body = init.body ? JSON.parse(init.body) : null } catch { body = init.body }
    const call = { url, method, body, at: net.calls.length }
    net.calls.push(call)

    let payload
    try { payload = defaultPayload(url) } catch (cause) {
      return Promise.reject(cause)
    }
    if (isThenable(payload)) return payload.then(p => makeResponse(p))
    return new Promise(resolve => setTimeout(() => resolve(makeResponse(payload)), 0))
  }
}

// The production plugin receives the task adapter, not the old global
// executePlugin HTTP method. Keep the same queued responses for the component
// tests while recording logical task calls locally.
const lcscApi = {
  dispose() {},
  executePlugin(pluginId, body, onTask) {
    const url = `/api/plugins/execute?id=${encodeURIComponent(pluginId)}`
    const call = { url, method: 'POST', body, at: net.calls.length }
    net.calls.push(call)
    const respond = (payload) => payload
    if (net.queue.length) {
      const next = net.queue.shift()
      if (isThenable(next)) return next.then(respond)
      return new Promise(resolve => setTimeout(() => resolve(respond(next)), 0))
    }
    if (!net.hold) {
      return new Promise(resolve => setTimeout(
        () => resolve(respond({ ok: true, plugin_result: { ok: true } })), 0))
    }
    return new Promise(resolve => {
      call.respond = (payload) => {
        const wrapper = respond(payload)
        if (onTask) onTask({ getSnapshot: () => ({}), subscribe: () => () => {}, cancel: async () => ({}) })
        resolve(wrapper)
      }
    })
  },
  compLibraries: () => api.compLibraries(),
  componentSymbolLibraries: (id) => api.componentSymbolLibraries(id),
  searchFootprints: (q, near) => api.searchFootprints(q, near),
}

// ============================================================
// DOM 事件
// ============================================================
const $ = sel => document.querySelector(sel)
const $$ = sel => [...document.querySelectorAll(sel)]
function tid(id) { return document.querySelector(`[data-testid="${id}"]`) }
function byText(sel, text) { return $$(sel).find(e => (e.textContent || '').includes(text)) }
// DOM 事件一律在 act() 里派发：真实事件驱动真实状态更新，不产生 act 警告
function setInput(node, value) {
  if (!node) throw new Error('setInput: node is null')
  const proto = node instanceof HTMLTextAreaElement ? HTMLTextAreaElement : HTMLInputElement
  act(() => {
    Object.getOwnPropertyDescriptor(proto.prototype, 'value').set.call(node, value)
    node.dispatchEvent(new Event('input', { bubbles: true }))
  })
}
function click(node) {
  if (!node) throw new Error('click: node is null')
  act(() => { node.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true })) })
}
function escape() {
  act(() => { document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
}
function enter(node) {
  act(() => { node.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })) })
}
async function settle(times = 3) {
  for (let i = 0; i < times; i += 1) {
    await act(async () => { await new Promise(r => setTimeout(r, 0)) })
  }
}

// ============================================================
// 挂载
// ============================================================
let root = null
function unmount() {
  if (root) { act(() => root.unmount()); root = null }
  const host = $('#root')
  if (host) host.innerHTML = ''
}
function mountElement(element) {
  unmount()
  root = createRoot($('#root'))
  act(() => { root.render(element) })
}

const uuids = {
  A: '11111111-1111-4111-8111-111111111111',
  B: '22222222-2222-4222-8222-222222222222',
}

// 正常候选必须显式带服务端状态：存在性判定只认 footprint_status='available'，
// 组件不替缺失状态兜底（夹具也不兜底，否则测不出「未知=未检查」这条规则）。
const live = (candidate) => ({ footprint_status: 'available', ...candidate })

const ANALYSIS = {
  ok: true,
  plugin_result: {
    ok: true,
    lcsc_id: 'C37593',
    symbol_names: ['AMS1117-3.3'],
    footprint_name: 'SOT-223-3_L6.5-W3.5-P2.30-LS7.0-BR',
    model_name: 'SOT-223',
    candidates: [
      live({ id: uuids.A, library: 'main', name: 'SOT-223', score: 0.94, pad_count: 4 }),
      live({ id: uuids.B, library: 'main', name: 'SOT-223-3', score: 0.71, pad_count: 4 }),
    ],
    warning: '',
  },
}

const PLUGIN = {
  id: 'com.kicad_forge.lcsc_import',
  name: 'LCSC Component Importer',
  version: '2.0.0',
  status: 'loaded',
  actions: [{
    id: 'import', name: 'Import LCSC', trigger: 'dialog', template: 'lcsc_import',
    analyze_action: 'analyze', protocol: 'staged-import-v1',
    schema: {
      fields: [
        { key: 'lcsc_id', label: 'LCSC Part Number', type: 'text', required: true },
        { key: 'target_component_library_id', label: 'Component Library Package', type: 'component_library_picker', required: true },
        { key: 'footprint_choice', label: 'Footprint', type: 'footprint_choice', modes: ['reuse', 'new'], required: true },
      ],
    },
    button: { show: true, location: 'sidebar', style: 'both' },
  }, {
    id: 'analyze', name: 'Analyze LCSC', trigger: 'hidden', hidden: true,
    button: { show: false },
  }],
}
// 稳定引用：rerender 不能造出新会话
const PLUGIN_ACTION = { plugin: PLUGIN, action: PLUGIN.actions[0] }

const harnessApi = {
  compLibraries: WORKSPACES.slice(),
  gates: {},          // symbols/libraries/status → deferred，用于证明刷新顺序
  refreshFail: '',    // 非空则 refreshSymbols 拒绝
  lastToast: '', closed: false,
  refreshCount: 0, refreshedSymbols: 0, loadedLibraries: 0, loadedStatus: 0,
  async loadCompLibraries() {
    harnessApi.refreshCount += 1
    let payload = null
    try { payload = await window.fetch('/api/component-libraries').then(r => r.json()) } catch { payload = null }
    // 与 useRequest 一致：失败返回 null；成功才更新 state 与传给组件的 props
    if (Array.isArray(payload)) {
      harnessApi.compLibraries = payload
      currentProps.compLibraries = payload
      rerender()
    }
    return payload
  },
  // 与真实 App 一致：refreshSymbols 是 createSymbolRefresher(useSymbols().loadSymbols)，
  // loadLibraries / loadStatus 就是 useRequest(...).load —— 成功返回本次原始结果
  // （数组 / 状态对象），失败返回 null。夹具不再返回 undefined。
  refreshSymbols() {
    harnessApi.refreshedSymbols += 1
    if (harnessApi.gates.symbols) return harnessApi.gates.symbols.promise
    if (harnessApi.refreshFail) return Promise.reject(new Error(harnessApi.refreshFail))
    return Promise.resolve([])
  },
  loadLibraries() {
    harnessApi.loadedLibraries += 1
    return harnessApi.gates.libraries ? harnessApi.gates.libraries.promise : Promise.resolve([])
  },
  loadStatus() {
    harnessApi.loadedStatus += 1
    return harnessApi.gates.status ? harnessApi.gates.status.promise : Promise.resolve({ symbols: 0, footprints: 0 })
  },
  toastMsg(msg) { harnessApi.lastToast = msg },
}

// 真实宿主接线：刷新回调不是测试替身，而是 App 里同一套 useRequest(...).load
// （失败返回 null）。用它证明「刷新失败 → 不能当成功关闭」是与真实接口契约
// 对齐的行为，而不是测试夹具自己的镜像。
function RealHost() {
  const { data: compLibs, load: loadCompLibraries } = useRequest(api.compLibraries)
  const { load: loadSymbols } = useRequest(api.symbols)
  const { load: loadLibraries } = useRequest(api.libraries)
  const { load: loadStatus } = useRequest(api.status)
  return (
    <LcscImportModal
      pluginAction={PLUGIN_ACTION}
      api={lcscApi}
      compLibraries={Array.isArray(compLibs) ? compLibs : []}
      loadCompLibraries={loadCompLibraries}
      refreshSymbols={loadSymbols}
      loadLibraries={loadLibraries}
      loadStatus={loadStatus}
      toastMsg={harnessApi.toastMsg}
      onClose={closeDialog}
      onRefresh={currentProps.onRefresh}
    />
  )
}

let currentProps = {}
function renderTree() {
  if (currentProps.mode === 'real') return <RealHost />
  return (
    <LcscImportModal
      pluginAction={PLUGIN_ACTION}
      api={lcscApi}
      compLibraries={currentProps.compLibraries}
      loadCompLibraries={harnessApi.loadCompLibraries}
      refreshSymbols={harnessApi.refreshSymbols}
      loadLibraries={harnessApi.loadLibraries}
      loadStatus={harnessApi.loadStatus}
      toastMsg={harnessApi.toastMsg}
      onClose={closeDialog}
      onRefresh={currentProps.onRefresh}
    />
  )
}
function rerender() {
  if (!root) return
  const tree = renderTree()
  // 原生驱动会话里 act 环境是关的：这时必须直接 render（异步刷新由 native.settle
  // 等），否则 React 会报 “not configured to support act(...)”，把真实问题淹掉。
  if (globalThis.IS_REACT_ACT_ENVIRONMENT === true) act(() => { root.render(tree) })
  else root.render(tree)
}
// 模拟 App 拿到新列表后传下来的 props（组件必须按最新 props 判定目标可用性）
function setCompLibraries(list) {
  harnessApi.compLibraries = list
  currentProps.compLibraries = list
  rerender()
}
// 真实关闭：卸载组件，和 App 里 setPluginAction(null) 的效果一致
function closeDialog() {
  harnessApi.closed = true
  unmount()
}

let LcscImportModal = null

async function setup(mode, { formData = {}, libraries = [], activeWorkspace = null, onRefresh } = {}) {
  unmount()
  harnessApi.closed = false
  currentProps = {
    mode,
    compLibraries: harnessApi.compLibraries,
    formData: { ...formData },
    libraries,
    activeWorkspace,
    onRefresh,
    setFormData: (v) => {
      currentProps.formData = typeof v === 'function' ? v(currentProps.formData) : v
      rerender()   // 旧组件读的是 props；不 rerender 它永远看到旧值
    },
  }
  if (!LcscImportModal) {
    LcscImportModal = (await import('../../../plugins/lcsc_import/frontend/src/LcscImportModal.jsx')).default
  }
  root = createRoot($('#root'))
  rerender()
  await settle()
  if (!$('.modal')) {
    throw new Error(`dialog did not render (mode=${mode}): ${($('#root').innerHTML || '').slice(0, 200)}`)
  }
}

function nextFrame() {
  return new Promise(resolve => {
    let done = false
    const finish = () => { if (!done) { done = true; resolve() } }
    requestAnimationFrame(finish)
    setTimeout(finish, 60)
  })
}

/// 不用 act 的等待：真实事件后的 React 更新落在宏任务/帧里
async function nativeWait(times = 6) {
  for (let i = 0; i < times; i += 1) {
    await new Promise(r => setTimeout(r, 0))
    await nextFrame()
  }
}

/// 既接受元素，也接受 getClientRects() 给出的 DOMRect（名称首行就是后者）
function rectOf(el) {
  if (!el) return null
  const r = typeof el.getBoundingClientRect === 'function' ? el.getBoundingClientRect() : el
  if (!r || typeof r.width !== 'number') return null
  return {
    left: r.left, top: r.top, right: r.right, bottom: r.bottom,
    width: r.width, height: r.height, x: r.x, y: r.y,
  }
}

/// 行度量：布局断言与点击取点都以渲染结果为准（不复制一份候选 UI 到测试里）
function measureRow(row) {
  if (!row) return null
  const cs = getComputedStyle(row)
  const radio = row.querySelector('input[type=radio]')
  const nameEl = row.querySelector('.lcsc-option-name')
  const radioCs = radio ? getComputedStyle(radio) : null
  const rowRect = row.getBoundingClientRect()
  const radioRect = radio ? radio.getBoundingClientRect() : null
  const nameLines = nameEl ? [...nameEl.getClientRects()] : []
  const firstLine = nameLines[0] || (nameEl ? nameEl.getBoundingClientRect() : null)
  const list = row.closest('.lcsc-list')
  return {
    testid: row.getAttribute('data-testid'),
    nameText: nameEl ? nameEl.textContent : '',
    text: row.textContent || '',
    selectable: row.getAttribute('data-selectable'),
    className: row.className,
    display: cs.display,
    alignItems: cs.alignItems,
    fontWeight: cs.fontWeight,
    background: cs.backgroundColor,
    selected: row.className.split(' ').includes('selected'),
    hasRadio: !!radio,
    radioDisabled: radio ? !!radio.disabled : null,
    radioChecked: radio ? !!radio.checked : null,
    radioWidth: radioRect ? radioRect.width : 0,
    radioHeight: radioRect ? radioRect.height : 0,
    radioPadding: radioCs ? radioCs.paddingLeft : '',
    radioRatio: rowRect.width ? (radioRect ? radioRect.width / rowRect.width : 1) : 1,
    rowRect: rectOf(row),
    radioRect: rectOf(radio),
    nameRect: rectOf(firstLine),
    nameLines: nameLines.length,
    // radio 与名称首行左右并排：右侧相接 + 竖直方向有重叠
    sideBySide: !!(radioRect && firstLine &&
      radioRect.right <= firstLine.left + 1 &&
      radioRect.top < firstLine.bottom - 1 && firstLine.top < radioRect.bottom - 1),
    rowOverflowX: row.scrollWidth - row.clientWidth,
    nameOverflowX: nameEl ? nameEl.scrollWidth - nameEl.clientWidth : 0,
    listOverflowX: list ? list.scrollWidth - list.clientWidth : 0,
    listScrollTop: list ? list.scrollTop : null,
    listScrollRange: list ? list.scrollHeight - list.clientHeight : null,
  }
}

/// 采样点：横 5 × 竖 3，覆盖整行而不只是 bbox 中心（遮挡校验不能只看中心那一点）
function samplePoints(rect) {
  const out = []
  for (const fy of [0.5, 0.18, 0.82]) {
    for (const fx of [0.5, 0.12, 0.88, 0.3, 0.7]) {
      out.push({
        x: rect.left + rect.width * fx,
        y: rect.top + rect.height * fy,
      })
    }
  }
  return out
}

/// 「行内空白」候选点：贴着行内边距，期望 elementFromPoint 命中的就是行本身
function blankPoints(rect) {
  const inset = 3
  const out = []
  for (const y of [rect.top + inset, rect.bottom - inset, rect.top + rect.height / 2]) {
    for (const x of [rect.left + inset, rect.right - inset, rect.left + rect.width / 2]) {
      out.push({ x, y })
    }
  }
  return out
}

function hitAt(x, y) {
  const el = document.elementFromPoint(x, y)
  const row = el ? el.closest('.lcsc-option') : null
  const holder = el ? el.closest('[data-testid]') : null
  return {
    x, y,
    tag: el ? el.tagName.toLowerCase() : null,
    testid: holder ? holder.getAttribute('data-testid') : null,
    rowTestid: row ? row.getAttribute('data-testid') : null,
    isRowElement: !!row && el === row,
  }
}

function scrollRowIntoView(testid) {
  const el = tid(testid)
  if (!el) throw new Error(`native: 找不到 ${testid}`)
  el.scrollIntoView({ block: 'center', inline: 'nearest' })
  return nextFrame().then(nextFrame)
}

function resetHarness() {
  net.calls.length = 0
  net.queue.length = 0
  net.hold = false
  net.overrides = {}
  harnessApi.compLibraries = WORKSPACES.slice()
  harnessApi.gates = {}
  harnessApi.refreshFail = ''
  harnessApi.closed = false
  harnessApi.lastToast = ''
  harnessApi.refreshCount = 0
  harnessApi.refreshedSymbols = 0
  harnessApi.loadedLibraries = 0
  harnessApi.loadedStatus = 0
  unmount()
}

function rowList() {
  return $$('[data-testid]')
    .filter(el => el.classList.contains('lcsc-option'))
    .map(el => measureRow(el))
}

function nativeSnapshot() {
  return {
    rows: rowList(),
    checked: $$('.lcsc-option input[type=radio]:checked')
      .map(r => r.closest('.lcsc-option').getAttribute('data-testid')),
    importDisabled: tid('lcsc-import') ? tid('lcsc-import').disabled : null,
    targetNextDisabled: tid('lcsc-target-next') ? tid('lcsc-target-next').disabled : null,
    symbolNextDisabled: tid('lcsc-symbol-next') ? tid('lcsc-symbol-next').disabled : null,
    importError: tid('lcsc-import-error')?.textContent || '',
    summary: tid('lcsc-summary')?.textContent || '',
    hasCandidates: !!tid('lcsc-candidates'),
    hasTargetList: !!tid('lcsc-target-list'),
    viewport: { width: window.innerWidth, height: window.innerHeight },
    analyzeCalls: analyzeCalls().length,
    importBodies: importCalls().map(c => c.body),
    lastAnalyze: analyzeCalls().at(-1)?.body || null,
    clickLog: native.clicks.slice(),
    actEnvironment: globalThis.IS_REACT_ACT_ENVIRONMENT === true,
  }
}

const native = { active: false, clicks: [], onDocClick: null, savedAct: false }

const nativeApi = {
  /// 准备会话：真实组件 + 真实 CSS，用与逻辑场景相同的 DOM 输入走到指定步骤。
  /// 结束后 act 环境保持开启，等待 runner 调 begin() 转入原生驱动。
  async prepare(spec = {}) {
    if (native.active) throw new Error('native: 上一次原生会话没有结束')
    resetHarness()
    if (spec.compLibraries) harnessApi.compLibraries = spec.compLibraries
    await setup('new')
    setInput(tid('lcsc-id-input'), spec.id || 'C37593')
    net.hold = true
    click(tid('lcsc-lookup'))
    await settle()
    const call = executeCalls().at(-1)
    if (!call) throw new Error('native prepare: 没有发出 analyze 请求')
    call.respond({
      ok: true,
      plugin_result: {
        ...ANALYSIS.plugin_result,
        ...(spec.analyze || {}),
        ...(spec.candidates ? { candidates: spec.candidates } : {}),
      },
    })
    await settle()
    if (spec.stopAt === 'target') return nativeSnapshot()
    click(tid(`lcsc-target-${spec.targetId || 'pkg-ok'}`))
    click(tid('lcsc-target-next'))
    await settle(5)
    if (spec.stopAt === 'symbol') return nativeSnapshot()
    await clickSymbolNext(spec.symbolPath || 'Device.kicad_sym')
    await settle()
    if (spec.manual) {
      net.overrides['/api/footprints/search'] = () => spec.manual
      setInput(tid('lcsc-manual-search'), spec.manualQuery || 'SOT')
      await settle(4)
    }
    return nativeSnapshot()
  },

  /// 进入原生驱动：关掉 act 环境，开始记录页面真实收到的 click（含 isTrusted）
  begin() {
    if (native.active) throw new Error('native: 会话已经开始了')
    native.active = true
    native.clicks = []
    native.savedAct = globalThis.IS_REACT_ACT_ENVIRONMENT === true
    globalThis.IS_REACT_ACT_ENVIRONMENT = false
    native.onDocClick = (e) => {
      const holder = e.target?.closest?.('[data-testid]')
      const row = e.target?.closest?.('.lcsc-option')
      native.clicks.push({
        testid: holder ? holder.getAttribute('data-testid') : null,
        rowTestid: row ? row.getAttribute('data-testid') : null,
        trusted: e.isTrusted === true,
        x: e.clientX, y: e.clientY,
      })
    }
    document.addEventListener('click', native.onDocClick, true)
    return { active: native.active, actEnvironment: globalThis.IS_REACT_ACT_ENVIRONMENT === true }
  },

  /// 会话结束：先恢复 act 环境，再按真实路径卸载
  end() {
    if (native.onDocClick) document.removeEventListener('click', native.onDocClick, true)
    native.onDocClick = null
    globalThis.IS_REACT_ACT_ENVIRONMENT = native.savedAct
    native.active = false
    const clicks = native.clicks.slice()
    native.clicks = []
    unmount()
    return { active: native.active, actEnvironment: globalThis.IS_REACT_ACT_ENVIRONMENT === true, clicks }
  },

  settle(times = 6) { return nativeWait(times) },
  clearClicks() { native.clicks = []; return true },
  snapshot() { return nativeSnapshot() },

  /// 取点前的遮挡校验：先 scrollIntoView + 等两帧，再采样 elementFromPoint
  async probe(testid, aim = 'row') {
    await scrollRowIntoView(testid)
    const row = tid(testid)
    if (!row) throw new Error(`native: 找不到 ${testid}`)
    const radio = row.querySelector('[type=radio]')
    const nameEl = row.querySelector('.lcsc-option-name')
    let points
    if (aim === 'radio') {
      if (!radio) throw new Error(`native: ${testid} 没有 radio`)
      points = samplePoints(radio.getBoundingClientRect())
    } else if (aim === 'name') {
      const line = nameEl && nameEl.getClientRects()[0]
      if (!line) throw new Error(`native: ${testid} 没有名称首行`)
      points = samplePoints(line)
    } else if (aim === 'blank') {
      points = blankPoints(row.getBoundingClientRect())
    } else {
      points = samplePoints(row.getBoundingClientRect())
    }
    return {
      testid, aim,
      geometry: measureRow(row),
      rect: rectOf(row),
      hits: points.map(p => hitAt(p.x, p.y)),
    }
  },
}

// ============================================================
// 场景
// ============================================================
const results = []
async function scenario(name, fn) {
  resetHarness()
  try {
    await fn()
    results.push({ name, ok: true })
  } catch (cause) {
    results.push({ name, ok: false, error: String(cause?.message || cause) })
  }
}
function assert(cond, message) { if (!cond) throw new Error(message) }
function assertEq(a, b, message) {
  if (a !== b) throw new Error(`${message}: expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`)
}
function executeCalls() { return net.calls.filter(c => c.url.includes('/api/plugins/execute')) }
function countCalls(fragment) { return net.calls.filter(c => c.url.includes(fragment)).length }
// 受控写入：同一个接口的每次请求各拿一个独立 deferred，测试自己决定哪一个响应
// 什么时候回来 —— 才能复现「旧请求的响应/收尾落到新会话上」这类竞态，
// 而不是靠改慢整个网络。返回按调用顺序排列的 gate 列表。
function gatedWrites(fragment) {
  const gates = []
  net.overrides[fragment] = () => {
    const gate = deferred()
    gates.push(gate)
    return gate.promise
  }
  // 收尾用：把该接口上所有 gate 一次性放行（已回来的 gate 再 resolve 是空操作）。
  // 遍历整个列表而不是只看 gates[0] —— 实现错误多发出来的 B/A 写入也留在挂起态里。
  // 载荷只在组件已经卸载后才被消费，取什么值都不影响断言结果。
  gates.releaseAll = (payload = { ok: true }) => { for (const gate of gates) gate.resolve(payload) }
  return gates
}
// 受控场景的统一收尾（放进 finally）：先卸载真实 root，再放行所有仍挂起的受控请求，
// 最后排空微任务。断言抛出会跳过末尾的 resolve/unmount，不能让挂起的 fetch 留给下个场景；
// 这里只做收尾、不 catch 断言 —— 业务失败照样由 scenario 记为红。
async function finishControlled(releases = []) {
  unmount()
  for (const release of releases) release()
  await settle(4)
}
function importCalls() { return executeCalls().filter(c => c.body?.action === 'import') }
function analyzeCalls() { return executeCalls().filter(c => c.body?.action === 'analyze') }

function modalCloseButton() {
  const btn = $('.modal .modal-close')
  if (!btn) throw new Error('missing .modal-close button')
  return btn
}

async function lookupOk(extra = {}, id = 'C37593') {
  net.hold = false
  net.queue.push({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, ...extra } })
  setInput(tid('lcsc-id-input'), id)
  click(tid('lcsc-lookup'))
  await settle()
}

async function clickTargetNext(targetId = 'pkg-ok') {
  click(tid(`lcsc-target-${targetId}`))
  click(tid('lcsc-target-next'))
  await settle(5)
  // General footprint scenarios explicitly choose the default test sublibrary.
  // Scenarios about the symbol step use the two target clicks directly.
  await clickSymbolNext()
}

async function clickSymbolNext(path = 'Device.kicad_sym') {
  click(tid(`lcsc-symbol-${path}`))
  click(tid('lcsc-symbol-next'))
  await settle()
}

async function newScenarios() {
  await scenario('new: 打开时只有第一步可操作（编号 + 查找）', async () => {
    await setup('new')
    assert(tid('lcsc-id-input'), 'part number input missing')
    assert(tid('lcsc-lookup'), 'lookup button missing')
    assert(!tid('lcsc-import'), 'import button must not exist before a successful lookup')
    assert(!tid('lcsc-target-list'), 'target list must be hidden before lookup')
    assert(!tid('lcsc-candidates'), 'candidate list must be hidden before lookup')
    assertEq(executeCalls().length, 0, 'opening the dialog sends no request')
  })

  await scenario('new: Enter 触发查找，成功后才出现目标库步', async () => {
    await setup('new')
    setInput(tid('lcsc-id-input'), 'C37593')
    net.hold = false
    net.queue.push(ANALYSIS)
    enter(tid('lcsc-id-input'))
    await settle()
    assertEq(executeCalls().length, 1, 'Enter must trigger exactly one analyze request')
    assertEq(executeCalls()[0].body.action, 'analyze', 'lookup uses the analyze action')
    assertEq(executeCalls()[0].body.lcsc_id, 'C37593', 'raw id passed to plugin')
    assert(tid('lcsc-target-list'), 'target list appears after a successful lookup')
    assert((tid('lcsc-device')?.textContent || '').includes('AMS1117-3.3'), 'device/symbol name shown')
    assert((tid('lcsc-device')?.textContent || '').includes('SOT-223'), 'original footprint name shown')
  })

  await scenario('new: 空编号与非法编号立即给提示且不发请求', async () => {
    await setup('new')
    click(tid('lcsc-lookup'))
    await settle()
    assert(tid('lcsc-input-error'), 'empty id must show an inline error')
    setInput(tid('lcsc-id-input'), 'C12A3')
    click(tid('lcsc-lookup'))
    await settle()
    assert(tid('lcsc-input-error'), 'malformed id must show an inline error')
    setInput(tid('lcsc-id-input'), '../etc/passwd')
    click(tid('lcsc-lookup'))
    await settle()
    assert(tid('lcsc-input-error'), 'path-like id must show an inline error')
    assertEq(executeCalls().length, 0, 'invalid input never reaches the plugin')
  })

  await scenario('new: 纯数字补 C、大小写与首尾空白规范化', async () => {
    await setup('new')
    await lookupOk({}, '  37593 ')
    assertEq(executeCalls().at(-1)?.body.lcsc_id, 'C37593', 'numeric id gains the C prefix')
    await setup('new')
    await lookupOk({}, ' c37593 ')
    assertEq(executeCalls().at(-1)?.body.lcsc_id, 'C37593', 'case and whitespace normalized')
  })

  await scenario('new: 查找失败给出原因，不留上一次的成功结果', async () => {
    await setup('new')
    await lookupOk()
    assert(tid('lcsc-target-list'), 'first lookup succeeded')
    setInput(tid('lcsc-id-input'), 'C99999')
    await settle()
    assert(!tid('lcsc-target-list'), 'changing the id drops the previous result immediately')
    net.queue.push({ ok: false, error: 'invalid LCSC part number' })
    click(tid('lcsc-lookup'))
    await settle()
    assert(!tid('lcsc-target-list'), 'failed lookup leaves no stale target step')
    assert(!tid('lcsc-import'), 'failed lookup leaves no import button')
    assert((tid('lcsc-lookup-error')?.textContent || '').includes('invalid LCSC part number'),
      'server reason is shown')
  })

  await scenario('new: 内层 plugin_result.ok=false 也按失败处理', async () => {
    await setup('new')
    setInput(tid('lcsc-id-input'), 'C37593')
    net.queue.push({ ok: true, plugin_result: { ok: false, error: 'conversion failed' } })
    click(tid('lcsc-lookup'))
    await settle()
    assert((tid('lcsc-lookup-error')?.textContent || '').includes('conversion failed'),
      'inner error surfaces instead of an empty candidate list')
    assert(!tid('lcsc-target-list'), 'no target step on inner failure')
  })

  await scenario('new: 真实插件失败包装（外层无 error）显示内层具体原因且无法继续', async () => {
    await setup('new')
    setInput(tid('lcsc-id-input'), 'C37593')
    net.queue.push({ ok: false, plugin_result: { ok: false, error: 'download conversion reason' } })
    click(tid('lcsc-lookup'))
    await settle()
    assert((tid('lcsc-lookup-error')?.textContent || '').includes('download conversion reason'),
      'the plugin inner reason is shown, not a generic wrapper error')
    assert(!tid('lcsc-target-list'), 'no library selection after a failed lookup')
    assert(!tid('lcsc-import'), 'no import entry after a failed lookup')
  })

  await scenario('new: 损坏候选不会打崩弹窗（合法候选保留 + 明确提示）', async () => {
    await setup('new')
    await lookupOk({ candidates: [live({ id: uuids.A, library: 'main', name: 'GOOD', score: 0.9, pad_count: 4 }), null, 7] })
    await clickTargetNext()
    const rows = $$('[data-testid^="lcsc-cand-"]')
    assertEq(rows.length, 1, 'valid candidates survive a corrupted payload')
    assert(rows[0].textContent.includes('GOOD'), 'the valid candidate is rendered')
    assert(tid('lcsc-candidates-dropped'), 'the dropped entries are reported')
    click(rows[0])
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'the dialog stays usable')
  })

  await scenario('new: 手动搜索返回损坏响应（null）时报错而不是装成空结果', async () => {
    await setup('new')
    await lookupOk({ candidates: [] })
    await clickTargetNext()
    net.overrides['/api/footprints/search'] = () => null
    setInput(tid('lcsc-manual-search'), 'SOT')
    await settle(4)
    assert(tid('lcsc-manual-error'), 'a malformed search response is reported')
    assertEq(tid('lcsc-import').disabled, true, 'no footprint can be chosen from a broken response')
    assert($('.modal'), 'dialog still mounted')
  })

  await scenario('new: 目标库列表来自真实包数据，锁定/缺路径禁用并给原因，空包可选', async () => {
    await setup('new')
    await lookupOk()
    const options = $$('[data-testid^="lcsc-target-pkg-"]')
    assertEq(options.length, 3, 'all packages from /api/component-libraries are listed')
    const locked = tid('lcsc-target-pkg-locked')
    const noPaths = tid('lcsc-target-pkg-paths')
    assertEq(locked.getAttribute('data-selectable'), 'false', 'locked package disabled')
    assert((locked.textContent || '').includes('锁定'), 'locked reason shown')
    assertEq(noPaths.getAttribute('data-selectable'), 'false', 'package without paths disabled')
    assert((noPaths.textContent || '').includes('路径'), 'missing-path reason shown')
    assertEq(tid('lcsc-target-pkg-ok').getAttribute('data-selectable'), 'true', 'usable package selectable')
    assertEq($$('input[type=radio]:checked').length, 0, 'no package is preselected')
    assertEq(tid('lcsc-target-next').disabled, true, 'cannot continue without an explicit package')

    // 空的新包也可选（不能依赖 /api/libraries 的 used 过滤）
    net.overrides['/api/component-libraries'] = () => [
      { id: 'pkg-empty', name: '新建空包', symbol_path: 'D:\\new\\sym', footprint_path: 'D:\\new\\fp', enabled: true, locked: false, is_protected: false },
    ]
    click(tid('lcsc-refresh-targets'))
    await settle()
    assertEq(tid('lcsc-target-pkg-empty').getAttribute('data-selectable'), 'true', 'empty package stays selectable')
    assertEq($$('input[type=radio]:checked').length, 0, 'refresh does not auto-pick a package')
  })

  await scenario('new: 选包后必须明确选符号子库，同名嵌套路径分别显示并原样提交', async () => {
    await setup('new')
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next'))
    await settle(5)
    assert(tid('lcsc-symbol-step'), 'package selection opens the symbol library step')
    assertEq(tid('lcsc-symbol-next').disabled, true, 'no default symbol library')
    assert(!tid('lcsc-candidates'), 'footprints remain hidden until a symbol library is confirmed')
    assertEq($$('input[name=lcsc-symbol]:checked').length, 0, 'both same-name files remain unselected')
    assert(tid('lcsc-symbol-Device.kicad_sym'), 'root file is available')
    assert(tid('lcsc-symbol-nested/Device.kicad_sym'), 'nested file is independently available')
    await clickSymbolNext('nested/Device.kicad_sym')
    assertEq(tid('lcsc-symbol-summary').textContent, 'nested/Device.kicad_sym', 'summary shows exact relative path')
    click(tid('lcsc-new-footprint')); click(tid('lcsc-import'))
    await settle(6)
    assertEq(importCalls()[0]?.body.symbol_library_choice.path, 'nested/Device.kicad_sym', 'selected file, not first same-name file')
    assertEq(importCalls()[0]?.body.symbol_library_choice.mode, 'existing', 'existing file choice')
    assertEq('target_library_id' in importCalls()[0].body, false, 'old library-id mode is absent')
  })

  await scenario('new: 空子库可选；没有子库时明确新建并规范化名称，不预先写文件', async () => {
    await setup('new')
    net.overrides['/symbol-libraries'] = () => ({ ok: true, libraries: [], can_create: true })
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next'))
    await settle(5)
    assert(tid('lcsc-symbol-empty'), 'an empty package is explained')
    assertEq(tid('lcsc-symbol-next').disabled, true, 'empty package is not an implicit create decision')
    click(tid('lcsc-symbol-new'))
    assertEq(tid('lcsc-symbol-next').disabled, true, 'a name is required')
    setInput(tid('lcsc-symbol-name'), '../Switches')
    assertEq(tid('lcsc-symbol-next').disabled, true, 'paths cannot be entered as names')
    setInput(tid('lcsc-symbol-name'), ' Switches.kicad_sym ')
    click(tid('lcsc-symbol-next'))
    assertEq(tid('lcsc-symbol-summary').textContent, 'Switches.kicad_sym', 'normalized final file is shown')
    assertEq(importCalls().length, 0, 'choosing a name does not write anything')
    click(tid('lcsc-new-footprint')); click(tid('lcsc-import'))
    await settle(6)
    assertEq(JSON.stringify(importCalls()[0]?.body.symbol_library_choice), JSON.stringify({ mode: 'new', name: 'Switches' }), 'single normalized stem is sent')
  })

  await scenario('new: 子库读取失败或未知响应阻断下一步，刷新可恢复且不默认选择', async () => {
    for (const payload of [null, { ok: true, libraries: [] }, { ok: false, libraries: [], can_create: true }]) {
      await setup('new')
      net.overrides['/symbol-libraries'] = () => payload
      await lookupOk()
      click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next'))
      await settle(5)
      assert(tid('lcsc-symbol-error'), 'malformed response is an error')
      assertEq(tid('lcsc-symbol-next').disabled, true, 'cannot proceed with unknown catalog')
      assert(!tid('lcsc-symbol-new'), 'a failed list is not treated as an empty createable package')
      net.overrides['/symbol-libraries'] = () => SYMBOL_LIBRARIES
      click(tid('lcsc-symbol-refresh'))
      await settle(5)
      assert(!tid('lcsc-symbol-error'), 'successful refresh clears error')
      assertEq(tid('lcsc-symbol-next').disabled, true, 'refresh does not pick the first item')
    }
  })

  await scenario('new: 大小写后缀已有文件可选，新建单次去后缀并拒绝重复、控制字符和超长名', async () => {
    await setup('new')
    net.overrides['/symbol-libraries'] = () => ({ ok: true, can_create: true,
      libraries: [{ name: 'Mixed', path: 'nested/Mixed.KICAD_SYM', id: 'mixed' }] })
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next')); await settle(5)
    assert(!tid('lcsc-symbol-error'), 'host accepts case-insensitive file extensions')
    click(tid('lcsc-symbol-nested/Mixed.KICAD_SYM'))
    assertEq(tid('lcsc-symbol-next').disabled, false, 'mixed-case existing file is selectable')
    click(tid('lcsc-symbol-new'))
    for (const invalid of ['A.KICAD_SYM.kicad_sym', `x${String.fromCodePoint(127)}`, '元'.repeat(82)]) {
      setInput(tid('lcsc-symbol-name'), invalid)
      assertEq(tid('lcsc-symbol-next').disabled, true, 'invalid name blocks next')
    }
    setInput(tid('lcsc-symbol-name'), 'Switches.KICAD_SYM')
    click(tid('lcsc-symbol-next'))
    assertEq(tid('lcsc-symbol-summary').textContent, 'Switches.kicad_sym', 'upper-case optional suffix is removed once')
    click(tid('lcsc-new-footprint')); click(tid('lcsc-import')); await settle(6)
    assertEq(importCalls()[0]?.body.symbol_library_choice.name, 'Switches', 'host receives normalized stem')
  })

  await scenario('new: 单文件配置只显示那个子库，刷新保留有效选择，删除后不静默改选', async () => {
    await setup('new')
    net.overrides['/symbol-libraries'] = () => ({ ...SYMBOL_LIBRARIES, can_create: false })
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next'))
    await settle(5)
    assert(!tid('lcsc-symbol-new'), 'single-file configuration cannot create another library')
    click(tid('lcsc-symbol-nested/Device.kicad_sym'))
    click(tid('lcsc-symbol-refresh')); await settle(5)
    assertEq(tid('lcsc-symbol-next').disabled, false, 'same valid explicit selection survives refresh')
    assertEq(tid('lcsc-symbol-nested/Device.kicad_sym').querySelector('input').checked, true, 'identity remains the relative path')
    net.overrides['/symbol-libraries'] = () => ({ ok: true, can_create: false, libraries: [SYMBOL_LIBRARIES.libraries[0]] })
    click(tid('lcsc-symbol-refresh')); await settle(5)
    assertEq(tid('lcsc-symbol-next').disabled, true, 'removed selection blocks next')
    assertEq(tid('lcsc-symbol-Device.kicad_sym').querySelector('input').checked, false, 'remaining item is not selected silently')
  })

  await scenario('new: 换包后忽略旧子库响应，关闭和改编号也作废在途读取', async () => {
    const second = { ...WORKSPACES[0], id: 'pkg-second', name: '第二包' }
    net.overrides['/api/component-libraries'] = () => [WORKSPACES[0], second]
    harnessApi.compLibraries = [WORKSPACES[0], second]
    await setup('new'); await lookupOk()
    const old = deferred()
    net.overrides['/api/component-libraries/pkg-ok/symbol-libraries'] = () => old.promise
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next')); await settle(3)
    click(tid('lcsc-symbol-back'))
    click(tid('lcsc-target-pkg-second')); click(tid('lcsc-target-next')); await settle(5)
    assert(tid('lcsc-symbol-Device.kicad_sym'), 'new package catalog arrived')
    old.resolve({ ok: true, can_create: true, libraries: [{ name: 'Stale', path: 'Stale.kicad_sym' }] })
    await settle(5)
    assert(!tid('lcsc-symbol-Stale.kicad_sym'), 'old package response ignored')
    const pending = deferred()
    net.overrides['/symbol-libraries'] = () => pending.promise
    click(tid('lcsc-symbol-refresh')); await settle(2)
    setInput(tid('lcsc-id-input'), 'C99999')
    pending.resolve(SYMBOL_LIBRARIES); await settle(5)
    assert(!tid('lcsc-symbol-step'), 'editing id returns to lookup and ignores old catalog')
    const closing = deferred()
    net.overrides['/symbol-libraries'] = () => closing.promise
    await lookupOk()
    click(tid('lcsc-target-pkg-second')); click(tid('lcsc-target-next')); await settle(2)
    click(tid('lcsc-close')); closing.resolve(SYMBOL_LIBRARIES); await settle(5)
    assertEq(harnessApi.closed, true, 'close during reading succeeds')
    assert(!$('.modal'), 'late read cannot reopen the modal')
  })

  await scenario('new: 改子库清掉封装选择和旧操作 ID，并显示新路径', async () => {
    await setup('new'); await lookupOk(); await clickTargetNext()
    net.hold = true
    click(tid(`lcsc-cand-${uuids.A}`)); click(tid('lcsc-import')); await settle(6)
    importCalls()[0].respond({ ok: false, error: 'first attempt failed' }); await settle(5)
    const previousOperation = importCalls()[0].body.operation_id
    click(tid('lcsc-back-target')); await settle(5)
    await clickSymbolNext('nested/Device.kicad_sym')
    assertEq(tid('lcsc-import').disabled, true, 'footprint must be chosen again')
    assertEq(tid('lcsc-symbol-summary').textContent, 'nested/Device.kicad_sym', 'new exact sublibrary shown')
    click(tid(`lcsc-cand-${uuids.B}`)); click(tid('lcsc-import')); await settle(6)
    assert(importCalls()[1].body.operation_id !== previousOperation, 'new symbol library is a new intent')
    assertEq(importCalls()[1].body.symbol_library_choice.path, 'nested/Device.kicad_sym', 'new intent uses chosen path')
    assertEq(importCalls()[1].body.footprint_id, uuids.B, 'new footprint choice sent')
  })

  await scenario('new: 新建重名按宿主大小写规则拒绝，新意图不能覆盖已有子库', async () => {
    await setup('new')
    net.overrides['/symbol-libraries'] = () => ({ ...SYMBOL_LIBRARIES, case_sensitive: false })
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next')); await settle(5)
    click(tid('lcsc-symbol-new')); setInput(tid('lcsc-symbol-name'), 'device.kicad_sym')
    assertEq(tid('lcsc-symbol-next').disabled, true, 'host says case-insensitive so lowercase collision is blocked')
    assert(tid('lcsc-symbol-validation').textContent.includes('已存在'), 'existing-library action is explained')
    setInput(tid('lcsc-symbol-name'), 'Switches'); click(tid('lcsc-symbol-next')); click(tid('lcsc-new-footprint'))
    net.overrides['/symbol-libraries'] = () => ({ ok: true, can_create: true, libraries: [{ name: 'Switches', path: 'Switches.kicad_sym' }] })
    click(tid('lcsc-import')); await settle(5)
    assertEq(importCalls().length, 0, 'collision found at final check does not write')
    assert(tid('lcsc-import-error').textContent.includes('已存在'), 'collision is shown')
  })

  await scenario('new: 新建导入返回未知后允许同操作重试，即使文件已出现；提交期间冻结选择', async () => {
    await setup('new')
    net.overrides['/symbol-libraries'] = () => ({ ok: true, can_create: true, libraries: [] })
    await lookupOk()
    click(tid('lcsc-target-pkg-ok')); click(tid('lcsc-target-next')); await settle(5)
    click(tid('lcsc-symbol-new')); setInput(tid('lcsc-symbol-name'), 'Switches')
    click(tid('lcsc-symbol-next')); click(tid('lcsc-new-footprint'))
    const lost = deferred(); net.queue.push(lost.promise)
    click(tid('lcsc-import')); await settle(6)
    assertEq(tid('lcsc-back-target').disabled, true, 'cannot change sublibrary while submitting')
    click(tid('lcsc-back-target')); assert(tid('lcsc-footprint-step'), 'synchronous edit guard keeps confirmation')
    lost.reject(new Error('connection lost')); await settle(6)
    const first = importCalls()[0].body
    net.overrides['/symbol-libraries'] = () => ({ ok: true, can_create: true, libraries: [{ name: 'Switches', path: 'Switches.kicad_sym' }] })
    net.queue.push({ ok: true, plugin_result: { ok: true, replayed: true, mode: 'new' } })
    click(tid('lcsc-import')); await settle(9)
    assertEq(importCalls().length, 2, 'same intention is resent for host replay')
    assertEq(importCalls()[1].body.operation_id, first.operation_id, 'same operation id retained')
    assertEq(JSON.stringify(importCalls()[1].body.symbol_library_choice), JSON.stringify(first.symbol_library_choice), 'same new mode and normalized stem retained')
    assertEq(harnessApi.closed, true, 'successful replay then refresh closes modal')
  })

  await scenario('new: 候选全部展示、默认没有选择、导入被禁用', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    const rows = $$('[data-testid^="lcsc-cand-"]')
    assertEq(rows.length, 2, 'both candidates are shown')
    assert(rows[0].textContent.includes('SOT-223'), 'candidate name shown')
    assert(rows[0].textContent.includes('main'), 'candidate library shown')
    assert(rows[0].textContent.includes('94%'), 'similarity shown')
    assert(rows[0].textContent.includes('4'), 'pad count shown')
    assert(!rows[0].textContent.includes(uuids.A), 'UUID is identity only, never displayed')
    assertEq($$('input[type=radio]:checked').length, 0, 'nothing is selected by default')
    assertEq(tid('lcsc-import').disabled, true, 'import disabled without an explicit choice')
    assert(tid('lcsc-new-footprint'), 'new-footprint option is offered')
  })

  await scenario('new: footprint_id 别名的候选同样能选中并提交 UUID', async () => {
    await setup('new')
    await lookupOk({ candidates: [live({ footprint_id: 'aaaaaaaa-1111-4111-8111-111111111111', library: 'main', name: 'ALIAS', score: 0.8, pad_count: 3 })] })
    await clickTargetNext()
    const row = $('[data-testid^="lcsc-cand-"]')
    assertEq(row.getAttribute('data-selectable'), 'true', 'footprint_id alias counts as an identity')
    click(row)
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'aliased candidate enables import')
    net.hold = true
    click(tid('lcsc-import'))
    await settle()
    const sent = executeCalls().at(-1)?.body
    assertEq(sent?.footprint_id, 'aaaaaaaa-1111-4111-8111-111111111111', 'the alias UUID is submitted as footprint_id')
  })

  await scenario('new: 空候选时不能自动新建，必须显式点“新建封装”', async () => {
    await setup('new')
    await lookupOk({ candidates: [] })
    await clickTargetNext()
    assertEq($$('[data-testid^="lcsc-cand-"]').length, 0, 'no candidates rendered')
    assertEq(tid('lcsc-import').disabled, true, 'empty candidates must not auto-select new')
    click(tid('lcsc-new-footprint'))
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'explicit new enables import')
  })

  await scenario('new: 缺少 UUID 的候选不可选且给出原因', async () => {
    await setup('new')
    await lookupOk({ candidates: [{ library: 'main', name: 'SOT-223', score: 0.9, pad_count: 4 }] })
    await clickTargetNext()
    const row = $('[data-testid^="lcsc-cand-"]')
    assert(row, 'candidate without UUID is still listed')
    assertEq(row.getAttribute('data-selectable'), 'false', 'candidate without UUID is not selectable')
    click(row)
    await settle()
    assertEq(tid('lcsc-import').disabled, true, 'cannot reuse a candidate without an identity')
  })

  await scenario('new: 复用提交带目标包 ID 与候选 UUID，且不带旧符号库字段', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.B}`))
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'explicit candidate enables import')
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse', footprint_reference: 'main:SOT-223-3' } })
    click(tid('lcsc-import'))
    await settle(6)
    const sent = executeCalls().at(-1)?.body
    assert(sent, 'import request sent')
    assertEq(sent.target_component_library_id, 'pkg-ok', 'component package id used as target')
    assertEq(sent.target_library, undefined, 'no legacy target_library path')
    assertEq(sent.target_library_id, undefined, 'no legacy symbol library id')
    assertEq(sent.footprint_id, uuids.B, 'explicitly chosen candidate UUID is sent')
    assertEq(sent.footprint_choice?.mode, 'reuse', 'choice mode is reuse')
    assertEq(sent.operation_id?.length > 0, true, 'an operation id is sent for idempotency')
    assertEq(harnessApi.closed, true, 'dialog closes after success')
    assert(harnessApi.refreshedSymbols >= 1, 'symbols refreshed after success')
    assert(harnessApi.loadedLibraries >= 1 && harnessApi.loadedStatus >= 1, 'libraries and counters refreshed')
  })

  await scenario('new: 新建模式不带任何 footprint_id', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid('lcsc-new-footprint'))
    await settle()
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'new', footprint_reference: 'SOT-223' } })
    click(tid('lcsc-import'))
    await settle(6)
    const sent = executeCalls().at(-1)?.body
    assertEq(sent.footprint_choice?.mode, 'new', 'new mode sent')
    assertEq('footprint_id' in sent, false, 'new mode must not carry a stale footprint UUID')
  })

  await scenario('new: 摘要显示编号/器件/目标库/复用或新建', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    await settle()
    const summary = tid('lcsc-summary')?.textContent || ''
    assert(summary.includes('C37593'), 'summary shows the part number')
    assert(summary.includes('AMS1117-3.3'), 'summary shows the device')
    assert(summary.includes('主元件库'), 'summary shows the target package name')
    assert(summary.includes('SOT-223'), 'summary shows the reused footprint')
    assert(!/pkg-ok/.test(summary), 'summary never leaks the package id')
  })

  await scenario('new: 改编号立即清掉候选、选择与提交意图', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    await settle()
    setInput(tid('lcsc-id-input'), 'C99999')
    await settle()
    assert(!tid('lcsc-candidates'), 'candidate list cleared on id change')
    assert(!tid('lcsc-import'), 'import intent cleared on id change')
    assert(!tid('lcsc-target-list'), 'back to step 1')
  })

  await scenario('new: 重新查找作废旧候选（不能带旧 UUID 提交）', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.queue.push({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, candidates: [] } })
    click(tid('lcsc-lookup'))
    await settle()
    await clickTargetNext()
    assertEq(tid('lcsc-import').disabled, true, 'stale selection cannot survive a new lookup')
    assertEq($$('[data-testid^="lcsc-cand-"]').length, 0, 'old candidates dropped')
  })

  await scenario('new: 乱序响应不覆盖新查找状态，旧 finally 不动新请求的 loading', async () => {
    await setup('new')
    net.hold = true
    setInput(tid('lcsc-id-input'), 'C11111')
    click(tid('lcsc-lookup'))
    await settle()
    setInput(tid('lcsc-id-input'), 'C22222')
    click(tid('lcsc-lookup'))
    await settle()
    const [first, second] = executeCalls()
    assert(first && second, 'two analyze requests in flight')
    // 旧请求先返回：它必须被丢弃，而且新请求仍在飞 → 仍在“查找中”
    first.respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, lcsc_id: 'C11111', symbol_names: ['FIRST'], footprint_name: 'FP-1' } })
    await settle()
    assertEq(tid('lcsc-lookup').disabled, true, 'the pending new request still owns the loading state')
    assert((tid('lcsc-lookup').textContent || '').includes('查找中'), 'old finally must not clear the new request loading flag')
    assert(!tid('lcsc-device'), 'stale response never produces a device panel')
    second.respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, lcsc_id: 'C22222', symbol_names: ['SECOND'], footprint_name: 'FP-2' } })
    await settle()
    const text = tid('lcsc-device')?.textContent || ''
    assert(text.includes('SECOND'), 'newest response wins')
    assert(!text.includes('FIRST'), 'stale response must not overwrite the new state')
  })

  await scenario('new: 同一个未完成查找期间重复 Enter/点击只发一次请求', async () => {
    await setup('new')
    net.hold = true
    setInput(tid('lcsc-id-input'), 'C11111')
    enter(tid('lcsc-id-input'))
    enter(tid('lcsc-id-input'))
    await settle(2)
    assertEq(analyzeCalls().length, 1, 'a pending lookup for the same id is never duplicated')
    assertEq(tid('lcsc-lookup').disabled, true, 'lookup button frozen while pending')

    // 改编号后可以发新查找，并按版本丢弃旧响应
    setInput(tid('lcsc-id-input'), 'C22222')
    enter(tid('lcsc-id-input'))
    await settle(2)
    assertEq(analyzeCalls().length, 2, 'a changed id starts a new lookup')
    const [first, second] = analyzeCalls()
    second.respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, lcsc_id: 'C22222', symbol_names: ['SECOND'] } })
    await settle(2)
    first.respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, lcsc_id: 'C11111', symbol_names: ['FIRST'] } })
    await settle(2)
    const text = tid('lcsc-device')?.textContent || ''
    assert(text.includes('SECOND'), 'newest response wins')
    assert(!text.includes('FIRST'), 'the invalidated response is dropped')
  })

  await scenario('new: 同编号旧请求晚返回不得清掉新请求的防重（A1→B→A2→A1 返回）', async () => {
    await setup('new')
    net.hold = true
    // A1：C37593 发出去但先不返回
    setInput(tid('lcsc-id-input'), 'C37593')
    enter(tid('lcsc-id-input'))
    await settle()
    // 改成 B 查找（B 也不返回），再改回 A 发 A2 —— 此刻 A1 仍是“同编号”的在途请求
    setInput(tid('lcsc-id-input'), 'C11111')
    await settle()
    enter(tid('lcsc-id-input'))
    await settle()
    setInput(tid('lcsc-id-input'), 'C37593')
    await settle()
    enter(tid('lcsc-id-input'))
    await settle(2)
    const calls = analyzeCalls()
    assertEq(calls.length, 3, 'A1 / B1 / A2 共三次查找')
    assertEq(calls[0].body.lcsc_id, 'C37593', '第一次是 A1')
    assertEq(calls[2].body.lcsc_id, 'C37593', '第三次是 A2')

    // 旧 A1 晚返回：它不是当前请求，不能清掉 A2 的同步防重标记
    calls[0].respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, symbol_names: ['A1-STALE'] } })
    await settle(2)
    assert(!tid('lcsc-device'), '过期的 A1 不产生器件面板')
    assertEq(tid('lcsc-lookup').disabled, true, 'A2 仍在飞，查找中')

    enter(tid('lcsc-id-input'))
    enter(tid('lcsc-id-input'))
    await settle(2)
    assertEq(analyzeCalls().length, 3, 'A2 在途时重复 Enter 不能再发第三次同编号请求')

    calls[2].respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, symbol_names: ['A2-CURRENT'] } })
    await settle(2)
    assert((tid('lcsc-device')?.textContent || '').includes('A2-CURRENT'), 'A2 的结果生效')
  })

  await scenario('new: 目标包在确认后被锁定/删除 → Next 与 Import 立即禁用并说明原因', async () => {
    await setup('new')
    await lookupOk()
    // 目标步：选中的包在 props 里变成锁定 → Next 不可点
    click(tid('lcsc-target-pkg-ok'))
    await settle()
    assertEq(tid('lcsc-target-next').disabled, false, 'usable package allows continuing')
    setCompLibraries(WORKSPACES.map(w => w.id === 'pkg-ok' ? { ...w, locked: true } : w))
    await settle()
    assertEq(tid('lcsc-target-next').disabled, true, 'a package locked in the meantime cannot be confirmed')
    assert((tid('lcsc-target-blocked')?.textContent || '').includes('锁定'), 'the reason is shown')

    // 恢复正常继续，进入封装步后再被删除 → Import 禁用并说明
    setCompLibraries(WORKSPACES.slice())
    await settle()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'import enabled with a usable package')
    setCompLibraries(WORKSPACES.filter(w => w.id !== 'pkg-ok'))
    await settle()
    assertEq(tid('lcsc-import').disabled, true, 'a deleted package blocks the import')
    assert(tid('lcsc-target-invalid'), 'the blocking reason is visible in the footprint step')
  })

  await scenario('new: 关闭真的卸载会话，重新打开后旧响应不影响新 DOM', async () => {
    await setup('new')
    net.hold = true
    setInput(tid('lcsc-id-input'), 'C11111')
    click(tid('lcsc-lookup'))
    await settle()
    const late = executeCalls().at(-1)
    click(modalCloseButton())
    await settle()
    assertEq(harnessApi.closed, true, 'lookup phase can be closed')
    assertEq($('#root').children.length, 0, 'the dialog is really unmounted, not just flagged')

    // 重新打开：一个独立的新会话
    harnessApi.closed = false
    await setup('new')
    setInput(tid('lcsc-id-input'), 'C33333')
    await settle()
    assertEq(executeCalls().length, 1, 'the reopened session sends nothing on its own')
    late.respond({ ok: true, plugin_result: { ...ANALYSIS.plugin_result, lcsc_id: 'C11111', symbol_names: ['LATE'] } })
    await settle()
    assert(!tid('lcsc-device'), 'late response from the closed session must not touch the new one')
    assert(!tid('lcsc-lookup-error'), 'late failure must not surface either')
    assertEq(tid('lcsc-id-input').value, 'C33333', 'new session keeps its own input')
    click(tid('lcsc-lookup'))
    await settle()
    assertEq(analyzeCalls().length, 2, 'the new session issues its own request')
    assertEq(importCalls().length, 0, 'still no import from the reopened session')
  })

  await scenario('new: 双击导入同步派发也只发一次请求（不靠按钮 disabled）', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.hold = true
    // 同一个事件循环内连点两次，中间没有 await
    click(tid('lcsc-import'))
    click(tid('lcsc-import'))
    assertEq(importCalls().length, 0, 'the first click is still blocked on the pre-submit refresh')
    await settle(2)
    assertEq(importCalls().length, 1, 'double click sends exactly one import request')
  })

  await scenario('new: 导入期间冻结界面，Escape/遮罩/关闭按钮都不能关闭', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.hold = true
    click(tid('lcsc-import'))
    await settle()
    escape()
    await settle()
    click(modalCloseButton())
    await settle()
    click($('.modal-overlay'))
    await settle()
    assertEq(harnessApi.closed, false, 'dialog stays open while the import is running')
    assert($('[data-testid="lcsc-id-input"]')?.disabled, 'inputs frozen during import')
    assertEq(tid('lcsc-import')?.disabled, true, 'import button frozen during import')
    assertEq($('.modal') !== null, true, 'dialog still mounted')
  })

  await scenario('new: 失败保留表单与同一个 operation_id 以便重试', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.hold = true
    click(tid('lcsc-import'))
    await settle()
    const first = executeCalls().at(-1)
    first.respond({ ok: false, error: 'disk full' })
    await settle()
    assert((tid('lcsc-import-error')?.textContent || '').includes('disk full'), 'failure reason shown')
    assertEq(harnessApi.closed, false, 'dialog stays open after a failure')
    assert(tid('lcsc-candidates'), 'candidate list kept for retry')
    assertEq(tid('lcsc-import').disabled, false, 'retry is possible')
    // 普通导入失败（没写进去）不冻结：表单与返回都还可以用
    assertEq(tid('lcsc-id-input').disabled, false, '普通失败后编号仍可编辑')
    assertEq(tid('lcsc-back-target').disabled, false, '普通失败后仍可返回上一步')
    click(tid('lcsc-import'))
    await settle()
    const second = executeCalls().at(-1)
    assertEq(second.body.operation_id, first.body.operation_id, 'retry reuses the same operation id')
    second.respond({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    await settle(6)
    assertEq(harnessApi.closed, true, 'retry succeeds and closes')
  })

  await scenario('new: 换候选后 operation_id 变化', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.hold = true
    click(tid('lcsc-import'))
    await settle()
    const first = executeCalls().at(-1)
    first.respond({ ok: false, error: 'boom' })
    await settle()
    click(tid(`lcsc-cand-${uuids.B}`))
    await settle()
    click(tid('lcsc-import'))
    await settle()
    const second = executeCalls().at(-1)
    assert(first.body.operation_id !== second.body.operation_id,
      'a different choice is a different operation')
  })

  await scenario('new: 公共刷新命令只调用一次，等待 void 完成后关闭', async () => {
    const gate = deferred()
    let calls = 0
    try {
      await setup('new', { onRefresh: async () => { calls += 1; await gate.promise } })
      await lookupOk()
      await clickTargetNext()
      click(tid(`lcsc-cand-${uuids.A}`))
      net.hold = false
      net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
      click(tid('lcsc-import'))
      await settle(6)
      assertEq(calls, 1, '统一命令只调用一次')
      assertEq(harnessApi.refreshedSymbols + harnessApi.loadedLibraries + harnessApi.loadedStatus,
        0, '统一刷新分支不再调用旧三项回调')
      assertEq(harnessApi.closed, false, '等待统一刷新完成')
      gate.resolve()
      await settle(6)
      assertEq(harnessApi.closed, true, 'Promise<void>成功完成即可关闭')
      assertEq(importCalls().length, 1, '只导入一次')
    } finally {
      await finishControlled([() => gate.resolve()])
    }
  })

  await scenario('new: 公共刷新命令失败后保留原因，只重试刷新', async () => {
    let calls = 0
    let failure = true
    await setup('new', { onRefresh: async () => {
      calls += 1
      if (failure) throw new Error('host refresh did not publish')
    } })
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    click(tid('lcsc-import'))
    await settle(6)
    assertEq(calls, 1, '第一次刷新调用一次')
    assertEq(harnessApi.closed, false, '刷新失败不关闭')
    assert((tid('lcsc-refresh-error')?.textContent || '').includes('host refresh did not publish'),
      '保留真实失败原因')
    failure = false
    click(tid('lcsc-retry-refresh'))
    click(tid('lcsc-retry-refresh'))
    await settle(6)
    assertEq(calls, 2, '双击重试仍只调用一次')
    assertEq(harnessApi.closed, true, '重试成功关闭')
    assertEq(importCalls().length, 1, '刷新重试不再次导入')
  })

  await scenario('new: 三个刷新逐个完成之后才关闭（符号 → 库 → 统计，每一步都真的在等）', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    // 三个受控 Promise 依次挂起：只有前一个解决，后一个才会被调用
    const symbolsGate = deferred()
    const librariesGate = deferred()
    const statusGate = deferred()
    harnessApi.gates.symbols = symbolsGate
    harnessApi.gates.libraries = librariesGate
    harnessApi.gates.status = statusGate
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    click(tid('lcsc-import'))
    await settle(5)
    assertEq(importCalls().length, 1, 'import request sent')
    assertEq(harnessApi.closed, false, 'dialog stays open until the refresh finishes')
    assert(tid('lcsc-refreshing'), 'refresh progress is visible')
    assertEq(harnessApi.loadedLibraries, 0, 'later refreshes wait for the symbol refresh')
    assertEq(harnessApi.loadedStatus, 0, '统计刷新也在等符号刷新')

    // 1) symbols 解决（真实 loadSymbols 成功返回本次符号数组）→ libraries 还挂着，不能关闭
    symbolsGate.resolve([])
    harnessApi.gates.symbols = null
    await settle(6)
    assertEq(harnessApi.loadedLibraries, 1, 'libraries refreshed after the symbol refresh')
    assertEq(harnessApi.loadedStatus, 0, '统计刷新要等库列表刷新')
    assertEq(harnessApi.closed, false, 'libraries 还 pending：绝不能关闭')
    assert(tid('lcsc-refreshing'), '仍在刷新中')

    // 2) libraries 解决（真实 loadLibraries 成功返回本次数组）→ status 还挂着，不能关闭
    librariesGate.resolve([])
    harnessApi.gates.libraries = null
    await settle(6)
    assertEq(harnessApi.loadedStatus, 1, 'counters refreshed too')
    assertEq(harnessApi.closed, false, 'status 还 pending：绝不能关闭')
    assert(tid('lcsc-refreshing'), '仍在刷新中')

    // 3) status 解决（真实 loadStatus 成功返回状态对象）→ 三项都完成，才关闭
    statusGate.resolve({ symbols: 0, footprints: 0 })
    harnessApi.gates.status = null
    await settle(8)
    assertEq(harnessApi.closed, true, 'close happens after the refreshes complete')
    assertEq(importCalls().length, 1, '整个刷新过程只有一次导入')
  })

  await scenario('new: 已提交成功但刷新失败 → 不重复导入，可重试刷新', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    harnessApi.refreshFail = 'symbols offline'
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    click(tid('lcsc-import'))
    await settle(6)
    assertEq(harnessApi.closed, false, 'dialog stays open when the refresh fails')
    assert((tid('lcsc-refresh-error')?.textContent || '').includes('symbols offline'), 'refresh error shown')
    assertEq(tid('lcsc-import').disabled, true, 'the finished import cannot run again')
    assert(tid('lcsc-retry-refresh'), 'a refresh retry is offered')

    click(tid('lcsc-import'))
    await settle(2)
    assertEq(importCalls().length, 1, 'clicking import again sends nothing')
    harnessApi.refreshFail = ''
    const before = harnessApi.refreshedSymbols
    // 同一事件循环内连点两次：重试刷新必须同步防重，只能跑一次
    click(tid('lcsc-retry-refresh'))
    click(tid('lcsc-retry-refresh'))
    await settle(6)
    assertEq(harnessApi.closed, true, 'retry refresh succeeds and closes')
    assertEq(harnessApi.refreshedSymbols - before, 1, '双击重试只刷新一次')
    assertEq(importCalls().length, 1, 'the import request was never repeated')
  })

  // ---- R2：真实 useRequest 刷新契约（失败返回 null）----
  // 三个刷新回调来自真实 useRequest(...).load + 真实 fetch：接口失败/null 时
  // load 返回 null，弹窗必须把它当失败，不能当成功关闭。
  const REAL_CASES = [
    ['符号', '/api/symbols', () => ({ __status: 500, error: 'symbols offline' })],
    ['库列表', '/api/libraries', () => ({ __status: 500, error: 'libraries offline' })],
    ['统计', '/api/status', () => null],
  ]
  for (const [label, endpoint, badPayload] of REAL_CASES) {
    await scenario(`real: useRequest 刷新返回 null（${label}）→ 不关闭、不重复导入、只重试刷新`, async () => {
      await setup('real')
      await lookupOk()
      await clickTargetNext()
      click(tid(`lcsc-cand-${uuids.A}`))
      const before = {
        symbols: countCalls('/api/symbols'),
        libraries: countCalls('/api/libraries'),
        status: countCalls('/api/status'),
      }
      net.overrides[endpoint] = badPayload
      net.hold = false
      net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
      click(tid('lcsc-import'))
      await settle(10)
      assertEq(importCalls().length, 1, `${label}：只发一次 import`)
      assertEq(harnessApi.closed, false, `${label}：刷新失败（null）绝不能当成功关闭`)
      const text = tid('lcsc-refresh-error')?.textContent || ''
      assert(text.includes(label), `${label}：要指出是哪一项刷新失败，实际：${text}`)
      if (label === '符号') {
        assertEq(countCalls('/api/libraries'), before.libraries, '符号失败后不再继续刷新库列表')
        assertEq(countCalls('/api/status'), before.status, '符号失败后不再继续刷新统计')
      }
      if (label === '库列表') {
        assertEq(countCalls('/api/symbols'), before.symbols + 1, '符号刷新已经跑过')
        assertEq(countCalls('/api/status'), before.status, '库列表失败后不再继续刷新统计')
      }
      if (label === '统计') {
        assertEq(countCalls('/api/status'), before.status + 1, '统计刷新确实发出去过')
      }
      assert(tid('lcsc-retry-refresh'), `${label}：提供重试刷新入口`)
      assertEq(tid('lcsc-import').disabled, true, `${label}：不能再导入`)
      click(tid('lcsc-import'))
      await settle(2)
      assertEq(importCalls().length, 1, `${label}：重复导入被拦住`)
      // 放开接口后重试刷新：三个刷新都成功 → 关闭，且 import 始终只有一次
      delete net.overrides[endpoint]
      click(tid('lcsc-retry-refresh'))
      await settle(10)
      assertEq(harnessApi.closed, true, `${label}：重试成功后关闭`)
      assertEq(importCalls().length, 1, `${label}：重试刷新不会重复导入`)
    })
  }

  // ---- R3：成功写入后直到关闭全程冻结 ----
  await scenario('new: 成功写入后直到关闭全程冻结（刷新中/刷新失败，重开可导入另一个器件）', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    const gate = deferred()
    harnessApi.gates.symbols = gate
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    click(tid('lcsc-import'))
    await settle(6)
    const firstOp = importCalls()[0]?.body.operation_id
    assertEq(importCalls().length, 1, '写入一次')
    assert(tid('lcsc-refreshing'), '正在刷新')
    assertEq(harnessApi.closed, false, '刷新期间不关闭')

    // 刷新期间：编号 / 查找 / 候选 / 新建 / 手动搜索 / 返回 全部冻结
    assertEq(tid('lcsc-id-input').disabled, true, '刷新期间编号输入冻结')
    setInput(tid('lcsc-id-input'), 'C99999')
    await settle(2)
    assertEq(tid('lcsc-id-input').value, 'C37593', '刷新期间编号值不可改')
    enter(tid('lcsc-id-input'))
    click(tid('lcsc-lookup'))
    await settle(2)
    assertEq(analyzeCalls().length, 1, '刷新期间不会再发查找')
    assertEq(tid(`lcsc-cand-${uuids.A}`).querySelector('input').disabled, true, '候选在刷新期间冻结')
    click(tid(`lcsc-cand-${uuids.A}`))
    click(tid('lcsc-new-footprint'))
    click(tid('lcsc-back-target'))
    assertEq(tid('lcsc-manual-search').disabled, true, '手动搜索在刷新期间冻结')
    setInput(tid('lcsc-manual-search'), 'SOT')
    await settle(2)
    assert(tid('lcsc-candidates'), '仍停在封装确认步（返回无效）')
    assertEq($$('input[type=radio]:checked').length, 1, '刷新期间选择没有被改动')
    assertEq(importCalls().length, 1, '刷新期间没有新提交')
    escape(); click(modalCloseButton()); click($('.modal-overlay')); click(tid('lcsc-close'))
    await settle(2)
    assertEq(harnessApi.closed, false, '刷新期间 Escape/X/遮罩/取消都不能关闭')
    assert($('.modal'), '弹窗仍在')

    // 刷新失败：仍然冻结，只允许“重试刷新”或明确关闭
    gate.reject(new Error('symbols offline'))
    harnessApi.gates.symbols = null
    await settle(8)
    assert(tid('lcsc-refresh-error'), '刷新失败要说明原因')
    assertEq(harnessApi.closed, false, '刷新失败不自动关闭')
    assertEq(tid('lcsc-import').disabled, true, '刷新失败后不能再导入')
    setInput(tid('lcsc-id-input'), 'C88888')
    enter(tid('lcsc-id-input'))
    click(tid('lcsc-back-target'))
    click(tid('lcsc-new-footprint'))
    await settle(2)
    assertEq(tid('lcsc-id-input').value, 'C37593', '刷新失败后编号仍不可改')
    assertEq(analyzeCalls().length, 1, '刷新失败后不再发查找')
    assertEq(importCalls().length, 1, '始终只发一次 import')
    assertEq($$('input[type=radio]:checked').length, 1, '选择仍未被改动')

    // 明确关闭 → 重开可以导入另一个器件（completed 不能把新流程永久锁死）
    click(tid('lcsc-close'))
    await settle(2)
    assertEq(harnessApi.closed, true, '刷新失败后允许明确关闭')
    harnessApi.closed = false
    await setup('new')
    await lookupOk({ lcsc_id: 'C88888' }, 'C88888')
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.B}`))
    net.hold = false
    net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'reuse' } })
    click(tid('lcsc-import'))
    await settle(8)
    assertEq(importCalls().length, 2, '重开后可以导入另一个器件')
    const second = importCalls().at(-1)
    assertEq(second.body.lcsc_id, 'C88888', '第二个器件的编号')
    assert(second.body.operation_id !== firstOp, '新流程使用新的操作 ID')
    assertEq(harnessApi.closed, true, '第二次导入刷新成功后关闭')
  })

  await scenario('new: 提交前刷新发现包被锁定/删除/刷新失败 → 阻断并说明原因', async () => {
    for (const [label, override, expected] of [
      ['锁定', () => [{ ...WORKSPACES[0], locked: true }], '锁定'],
      ['删除', () => [], '不存在'],
      ['刷新失败', () => { throw new Error('offline') }, ''],
    ]) {
      net.overrides = {}
      harnessApi.compLibraries = WORKSPACES.slice()
      await setup('new')
      await lookupOk()
      await clickTargetNext()
      click(tid(`lcsc-cand-${uuids.A}`))
      net.overrides['/api/component-libraries'] = override
      net.hold = true
      click(tid('lcsc-import'))
      await settle(5)
      assertEq(importCalls().length, 0, `${label}：不得发出 import 请求`)
      assert(tid('lcsc-import-error'), `${label}：要显示阻断原因`)
      if (expected) {
        assert((tid('lcsc-import-error').textContent || '').includes(expected), `${label}：原因文案`)
      }
      assertEq(harnessApi.closed, false, `${label}：弹窗保持打开`)
    }
  })

  await scenario('new: 刷新后包配置变化 → 停下更新摘要并要求重新确认', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    net.overrides['/api/component-libraries'] = () => [
      { ...WORKSPACES[0], name: '改名后的库', symbol_path: 'E:\\other\\symbols' },
    ]
    click(tid('lcsc-import'))
    await settle(5)
    assertEq(importCalls().length, 0, 'changed package is never written without a fresh confirmation')
    const summary = tid('lcsc-target-list')?.textContent || ''
    assert(summary.includes('改名后的库'), 'target selection updated to the fresh package')
    assert(summary.includes('E:\\other\\symbols'), 'selection shows the fresh symbol path')
    assert(!summary.includes('主元件库'), 'stale package name is gone')
    assert(tid('lcsc-import-error'), 'user is asked to confirm again')
    assert(!tid('lcsc-candidates'), 'changed package requires fresh symbol and footprint confirmation')

    // 用新信息重新确认后可以正常提交（operation_id 也必须换新）
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    click(tid('lcsc-import'))
    await settle(3)
    const resent = importCalls().at(-1)
    assert(resent, 'after re-confirming, the import goes through')
    assertEq(resent.body.target_component_library_id, 'pkg-ok', 'fresh package id')
  })

  await scenario('new: 返回上一步清掉不适用的封装选择', async () => {
    await setup('new')
    await lookupOk()
    await clickTargetNext()
    click(tid(`lcsc-cand-${uuids.A}`))
    click(tid('lcsc-back-target'))
    await settle()
    assert(tid('lcsc-symbol-list'), 'back to the symbol library step')
    click(tid('lcsc-symbol-back'))
    assert(tid('lcsc-target-list'), 'can continue back to the package step')
    await clickTargetNext()
    assertEq(tid('lcsc-import')?.disabled, true, 'previous footprint choice is dropped on target change')
    assertEq($$('input[type=radio]:checked').length, 0, 'nothing selected after going back')
  })

  await scenario('new: 手动搜索不自动选第一条', async () => {
    await setup('new')
    await lookupOk({ candidates: [] })
    await clickTargetNext()
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'SOT-223' }),
      live({ id: uuids.B, library: 'main', name: 'SOT-23' }),
    ]
    setInput(tid('lcsc-manual-search'), 'SOT')
    await settle(4)
    const rows = $$('label[data-testid^="lcsc-manual-"]')
    assertEq(rows.length, 2, 'manual results rendered')
    assertEq(tid('lcsc-import').disabled, true, 'manual search alone does not pick a footprint')
    click(rows[1])
    await settle()
    assertEq(tid('lcsc-import').disabled, false, 'manual pick enables import')
  })

  await scenario('new: 手动搜索乱序（第二个先返回）不覆盖最新结果', async () => {
    await setup('new')
    await lookupOk({ candidates: [] })
    await clickTargetNext()
    const first = deferred()
    const second = deferred()
    net.overrides['/api/footprints/search'] = () => first.promise
    setInput(tid('lcsc-manual-search'), 'SOT-2')
    await settle(2)
    net.overrides['/api/footprints/search'] = () => second.promise
    setInput(tid('lcsc-manual-search'), 'SOT-3')
    await settle(2)
    second.resolve([live({ id: uuids.B, library: 'main', name: 'NEW' })])
    await settle(3)
    assert((tid('lcsc-manual-results')?.textContent || '').includes('NEW'), 'newest manual search shown')
    first.resolve([live({ id: uuids.A, library: 'main', name: 'OLD' })])
    await settle(3)
    const text = tid('lcsc-manual-results')?.textContent || ''
    assert(text.includes('NEW'), 'first-started response must not replace the newest results')
    assert(!text.includes('OLD'), 'stale manual results are dropped')
    assertEq(tid('lcsc-manual-search').disabled, false, 'loading flag is not stuck after the stale finally')
  })

  // ---------- 存在性状态：真实列表/详情/设置组件（受控 API 状态 mock） ----------
  // 服务端状态是唯一判据：C++ 的真实临时文件测试负责「存在性」证据，
  // 这里负责「界面拿到状态后怎么显示」，两者不互相冒充。
  await scenario('model: inline 导入跨面板完成后，可明确开始第二个编号且不重发旧意图', async () => {
    const { createLcscModel } = await import('../../../plugins/lcsc_import/frontend/src/model.js')
    const View = (await import('../../../plugins/lcsc_import/frontend/src/LcscImportModal.jsx')).default
    let refreshes = 0
    const model = createLcscModel({ api: lcscApi, pluginAction: PLUGIN_ACTION,
      compLibraries: WORKSPACES.slice(), onRefresh: async () => { refreshes++ } })
    const Host = () => {
      const [shown, setShown] = useState(true)
      return <><button data-testid="toggle-inline" onClick={() => setShown(value => !value)}>toggle</button>
        {shown && <View inline model={model} />}</>
    }
    try {
      mountElement(<Host />)
      await lookupOk()
      await clickTargetNext()
      click(tid('lcsc-new-footprint'))
      const pending = deferred()
      net.queue.push(pending.promise)
      click(tid('lcsc-import')); await settle(5)
      assertEq(importCalls().length, 1, '第一次确认只提交一次')
      const firstOperation = importCalls()[0].body.operation_id
      click(tid('toggle-inline')); await settle(3)
      assert(!tid('lcsc-inline'), '面板已卸载，模型仍持有业务任务')
      pending.resolve({ ok: true, plugin_result: { ok: true, mode: 'new' } }); await settle(6)
      click(tid('toggle-inline')); await settle(4)
      assert(tid('lcsc-next-import'), '重开面板显示完成状态和下一次导入入口')
      assertEq(refreshes, 1, '面板关闭期间成功只刷新一次')
      click(tid('lcsc-next-import'))
      assertEq(tid('lcsc-id-input').value, '', '新意图清空旧编号')
      assertEq(tid('lcsc-id-input').disabled, false, '新意图允许编辑')
      await lookupOk({}, 'C37594'); await clickTargetNext()
      click(tid('lcsc-new-footprint'))
      net.queue.push({ ok: true, plugin_result: { ok: true, mode: 'new' } })
      click(tid('lcsc-import')); await settle(6)
      assertEq(importCalls().length, 2, '两个明确意图各提交一次')
      assert(importCalls()[1].body.operation_id !== firstOperation, '第二次使用新的operation ID')
      assertEq(refreshes, 2, '每个意图各刷新一次')
    } finally { unmount(); model.dispose() }
  })

  await scenario('status: 列表圆点与问题筛选只看服务端状态（旧字段为 true 也照样算问题）', async () => {
    const { default: SymbolTable } = await import('../../src/components/SymbolTable.jsx')
    const ghostRef = 'My_Library:SW-SMD_SS-0323PG-5N5WW'
    const symbols = [
      { id: 'sym-good', name: 'GOOD', footprint: 'main:GOOD', footprint_status: 'available', model_status: 'available', has_footprint: true, has_3d_model: true },
      // 用户示例：引用非空、旧 has_footprint/has_3d_model 都是 true，但本地没有这条记录
      { id: 'sym-ghost', name: 'SS-0323PG', footprint: ghostRef, footprint_status: 'not_found', footprint_reason: 'no_match', has_footprint: true, has_3d_model: true },
      { id: 'sym-stale', name: 'STALE', footprint: 'main:OLD', footprint_status: 'stale', footprint_reason: 'missing_file', model_status: 'stale', model_reason: 'footprint_unavailable', has_footprint: true, has_3d_model: true },
    ]
    mountElement(<SymbolTable symbols={symbols} filter={null} issueFilter={false} />)
    await settle()
    assertEq(tid('symbol-status-sym-good').getAttribute('data-status'), 'available')
    assert(tid('symbol-status-sym-good').classList.contains('ok'), '正常项是绿色')
    assertEq(tid('symbol-status-sym-ghost').getAttribute('data-status'), 'not_found')
    assert(!tid('symbol-status-sym-ghost').classList.contains('ok'), '未找到不能显示绿色')
    assert(tid('symbol-status-sym-ghost').classList.contains('bad'), '未找到显示为问题')
    assertEq(tid('symbol-status-sym-stale').getAttribute('data-status'), 'stale')
    assert(!tid('symbol-status-sym-stale').classList.contains('ok'), '失效不能显示绿色')
    assert(($('#root').textContent || '').includes(ghostRef), '原始引用照样显示')

    // App 已在父层按相同结果集筛选并维护选择；SymbolTable 渲染传入结果。
    // 此组件场景复用生产状态判定，不把筛选职责移回列表组件。
    mountElement(<SymbolTable symbols={symbols.filter(isProblemSymbol)} filter={null} issueFilter />)
    await settle()
    assert(tid('symbol-status-sym-ghost'), '问题筛选包含未找到的元件')
    assert(tid('symbol-status-sym-stale'), '问题筛选包含失效的元件')
    assert(!tid('symbol-status-sym-good'), '正常元件不在问题列表里')
    unmount()
  })

  await scenario('status: Inspector 用绑定状态显示未找到/加载中/失败，并给更换入口', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const ghostRef = 'My_Library:SW-SMD_SS-0323PG-5N5WW'
    const symbol = { id: 'sym-ghost', name: 'SS-0323PG', footprint: ghostRef, type: 'Connector' }
    const field = () => $('[data-testid^="fp-state-"]')
    const modelField = () => $('[data-testid^="model-state-"]')

    // 1) 加载中：既不是正常，也不是未找到
    const gate = deferred()
    net.overrides['/api/symbols/sym-ghost/binding'] = () => gate.promise
    mountElement(<Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} />)
    await settle()
    assertEq(field().getAttribute('data-status'), 'loading', '加载中状态')
    assert(!field().classList.contains('ok'), '加载中不能显示正常')
    assert((field().textContent || '').includes(ghostRef), '加载中也显示原始引用')
    assert(!tid('inspector-replace-footprint'), '加载中不给更换入口（还没结论）')

    // 2) 未找到：原始引用保留 + 失效文案 + 更换入口；旧布尔字段为 true 也不改结论
    gate.resolve({
      footprint_id: '', footprint_name: '',
      footprint_status: 'not_found', footprint_reason: 'no_match',
      footprint_reference: ghostRef, footprint_resolved_reference: '',
      has_footprint: true, has_3d_model: true,
      model_status: 'unassigned', model_reason: 'no_model_link', model_id: '', model_name: '',
    })
    await settle(4)
    assertEq(field().getAttribute('data-status'), 'not_found')
    assert(!field().classList.contains('ok'), '未找到不显示正常')
    assert((field().textContent || '').includes(ghostRef), '错误状态也不清空原始引用')
    assert((field().textContent || '').includes('未找到本地封装'), '给出未找到文案')
    assert(!modelField().classList.contains('ok'), '封装失效时 3D 也不能显示正常')
    assert(tid('inspector-replace-footprint'), '未找到要有更换封装入口')

    // 3) 请求失败：检查失败，不沿用任何正常状态
    net.overrides['/api/symbols/sym-ghost/binding'] = () => ({ __status: 500, error: 'boom' })
    mountElement(<Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} />)
    await settle(4)
    assertEq(field().getAttribute('data-status'), 'error')
    assert(!field().classList.contains('ok'), '请求失败不能显示正常')
    assert((field().textContent || '').includes('检查失败'), '区分「检查失败」与「未找到」')
    assert((field().textContent || '').includes(ghostRef), '失败也保留原始引用')
    unmount()
  })

  await scenario('status: Inspector 更换入口不提交失效/未检查候选，失败保留弹窗并报出原因', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const deadId = 'dddddddd-4444-4444-8444-444444444444'
    const toasts = []
    let refreshes = 0
    const symbol = {
      id: 'sym-replace', name: 'U2', footprint: 'main:OLD', type: 'Connector',
      footprint_status: 'stale', footprint_reason: 'missing_file',
    }
    const bindCalls = () => countCalls('/bind-footprint')
    const mountInspector = () => mountElement(
      <Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}}
        onRefresh={() => { refreshes += 1 }} toastMsg={(m) => toasts.push(m)} />
    )
    const openReplace = async () => {
      assert(tid('inspector-replace-footprint'), '失效封装要有更换入口')
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert($('.search-popover-input'), '更换入口要打开候选弹窗')
      const dead = $$('.search-popover-item').find(e => (e.textContent || '').includes('DEAD'))
      const nostatus = $$('.search-popover-item').find(e => (e.textContent || '').includes('NO-STATUS'))
      const good = $$('.search-popover-item').find(e => (e.textContent || '').includes('GOOD'))
      assert(dead && nostatus && good, `三个候选都要显示：${$$('.search-popover-item').map(e => e.textContent).join('|')}`)
      return { dead, nostatus, good }
    }

    // 候选：stale 的、没有状态的（未检查）、正常的
    net.overrides['/api/symbols/sym-replace/binding'] = () => ({
      footprint_id: 'old-uuid', footprint_name: 'OLD',
      footprint_reference: 'main:OLD', footprint_status: 'stale', footprint_reason: 'missing_file',
      model_status: 'unassigned', model_reason: 'no_model_link',
    })
    net.overrides['/api/footprints/search'] = () => [
      { id: deadId, library: 'main', name: 'DEAD', pad_count: 4, footprint_status: 'stale', footprint_reason: 'missing_file' },
      { id: 'eeeeeeee-5555-4555-8555-555555555555', library: 'main', name: 'NO-STATUS', pad_count: 4 },
      live({ id: uuids.A, library: 'main', name: 'GOOD', pad_count: 4 }),
    ]
    net.overrides['/api/symbols/sym-replace/bind-footprint'] = () => ({ ok: false, error: 'REJECTED-REASON' })

    // 1) 失效/未检查候选点了也不能提交绑定，弹窗要留着并说明原因
    mountInspector()
    await settle(4)
    let items = await openReplace()
    assert((items.dead.textContent || '').includes('封装文件已失效'), `失效候选要给出原因，实际：${items.dead.textContent}`)
    const beforeStale = bindCalls()
    click(items.dead)
    await settle(4)
    assertEq(bindCalls(), beforeStale, '点失效候选不能调用绑定')
    click(items.nostatus)
    await settle(4)
    assertEq(bindCalls(), beforeStale, '点未检查候选不能调用绑定')
    assert($('.search-popover-input'), '被拒绝的选择不关闭弹窗')
    unmount()

    // 2) 正常候选遇到 200 + ok:false：保留弹窗、toast 给出服务端原因、不刷新成假成功
    mountInspector()
    await settle(4)
    items = await openReplace()
    const beforeGood = bindCalls()
    click(items.good)
    await settle(4)
    assertEq(bindCalls(), beforeGood + 1, '正常候选要真的提交一次绑定')
    assert($('.search-popover-input'), '绑定失败必须保留弹窗')
    assert(toasts.some(m => String(m).includes('REJECTED-REASON')),
      `toast 要显示具体失败原因，实际：${JSON.stringify(toasts)}`)
    assertEq(refreshes, 0, '绑定失败不能触发刷新假装成功')
    unmount()

    // 3) HTTP 500：同样保留弹窗并把服务端原因告诉用户
    net.overrides['/api/symbols/sym-replace/bind-footprint'] = () => ({ __status: 500, error: 'SERVER-DOWN' })
    mountInspector()
    await settle(4)
    items = await openReplace()
    click(items.good)
    await settle(4)
    assert($('.search-popover-input'), 'HTTP 失败也要保留弹窗')
    assert(toasts.some(m => String(m).includes('SERVER-DOWN')),
      `HTTP 失败要显示服务端原因，实际：${JSON.stringify(toasts)}`)
    assertEq(refreshes, 0, 'HTTP 失败不能触发刷新假装成功')
    unmount()
  })

  await scenario('status: 候选搜索失败时显示真实错误，不伪装成没有候选', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const symbol = {
      id: 'sym-search-fail', name: 'U4', footprint: 'main:OLD', type: 'Connector',
      footprint_status: 'stale', footprint_reason: 'missing_file',
    }
    net.overrides['/api/symbols/sym-search-fail/binding'] = () => ({
      footprint_id: 'old-uuid', footprint_name: 'OLD',
      footprint_reference: 'main:OLD', footprint_status: 'stale', footprint_reason: 'missing_file',
      model_status: 'unassigned', model_reason: 'no_model_link',
    })
    net.overrides['/api/footprints/search'] = () => ({ __status: 500, error: 'SEARCH-DOWN' })
    mountElement(<Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}}
      onRefresh={() => {}} toastMsg={() => {}} />)
    await settle(4)
    assert(tid('inspector-replace-footprint'), '失效封装要有更换入口')
    click(tid('inspector-replace-footprint'))
    await settle(4)
    const error = tid('search-popover-error')
    assert(error, '搜索失败要显示真实错误，不能只显示空列表')
    assert((error.textContent || '').includes('SEARCH-DOWN'), `要显示服务端原因：${error.textContent}`)
    assertEq($$('.search-popover-item').length, 0, '失败时不能出现伪造的候选')
    unmount()
  })

  await scenario('status: 绑定请求未完成或失败时，设置弹窗不显示封装/模型正常', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    // 列表里的选中项还是 available 的旧快照：设置弹窗不能拿它当结论
    const symbol = {
      id: 'sym-settings', name: 'U3', footprint: 'main:OK', type: 'Connector',
      footprint_status: 'available', footprint_reason: 'ok',
      model_status: 'available', model_reason: 'ok', model_name: 'ok.step',
    }
    net.overrides['/api/footprints/search'] = () => []
    net.overrides['/api/models/search'] = () => []
    const openSettings = async () => {
      // 生产组件的设置入口只有 className="inspector-gear"（没有 data-testid），
      // 这里按真实选择器点击真实齿轮；设置字段的 data-testid 由 SymbolSettings 提供。
      const gear = $('.inspector-gear')
      assert(gear, '要有设置入口')
      click(gear)
      await settle(4)
      const fp = tid('settings-current-footprint')
      const model = tid('settings-current-model')
      assert(fp && model, '设置弹窗要显示当前封装与模型')
      return { fp, model }
    }

    // 1) 绑定请求还挂着
    const gate = deferred()
    net.overrides['/api/symbols/sym-settings/binding'] = () => gate.promise
    mountElement(<Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} />)
    await settle(2)
    let current = await openSettings()
    assert(current.fp.getAttribute('data-status') !== 'available',
      `请求未完成不能显示封装正常，实际 ${current.fp.getAttribute('data-status')}`)
    assert(!(current.fp.textContent || '').includes('封装正常'), `不能显示封装正常：${current.fp.textContent}`)
    assert(current.model.getAttribute('data-status') !== 'available',
      `请求未完成不能显示模型正常，实际 ${current.model.getAttribute('data-status')}`)
    assert(!(current.model.textContent || '').includes('3D 模型正常'), `不能显示模型正常：${current.model.textContent}`)
    gate.resolve({
      footprint_id: '', footprint_name: '', footprint_reference: 'main:OK',
      footprint_status: 'stale', footprint_reason: 'missing_file',
      model_status: 'stale', model_reason: 'footprint_unavailable',
    })
    await settle(4)
    unmount()

    // 2) 绑定请求 500：设置弹窗要保留失败原因，不能回落成「没绑定」的平静样子。
    // 服务端给的原因（DB-DOWN）由真实 api.js 从错误体里取出、真实 Inspector 保存，
    // 界面必须把它显示出来 —— 只显示一句通用「检查失败」等于把故障原因丢了。
    net.overrides['/api/symbols/sym-settings/binding'] = () => ({ __status: 500, error: 'DB-DOWN' })
    mountElement(<Inspector selected={symbol} classify={() => {}} onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} />)
    await settle(4)
    const inspectorFp = $('[data-testid^="fp-state-"]')
    assertEq(inspectorFp.getAttribute('data-status'), 'error', '请求失败在 Inspector 里是「检查失败」')
    assert((inspectorFp.textContent || '').includes('DB-DOWN'),
      `Inspector 要显示服务端给出的原因，不能压成布尔：${inspectorFp.textContent}`)
    current = await openSettings()
    assert(current.fp.getAttribute('data-status') !== 'available',
      `请求失败不能显示封装正常，实际 ${current.fp.getAttribute('data-status')}`)
    assert(!(current.fp.textContent || '').includes('封装正常'), `不能显示封装正常：${current.fp.textContent}`)
    const failedText = current.fp.textContent || ''
    assert(failedText.includes('检查失败') || failedText.includes('无法检查'),
      `要保留「检查失败」而不是变成没绑定：${failedText}`)
    assert(failedText.includes('DB-DOWN'),
      `设置弹窗要显示服务端给出的具体原因，不能只剩通用文案：${failedText}`)
    assert(current.model.getAttribute('data-status') !== 'available',
      `请求失败不能显示模型正常，实际 ${current.model.getAttribute('data-status')}`)
    assert(!(current.model.textContent || '').includes('3D 模型正常'), `不能显示模型正常：${current.model.textContent}`)
    unmount()
  })

  await scenario('status: 迟到的绑定响应不覆盖另一个选中符号', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const gateA = deferred()
    const symA = { id: 'sym-order-a', name: 'A', footprint: 'A:REF', type: 'Connector' }
    const symB = { id: 'sym-order-b', name: 'B', footprint: 'B:REF', type: 'Connector' }
    net.overrides['/api/symbols/sym-order-a/binding'] = () => gateA.promise
    net.overrides['/api/symbols/sym-order-b/binding'] = () => ({
      footprint_reference: 'B:REF', footprint_status: 'not_found', footprint_reason: 'no_match',
      model_status: 'unassigned', model_reason: 'no_model_link',
    })
    const Switch = () => {
      const [sel, setSel] = useState(symA)
      return (
        <div>
          <button data-testid="switch-symbol" onClick={() => setSel(symB)}>switch</button>
          <Inspector selected={sel} classify={() => {}} onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} />
        </div>
      )
    }
    mountElement(<Switch />)
    await settle(2)
    click(tid('switch-symbol'))
    await settle(4)
    const field = () => $('[data-testid^="fp-state-"]')
    assertEq(field().getAttribute('data-status'), 'not_found', '切到 B 后显示 B 的状态')
    // A 的迟到响应在这之后才回来：不能覆盖 B 的显示
    gateA.resolve({
      footprint_reference: 'A:REF', footprint_status: 'available', footprint_reason: 'ok',
      model_status: 'available', model_reason: 'ok',
    })
    await settle(4)
    assertEq(field().getAttribute('data-status'), 'not_found', 'A 的迟到响应不能覆盖 B')
    assert((field().textContent || '').includes('B:REF'), `显示的必须还是 B 的引用：${field().textContent}`)
    unmount()
  })

  // 下面五条都是「写入/搜索请求的迟到响应」：getBinding 的乱序已有用例，这里补的是
  // 选中即提交（onSelect）与候选搜索在换符号后的会话隔离 —— 真实 Inspector +
  // 真实 api.js，只把 fetch 换成可控实现；每个绑定请求各有一个独立 gate。
  const lateSymbol = (id, name, ref) => ({
    id, name, footprint: ref, type: 'Connector',
    footprint_status: 'stale', footprint_reason: 'missing_file',
  })
  const staleBinding = (ref) => ({
    footprint_id: '', footprint_name: '', footprint_reference: ref,
    footprint_status: 'stale', footprint_reason: 'missing_file',
    model_status: 'unassigned', model_reason: 'no_model_link',
  })
  const itemByName = (name) => {
    const item = $$('.search-popover-item').find(e => (e.textContent || '').includes(name))
    assert(item, `候选 ${name} 必须显示：${$$('.search-popover-item').map(e => e.textContent).join('|')}`)
    return item
  }
  // 受控宿主：真实 useState 切换选中符号，Inspector 是生产组件本体
  const selectionHost = (Inspector, symA, symB, onRefresh, onToast) => () => {
    const [sel, setSel] = useState('a')
    return (
      <div>
        <button data-testid="select-a" onClick={() => setSel('a')}>A</button>
        <button data-testid="select-b" onClick={() => setSel('b')}>B</button>
        <Inspector selected={sel === 'a' ? symA : symB} classify={() => {}}
          onDeleteSymbol={() => {}} onRefresh={onRefresh} toastMsg={onToast} />
      </div>
    )
  }

  await scenario('status: A 的迟到成功不能关掉 B 的候选弹窗，也不能释放 B 的提交锁', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-late-a', B = 'sym-late-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    const bGates = gatedWrites(`/api/symbols/${B}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-GOOD', pad_count: 4 }),
      live({ id: uuids.B, library: 'main', name: 'B-GOOD', pad_count: 4 }),
    ]
    const toasts = []
    let refreshes = 0
    const bindsB = () => countCalls(`/api/symbols/${B}/bind-footprint`)
    const getsB = () => countCalls(`/api/symbols/${B}/binding`)
    const Host = selectionHost(Inspector, symA, symB, () => { refreshes += 1 }, (m) => toasts.push(m))

    try {
      mountElement(<Host />)
      await settle(4)
      // A：打开更换弹窗 → 点正常候选，写入挂起（还没回来）
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-GOOD'))
      await settle(2)
      assertEq(aGates.length, 1, 'A 的候选点击要发出一次绑定并挂起')
      // 切到 B：B 的绑定快照先到（stale → 有更换入口），再选 B 的候选
      click(tid('select-b'))
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bGates.length, 1, 'B 的候选点击要发出一次绑定并挂起')
      const getsBefore = getsB()
      // A 的成功现在才回来：这只属于旧会话，不能动 B 的界面
      aGates[0].resolve({ ok: true })
      await settle(4)
      assertEq(getsB(), getsBefore, 'A 的迟到成功不能让 B 重新拉绑定')
      assertEq(refreshes, 0, 'A 的迟到成功不能触发刷新')
      assert($('.search-popover-input'), 'B 的候选弹窗必须保留')
      // B 还没返回：重复点击不能发第二次写入（旧请求的 finally 不能释放 B 的锁）
      const bindsBeforeRepeat = bindsB()
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bindsB(), bindsBeforeRepeat, 'B 的写入还没结束，再点一次不能发出第二次绑定')
      // B 自己成功才收尾
      bGates[0].resolve({ ok: true })
      await settle(4)
      assert(!$('.search-popover-input'), 'B 成功后才关闭候选弹窗')
      assertEq(refreshes, 1, 'B 成功才触发一次刷新')
      assertEq(toasts.length, 0, `整个过程不该出现错误提示：${JSON.stringify(toasts)}`)
    } finally {
      await finishControlled([() => aGates.releaseAll(), () => bGates.releaseAll()])
    }
  })

  await scenario('status: A 的迟到业务失败不能给 B 报错，也不能释放 B 的提交锁', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-reject-a', B = 'sym-reject-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    const bGates = gatedWrites(`/api/symbols/${B}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-GOOD', pad_count: 4 }),
      live({ id: uuids.B, library: 'main', name: 'B-GOOD', pad_count: 4 }),
    ]
    const toasts = []
    let refreshes = 0
    const bindsB = () => countCalls(`/api/symbols/${B}/bind-footprint`)
    const Host = selectionHost(Inspector, symA, symB, () => { refreshes += 1 }, (m) => toasts.push(m))

    try {
      mountElement(<Host />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-GOOD'))
      await settle(2)
      assertEq(aGates.length, 1, 'A 的候选点击要发出一次绑定并挂起')
      click(tid('select-b'))
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bGates.length, 1, 'B 的候选点击要发出一次绑定并挂起')
      // A 被服务端拒绝（200 + ok:false）：这是 A 的结果，不能报给 B 的会话
      aGates[0].resolve({ ok: false, error: 'OLD-A-REJECTED' })
      await settle(4)
      assert(!toasts.some(m => String(m).includes('OLD-A-REJECTED')),
        `A 的业务失败不能 toast 到 B 的界面：${JSON.stringify(toasts)}`)
      assertEq(refreshes, 0, 'A 的失败不能触发刷新')
      assert($('.search-popover-input'), 'B 的候选弹窗必须保留')
      const bindsBeforeRepeat = bindsB()
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bindsB(), bindsBeforeRepeat, 'A 的 finally 不能释放 B 的提交锁')
      bGates[0].resolve({ ok: true })
      await settle(4)
      assert(!$('.search-popover-input'), 'B 成功后才关闭候选弹窗')
      assertEq(refreshes, 1, 'B 成功才触发一次刷新')
    } finally {
      await finishControlled([() => aGates.releaseAll(), () => bGates.releaseAll()])
    }
  })

  await scenario('status: A 的迟到 HTTP 失败不能给 B 报错，也不能释放 B 的提交锁', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-http-a', B = 'sym-http-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    const bGates = gatedWrites(`/api/symbols/${B}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-GOOD', pad_count: 4 }),
      live({ id: uuids.B, library: 'main', name: 'B-GOOD', pad_count: 4 }),
    ]
    const toasts = []
    let refreshes = 0
    const bindsB = () => countCalls(`/api/symbols/${B}/bind-footprint`)
    const Host = selectionHost(Inspector, symA, symB, () => { refreshes += 1 }, (m) => toasts.push(m))

    try {
      mountElement(<Host />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-GOOD'))
      await settle(2)
      assertEq(aGates.length, 1, 'A 的候选点击要发出一次绑定并挂起')
      click(tid('select-b'))
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bGates.length, 1, 'B 的候选点击要发出一次绑定并挂起')
      // A 是 HTTP 500（异常路径，不是 ok:false）：同样只属于旧会话
      aGates[0].resolve({ __status: 500, error: 'A-SERVER-DOWN' })
      await settle(4)
      assert(!toasts.some(m => String(m).includes('A-SERVER-DOWN')),
        `A 的 HTTP 失败不能 toast 到 B 的界面：${JSON.stringify(toasts)}`)
      assertEq(refreshes, 0, 'A 的失败不能触发刷新')
      assert($('.search-popover-input'), 'B 的候选弹窗必须保留')
      const bindsBeforeRepeat = bindsB()
      click(itemByName('B-GOOD'))
      await settle(2)
      assertEq(bindsB(), bindsBeforeRepeat, 'A 的 finally 不能释放 B 的提交锁')
      bGates[0].resolve({ ok: true })
      await settle(4)
      assert(!$('.search-popover-input'), 'B 成功后才关闭候选弹窗')
      assertEq(refreshes, 1, 'B 成功才触发一次刷新')
    } finally {
      await finishControlled([() => aGates.releaseAll(), () => bGates.releaseAll()])
    }
  })

  // 换符号后的界面隔离拆成两条：一条只管「切走就清」（不发新查询），
  // 一条只管「旧会话挂起的搜索晚回」（不要求挂起期间旧结果还在，清不清都由实现决定）。
  await scenario('status: 换符号后旧候选弹窗/候选立即清掉，不让新符号看到旧列表', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-clear-a', B = 'sym-clear-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-ONLY', pad_count: 4 }),
    ]
    const Host = selectionHost(Inspector, symA, symB, () => {}, () => {})

    try {
      mountElement(<Host />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert(itemByName('A-ONLY'), 'A 的候选要真的显示出来（这才是要清掉的东西）')
      // 切到 B：旧选择会话的弹窗/候选必须立刻消失，不能让 B 看到 A 的列表
      click(tid('select-b'))
      await settle(4)
      const stillOpen = $('.search-popover-input')
      const staleItems = $$('.search-popover-item')
      assert(!stillOpen || staleItems.length === 0,
        `切到 B 后旧的候选弹窗/候选必须清掉：open=${!!stillOpen} `
        + `items=${staleItems.map(e => e.textContent).join('|')}`)
    } finally {
      await finishControlled()
    }
  })

  await scenario('status: 旧会话挂起的候选搜索迟到返回时不混进新符号', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-sess-a', B = 'sym-sess-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    const lateSearch = deferred()
    let switched = false
    let firstSearch = true
    net.overrides['/api/footprints/search'] = () => {
      if (switched) return [live({ id: uuids.B, library: 'main', name: 'B-ONLY', pad_count: 4 })]
      if (firstSearch) {
        firstSearch = false
        return [live({ id: uuids.A, library: 'main', name: 'A-ONLY', pad_count: 4 })]
      }
      return lateSearch.promise   // A 的第二次搜索：故意挂到切走之后才回
    }
    let refreshes = 0
    const Host = selectionHost(Inspector, symA, symB, () => { refreshes += 1 }, () => {})

    try {
      mountElement(<Host />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert(itemByName('A-ONLY'), 'A 的候选要真的显示出来')
      // A 会话再发一次搜索并挂起：这时要不要清掉已显示的旧结果由实现决定，不作断言
      setInput($('.search-popover-input'), 'A2')
      await settle(2)
      // 切到 B，让 B 先拿到自己的候选，之后才放 A 的迟到搜索回来
      switched = true
      click(tid('select-b'))
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert(itemByName('B-ONLY'), 'B 的候选要显示')
      lateSearch.resolve([live({ id: uuids.A, library: 'main', name: 'A-LATE', pad_count: 4 })])
      await settle(4)
      const items = $$('.search-popover-item').map(e => e.textContent || '')
      assert(items.length === 1 && items[0].includes('B-ONLY'),
        `迟到的 A 搜索结果不能混进 B 的候选列表：${items.join('|')}`)
      assertEq(refreshes, 0, '搜索/选择都不该触发刷新')
    } finally {
      await finishControlled([() => lateSearch.resolve([])])
    }
  })

  await scenario('status: A→B→A 之后，旧 A 的迟到成功不能作用于新 A 的弹窗与提交锁', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-round-a', B = 'sym-round-b'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    const symB = lateSymbol(B, 'B', 'main:OLD-B')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    net.overrides[`/api/symbols/${B}/binding`] = () => staleBinding('main:OLD-B')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-FIRST', pad_count: 4 }),
      live({ id: 'eeeeeeee-5555-4555-8555-555555555555', library: 'main', name: 'A-SECOND', pad_count: 4 }),
    ]
    const toasts = []
    let refreshes = 0
    const bindsA = () => countCalls(`/api/symbols/${A}/bind-footprint`)
    const getsA = () => countCalls(`/api/symbols/${A}/binding`)
    const Host = selectionHost(Inspector, symA, symB, () => { refreshes += 1 }, (m) => toasts.push(m))

    try {
      mountElement(<Host />)
      await settle(4)
      // 第一次 A 会话：点候选，写入挂起（对应「已提交到旧目标」的那次请求）
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-FIRST'))
      await settle(2)
      assertEq(aGates.length, 1, '第一次 A 的写入要发出并挂起')
      // A → B（必须真的切走过，才不是同一个交互会话）
      click(tid('select-b'))
      await settle(4)
      assert(($('[data-testid^="fp-state-"]').textContent || '').includes('main:OLD-B'),
        '必须真的显示过 B 的状态')
      // B → A：回到 A 是一个新的交互会话，打开新弹窗、选新的待选项
      click(tid('select-a'))
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-SECOND'))
      await settle(2)
      assertEq(aGates.length, 2, '回到 A 之后要能发出新会话自己的写入')
      const getsBefore = getsA()
      const bindsBefore = bindsA()
      // 第一次 A 的写入这时才成功返回：不能关掉新会话的弹窗、不能刷新、不能解锁
      aGates[0].resolve({ ok: true })
      await settle(4)
      assert($('.search-popover-input'), '旧 A 的迟到成功不能关闭新 A 会话的候选弹窗')
      assertEq(refreshes, 0, '旧 A 的迟到成功不能触发新会话的刷新')
      assertEq(getsA(), getsBefore, '旧 A 的迟到成功不能让新会话重新拉绑定')
      click(itemByName('A-SECOND'))
      await settle(2)
      assertEq(bindsA(), bindsBefore, '新 A 会话的写入还没结束，旧请求的 finally 不能解锁')
      // 新会话自己的成功才收尾
      aGates[1].resolve({ ok: true })
      await settle(4)
      assert(!$('.search-popover-input'), '新 A 会话成功才关闭候选弹窗')
      assertEq(refreshes, 1, '新 A 会话成功才刷新一次')
      assertEq(toasts.length, 0, `整个过程不该出现错误提示：${JSON.stringify(toasts)}`)
    } finally {
      await finishControlled([() => aGates.releaseAll()])
    }
  })

  // 「关闭重开候选弹窗」= 新的一次交互：旧会话在飞的写入迟到成功时，
  // 不能关掉用户刚重新打开的弹窗，也不能触发刷新。
  await scenario('status: 关闭重开候选弹窗后，旧写入的迟到成功不能关掉新弹窗', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-reopen-a'
    const symA = lateSymbol(A, 'A', 'main:OLD-A')
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-GOOD', pad_count: 4 }),
    ]
    let refreshes = 0

    try {
      mountElement(<Inspector selected={symA} classify={() => {}} onDeleteSymbol={() => {}}
        onRefresh={() => { refreshes += 1 }} toastMsg={() => {}} />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-GOOD'))
      await settle(2)
      assertEq(aGates.length, 1, '第一次写入要发出并挂起')
      // 用户关闭弹窗，然后又重新打开（新的一次交互）
      click($('.search-popover-close'))
      await settle(4)
      assert(!$('.search-popover-input'), '关闭后弹窗消失')
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert($('.search-popover-input'), '重新打开弹窗')
      // 旧写入这时才成功：只属于已关闭的那次交互
      aGates[0].resolve({ ok: true })
      await settle(4)
      assert($('.search-popover-input'), '旧写入的迟到成功不能关掉重新打开的弹窗')
      assertEq(refreshes, 0, '旧写入的迟到成功不能触发刷新')
    } finally {
      await finishControlled([() => aGates.releaseAll()])
    }
  })

  // 「同 id 重选」（列表刷新后重新点同一个符号 = 新的对象实例）也是新会话：
  // 旧会话的迟到成功不能关新会话的弹窗、不能刷新 —— 只比较 id 是不够的。
  await scenario('status: 同 id 重选（新对象）也是新会话，旧写入的迟到成功不能作用于它', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const A = 'sym-sameid-a'
    const symA1 = lateSymbol(A, 'A', 'main:OLD-A')
    const symA2 = { ...lateSymbol(A, 'A', 'main:OLD-A') }  // 同 id 的另一个对象实例
    net.overrides[`/api/symbols/${A}/binding`] = () => staleBinding('main:OLD-A')
    const aGates = gatedWrites(`/api/symbols/${A}/bind-footprint`)
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.A, library: 'main', name: 'A-GOOD', pad_count: 4 }),
    ]
    let refreshes = 0
    const Host = () => {
      const [sel, setSel] = useState(symA1)
      return (
        <div>
          <button data-testid="reselect-same" onClick={() => setSel(symA2)}>same</button>
          <Inspector selected={sel} classify={() => {}} onDeleteSymbol={() => {}}
            onRefresh={() => { refreshes += 1 }} toastMsg={() => {}} />
        </div>
      )
    }

    try {
      mountElement(<Host />)
      await settle(4)
      click(tid('inspector-replace-footprint'))
      await settle(4)
      click(itemByName('A-GOOD'))
      await settle(2)
      assertEq(aGates.length, 1, '第一次写入要发出并挂起')
      click(tid('reselect-same'))
      await settle(4)
      assert(!$('.search-popover-input'), '重选后旧会话的弹窗要被清掉')
      click(tid('inspector-replace-footprint'))
      await settle(4)
      assert($('.search-popover-input'), '新会话重新打开弹窗')
      aGates[0].resolve({ ok: true })
      await settle(4)
      assert($('.search-popover-input'), '旧会话的迟到成功不能关掉新会话的弹窗')
      assertEq(refreshes, 0, '旧会话的迟到成功不能触发刷新')
    } finally {
      await finishControlled([() => aGates.releaseAll()])
    }
  })

  await scenario('inspector model: StrictMode 重放只保留当前选择，关闭后搜索不能复活', async () => {
    const { default: Inspector } = await import('../../src/components/Inspector.jsx')
    const id = 'sym-model-strict'
    const selected = lateSymbol(id, 'STRICT', 'main:OLD')
    net.overrides[`/api/symbols/${id}/binding`] = () => staleBinding('main:OLD')
    const searches = gatedWrites('/api/footprints/search')
    try {
      mountElement(<StrictMode><Inspector selected={selected} classify={() => {}}
        onDeleteSymbol={() => {}} onRefresh={() => {}} toastMsg={() => {}} /></StrictMode>)
      await settle(4)
      assertEq(countCalls(`/api/symbols/${id}/binding`), 2, 'StrictMode 两次 setup 各发一次读取')
      click(tid('inspector-replace-footprint'))
      await settle(2)
      assertEq(searches.length, 1, '打开候选只读一次')
      click($('.search-popover-close'))
      searches[0].resolve([live({ id: uuids.A, name: 'LATE', library: 'main' })])
      await settle(4)
      assert(!$('.search-popover-input'), '取消后的迟到结果不能重开弹窗')
      unmount()
      assert(!$('.inspector'), '卸载释放展示订阅')
    } finally {
      await finishControlled([() => searches.releaseAll()])
    }
  })

  await scenario('settings: 迟到的旧搜索成功和失败都不能覆盖当前关键词的候选', async () => {
    const { default: SymbolSettings } = await import('../../src/components/SymbolSettings.jsx')
    const footprints = gatedWrites('/api/footprints/search')
    const models = gatedWrites('/api/models/search')
    try {
      mountElement(<SymbolSettings open symbol={{ id: 'sym-search', name: 'U1' }}
        binding={{ footprint_id: uuids.A }} onClose={() => {}} toastMsg={() => {}} />)
      await settle()
      const inputs = $$('.modal-search')
      setInput(inputs[0], 'old-fp')
      setInput(inputs[0], 'new-fp')
      setInput(inputs[1], 'old-model')
      setInput(inputs[1], 'new-model')
      await settle()
      assertEq(footprints.length, 3, '初始和两次封装查询各发一次请求')
      assertEq(models.length, 3, '初始和两次模型查询各发一次请求')
      footprints[2].resolve([live({ id: uuids.B, name: 'NEW-FP' })])
      models[2].resolve([{ id: 'model-new', name: 'NEW-MODEL' }])
      await settle()
      assert(byText('.modal-results .search-popover-name', 'NEW-FP'), '显示最新封装候选')
      assert(byText('.modal-results .search-popover-name', 'NEW-MODEL'), '显示最新模型候选')
      footprints[1].resolve([live({ id: uuids.A, name: 'OLD-FP' })])
      models[1].resolve({ __status: 503, __payload: { error: 'OLD-MODEL-FAILURE' } })
      await settle()
      assert(byText('.modal-results .search-popover-name', 'NEW-FP'), '旧封装成功不得覆盖新搜索')
      assert(byText('.modal-results .search-popover-name', 'NEW-MODEL'), '旧模型失败不得清空新搜索')
      footprints[0].resolve([live({ id: uuids.A, name: 'INITIAL-FP' })])
      models[0].resolve([{ id: 'model-old', name: 'INITIAL-MODEL' }])
      await settle()
      assert(byText('.modal-results .search-popover-name', 'NEW-FP'), '迟到的初始封装搜索也必须丢弃')
      assert(byText('.modal-results .search-popover-name', 'NEW-MODEL'), '迟到的初始模型搜索也必须丢弃')
    } finally {
      await finishControlled([() => footprints.releaseAll([]), () => models.releaseAll([])])
    }
  })

  await scenario('settings: A→B→A和关闭重开之后，旧会话搜索不得混进新会话', async () => {
    const { default: SymbolSettings } = await import('../../src/components/SymbolSettings.jsx')
    const footprints = gatedWrites('/api/footprints/search')
    net.overrides['/api/models/search'] = () => []
    function Host() {
      const [id, setId] = useState('A')
      const [open, setOpen] = useState(true)
      return <>
        <button data-testid="settings-switch-a" onClick={() => setId('A')}>A</button>
        <button data-testid="settings-switch-b" onClick={() => setId('B')}>B</button>
        <button data-testid="settings-reopen" onClick={() => setOpen(true)}>Open</button>
        <SymbolSettings open={open} symbol={{ id, name: id }} binding={null}
          onClose={() => setOpen(false)} toastMsg={() => {}} />
      </>
    }
    try {
      mountElement(<Host />)
      await settle()
      click(tid('settings-switch-b'))
      await settle()
      click(tid('settings-switch-a'))
      await settle()
      assertEq(footprints.length, 3, '三个选择会话各加载默认候选一次')
      footprints[2].resolve([live({ id: uuids.B, name: 'CURRENT-A' })])
      await settle()
      footprints[0].resolve([live({ id: uuids.A, name: 'STALE-A' })])
      footprints[1].resolve([live({ id: uuids.A, name: 'STALE-B' })])
      await settle()
      assert(byText('.search-popover-name', 'CURRENT-A'), '回到相同ID也不能接收最早A会话的结果')
      setInput($('.modal-search'), 'before-close')
      await settle()
      click($('.modal-close'))
      await settle()
      click(tid('settings-reopen'))
      await settle()
      assertEq(footprints.length, 5, '关闭重开创建新默认搜索')
      footprints[4].resolve([live({ id: uuids.B, name: 'REOPENED-A' })])
      await settle()
      footprints[3].resolve([live({ id: uuids.A, name: 'CLOSED-QUERY' })])
      await settle()
      assert(byText('.search-popover-name', 'REOPENED-A'), '已关闭会话的搜索不得覆盖重开后的默认列表')
      assertEq($('.modal-search').value, '', '重开后关键词重置')
    } finally { await finishControlled([() => footprints.releaseAll([])]) }
  })

  await scenario('settings: 绑定详情更新near不清草稿，后续搜索使用新near', async () => {
    const { default: SymbolSettings } = await import('../../src/components/SymbolSettings.jsx')
    net.overrides['/api/footprints/search'] = () => [live({ id: uuids.B, name: 'PENDING-FP' })]
    net.overrides['/api/models/search'] = () => [{ id: 'pending-model', name: 'PENDING-MODEL' }]
    function Host() {
      const [fresh, setFresh] = useState(false)
      return <>
        <button data-testid="settings-update-near" onClick={() => setFresh(true)}>Refresh binding</button>
        <SymbolSettings open symbol={{ id: 'sym-near', name: 'U1' }}
          binding={{ footprint_id: uuids.A, footprint_name: fresh ? 'NEW-NEAR' : 'OLD-NEAR',
            model_name: fresh ? 'NEW-MODEL-NEAR' : 'OLD-MODEL-NEAR' }}
          onClose={() => {}} toastMsg={() => {}} />
      </>
    }
    mountElement(<Host />)
    await settle(4)
    setInput($$('.modal-search')[0], 'keep-fp')
    setInput($$('.modal-search')[1], 'keep-model')
    await settle(4)
    click(tid(`settings-fp-${uuids.B}`))
    click(byText('.search-popover-name', 'PENDING-MODEL'))
    await settle()
    const before = net.calls.length
    click(tid('settings-update-near'))
    await settle(4)
    assertEq($$('.modal-search')[0].value, 'keep-fp', '详情更新不得重置封装关键词')
    assertEq($$('.modal-search')[1].value, 'keep-model', '详情更新不得重置模型关键词')
    assert(byText('.modal-actions button', '应用修改'), '详情更新不得清空待提交选择')
    assertEq(net.calls.length, before, '仅near变化不重复发初始搜索')
    setInput($$('.modal-search')[0], 'next-fp')
    setInput($$('.modal-search')[1], 'next-model')
    await settle(4)
    const lastFp = net.calls.filter(call => call.url.includes('/api/footprints/search')).at(-1)
    const lastModel = net.calls.filter(call => call.url.includes('/api/models/search')).at(-1)
    assertEq(new URL(lastFp.url, location.origin).searchParams.get('near'), 'NEW-NEAR', '后续封装搜索读取最新near')
    assertEq(new URL(lastModel.url, location.origin).searchParams.get('near'), 'NEW-MODEL-NEAR', '后续模型搜索读取最新near')
    net.overrides['/bind-footprint'] = () => ({ ok: true })
    net.overrides['/bind-model'] = () => ({ ok: true })
    net.overrides['/binding'] = () => ({ footprint_id: uuids.B })
    click(byText('.modal-actions button', '应用修改'))
    await settle(8)
    assertEq(net.calls.find(call => call.url.includes('/bind-footprint'))?.body?.footprint_id,
      uuids.B, '更新near后仍提交原先选好的封装')
    assertEq(net.calls.find(call => call.url.includes('/bind-model'))?.body?.model_id,
      'pending-model', '更新near后仍提交原先选好的模型')
    unmount()
  })

  await scenario('status(DOM): 元件设置里失效候选禁用、正常候选点选后提交 UUID（脚本事件，非原生点击）', async () => {
    const { default: SymbolSettings } = await import('../../src/components/SymbolSettings.jsx')
    const deadId = 'cccccccc-3333-4333-8333-333333333333'
    const binding = {
      footprint_id: uuids.A, footprint_name: 'SOT-223',
      footprint_status: 'available', footprint_reason: 'ok',
      model_status: 'unassigned', model_reason: 'no_model_link',
    }
    net.overrides['/api/footprints/search'] = () => [
      live({ id: uuids.B, library: 'main', name: 'SOT-23', pad_count: 4 }),
      { id: deadId, library: 'main', name: 'DEAD', pad_count: 4, footprint_status: 'stale', footprint_reason: 'missing_file' },
    ]
    net.overrides['/api/models/search'] = () => []
    mountElement(<SymbolSettings open symbol={{ id: 'sym-1', name: 'U1' }} binding={binding}
      onClose={() => {}} onChanged={() => {}} toastMsg={() => {}} />)
    await settle(4)
    const dead = tid(`settings-fp-${deadId}`)
    const good = tid(`settings-fp-${uuids.B}`)
    assert(dead && good, '两个候选都显示（失效项也要看得见原因）')
    assertEq(dead.getAttribute('data-selectable'), 'false', '失效候选不可选')
    assert((dead.textContent || '').includes('封装文件已失效'), '给出失效原因')
    click(dead)
    await settle()
    assert(!byText('.modal-actions button', '应用修改'), '点失效候选不会进入待提交')
    assertEq(good.getAttribute('data-selectable'), 'true', '正常候选可选')
    click(good)
    await settle()
    assert(byText('.modal-actions button', '应用修改'), '正常候选原生点选后出现确认按钮')
    click(byText('.modal-actions button', '应用修改'))
    await settle(4)
    const bind = net.calls.find(c => c.url.includes('/bind-footprint'))
    assert(bind, 'confirm 之后才提交绑定')
    assertEq(bind.body.footprint_id, uuids.B, '提交的是候选 UUID')
    unmount()
  })
}

// ---- 入口 ----
const harness = {
  // 原生鼠标会话（runner 通过 CDP Input.dispatchMouseEvent 驱动）
  native: nativeApi,
  async run(mode) {
    results.length = 0
    installFetch()
    harnessApi.compLibraries = WORKSPACES.slice()
    if (i18n.isInitialized === false) await i18n.init()
    await i18n.changeLanguage('zh')
    await newScenarios()
    unmount()
    return { mode, results }
  },
}
window.__harness = harness
window.__ready = true
