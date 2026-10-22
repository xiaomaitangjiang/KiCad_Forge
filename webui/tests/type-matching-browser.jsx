/* eslint-disable react/only-export-components -- 浏览器测试薄壳，无产品组件导出 */
// 真实 React + act + API 包装；夹具只存合成数据和编排响应，不实现匹配器。
import { act, StrictMode, useState } from 'react'
import { createRoot } from 'react-dom/client'
import i18n from '../src/i18n'
import App from '../src/App'
import TypeMatchingModal from '../src/components/TypeMatchingModal'
import { ClassifyResultModal } from '../src/components/Modals'
import { useRequest } from '../src/hooks/useData'
import { useTypeMatching } from '../src/hooks/useTypeMatching'
import { committedRefreshOutcome } from '../src/lib/typeMatching'
import { buildTypeMap } from '../src/lib/typeMetadata'
import { setLoadingCallback } from '../src/api'
import '../src/App.css'

globalThis.IS_REACT_ACT_ENVIRONMENT = true
const PATH = '/api/component-type-matching'
const TYPES = [
  { id: 'Unknown', name: '未分类', icon: '?', color: '#666666', builtin: true },
  { id: 'custom:relay-fixture', name: '合成继电器', icon: '↯', color: '#006633', builtin: false },
]
const TYPE = TYPES[1].id
const TYPE_MAP = buildTypeMap(TYPES)
const config = (revision = 7) => ({ schema_version: 1, revision,
  rules: [{ id: 'fixture-rule', enabled: true, field: 'name', pattern: '^ SYNTH\\d+ $',
    priority: 10, case_sensitive: true, type_id: TYPE }],
  exceptions: [{ id: 'fixture-exception', enabled: true, field: 'reference', key: '  K?  ', type_id: TYPE }],
})
const clone = value => JSON.parse(JSON.stringify(value))
const readBody = (value = backend.config) => ({ ...clone(value), ok: true, has_snapshot: true,
  load_error: '', scope: 'global', engine: { name: 'PCRE2', dialect: 'PCRE2', code_unit_width: 8,
    semantics: 'byte', utf: false, ucp: false, case_folding: 'ASCII', jit: false,
    search: 'regex_search', newline: 'ANYCRLF', multiline: false, dotall: false,
    dollar_end_only: true, wall_clock_timeout: false },
  limits: { max_rules: 500, max_exceptions: 500, max_pattern_bytes: 512,
    max_exception_key_bytes: 4096, max_source_field_bytes: 4096 },
})
const evidence = [{ source: 'rule', id: 'fixture-rule', field: 'name',
  text: '^ SYNTH\\d+ $', priority: 10, type_id: TYPE }]
function report(overrides = {}) {
  return { ok: true, source: 'saved', summary: { total: 3, revision: backend.config.revision,
    protected_count: 1, matched: 1, unmatched: 1, conflicts: 0, input_too_long: 0,
    evaluation_limited: 0, evaluation_errors: 0, would_update: 1, unchanged: 1,
    type_updated: 1, source_changed: 0, cas_skipped: 0, deleted: 0, rolled_back: 0, not_attempted: 0 },
  details: [{ name: 'SYNTH_SYMBOL', library: 'Synthetic', type_id: TYPE, type: TYPES[1].name,
    confidence: 1, symbol_id: 'fixture-symbol', status: 'matched', reason: '合成后端诊断',
    evidence: '', raw_type_id: 'Unknown', sample: { name: 'SYNTH_SYMBOL', reference: '  ' },
    candidate_type_id: TYPE, structured_evidence: clone(evidence), is_protected: false,
    would_update: true, write_status: 'updated', write_reason: '' }], ...overrides }
}
const sampleReply = (reason = '合成样本响应') => ({ ok: true, mode: 'sample', source: 'draft',
  revision: backend.config.revision, sample: {}, status: 'matched', type_id: TYPE,
  reason, structured_evidence: clone(evidence), load_error: '', saved_healthy: true })
const symbol = (known = false) => ({ id: 'fixture-symbol', library_id: 'fixture-library',
  name: 'SYNTH_SYMBOL', type_id: known ? TYPE : 'Unknown', type: known ? TYPES[1].name : 'Unknown',
  pins: 2, value: 'SYNTH_VALUE', mpn: 'SYNTH_MPN', footprint: '', footprint_reference: '',
  footprint_status: 'unassigned', footprint_reason: '', model_status: 'unassigned', locked: false })
const backend = { config: config(), known: false, extraSymbols: [] }
const net = { calls: [], once: [], pending: [], unknown: [] }
const originalFetch = window.fetch
function response(payload, status = 200) {
  return new Response(JSON.stringify(payload), { status, headers: { 'Content-Type': 'application/json' } })
}
function once(path, handler, method = 'POST') { net.once.push({ path, method, handler }) }
function deferred() {
  let resolve, reject
  const promise = new Promise((yes, no) => { resolve = yes; reject = no })
  return { promise, resolve, reject }
}
function hold(path, method = 'POST') {
  const pending = deferred()
  const item = { done: false, call: null,
    resolve(payload, status = 200) { item.done = true; pending.resolve(response(payload, status)) },
    reject() { item.done = true; pending.reject(new Error('synthetic connection lost')) },
  }
  once(path, call => { item.call = call; return pending.promise }, method)
  net.pending.push(item)
  return item
}
function route(call) {
  const { path, method, body } = call
  if (path === PATH && method === 'GET') return readBody()
  if (path === PATH && method === 'POST') {
    backend.config = { ...clone(body.config), revision: backend.config.revision + 1 }
    return { ok: true, revision: backend.config.revision }
  }
  if (path === `${PATH}/reload`) return readBody()
  if (path === `${PATH}/preview`) return Object.hasOwn(body, 'sample')
    ? { ...sampleReply(), sample: body.sample } : { ...report(), mode: 'database', source: 'draft' }
  if (path === `${PATH}/apply`) { backend.known = true; return report() }
  if (path === '/api/classify') { backend.known = true; return report() }
  if (path.endsWith('/binding')) return { footprint_status: 'unassigned', footprint_reference: '',
    footprint_reason: '', model_status: 'unassigned', footprint_id: '', model_id: '' }
  const reads = {
    '/api/status': { symbols: 1, footprints: 0, models_3d: 0 },
    '/api/symbols': [symbol(backend.known), ...backend.extraSymbols],
    '/api/libraries': [{ id: 'fixture-library', name: 'Synthetic', symbol_count: 1, component_library_id: '' }],
    '/api/component-libraries': [], '/api/config': { active_component_library: '' },
    '/api/component-types': TYPES, '/api/package-types': [], '/api/plugins': [],
    '/api/rules': [], '/api/check': [], '/api/automatch': [],
    '/api/editing/history': { ok: true, can_undo: false, can_redo: false, undo: null, redo: null, entries: [] },
    '/api/import-status': { importing: false, symbols: 1, footprints: 0 },
  }
  if (Object.hasOwn(reads, path)) return reads[path]
  net.unknown.push(`${method} ${path}`)
  throw new Error(`未配置的合成接口：${method} ${path}`)
}
function installFetch() {
  window.fetch = (input, options = {}) => {
    const url = new URL(typeof input === 'string' ? input : input.url, location.origin)
    const call = { path: url.pathname, method: options.method || 'GET',
      body: options.body ? JSON.parse(options.body) : null, search: url.search, signal: options.signal }
    net.calls.push(call)
    const index = net.once.findIndex(item => item.path === call.path && item.method === call.method)
    try {
      const output = index < 0 ? route(call) : net.once.splice(index, 1)[0].handler(call)
      return output && typeof output.then === 'function' ? output : Promise.resolve(response(output))
    } catch (cause) { return Promise.reject(cause) }
  }
}
const $ = selector => document.querySelector(selector)
const $$ = selector => [...document.querySelectorAll(selector)]
const label = name => $(`[aria-label="${name}"]`)
const button = text => $$('button').find(node => node.textContent.trim() === text)
const activityBarItem = text =>
  $$('.activitybar .nav-item').find(node => (node.getAttribute('title') || '').includes(text))
const posts = path => net.calls.filter(call => call.path === path && call.method === 'POST')
const calls = path => net.calls.filter(call => call.path === path)
function assert(value, message) { if (!value) throw new Error(message) }
function equal(actual, expected, message) {
  assert(JSON.stringify(actual) === JSON.stringify(expected), `${message}：期望 ${JSON.stringify(expected)}，实际 ${JSON.stringify(actual)}`)
}
function click(node, times = 1) {
  assert(node, '点击目标不存在')
  act(() => { for (let index = 0; index < times; index++) node.click() })
}
function input(node, value) {
  assert(node, '输入目标不存在')
  const owner = node instanceof HTMLTextAreaElement ? HTMLTextAreaElement : HTMLInputElement
  act(() => {
    Object.getOwnPropertyDescriptor(owner.prototype, 'value').set.call(node, value)
    node.dispatchEvent(new Event('input', { bubbles: true }))
  })
}
async function settle(count = 3) {
  for (let index = 0; index < count; index++) await act(async () => { await new Promise(resolve => setTimeout(resolve, 0)) })
}
function start(fn) { let result; act(() => { result = fn() }); return result }
let root, setOpen, refreshCount, refreshImpl
function unmount() {
  if (root) { act(() => root.unmount()); root = null }
}
function mount(element) { unmount(); root = createRoot($('#root')); act(() => root.render(element)) }
function Shell() {
  const [open, updateOpen] = useState(true)
  setOpen = updateOpen
  return <div className="app"><TypeMatchingModal open={open} onClose={() => updateOpen(false)} types={TYPES} typeMap={TYPE_MAP}
    onRefreshCommitted={() => { refreshCount++; return refreshImpl() }} /></div>
}
async function modal() { mount(<Shell />); await settle(); assert($('.matching-modal'), '真实匹配弹窗已挂载') }
const draftText = () => label('正则规则 1 模式')?.value
async function reopen() { start(() => setOpen(true)); await settle() }
const close = () => click($('.matching-modal .modal-close'))
function dirty(text = '  ^SYNTH_NEW$  ') { input(label('正则规则 1 模式'), text) }
function reset() {
  backend.config = config(); backend.known = false; backend.extraSymbols = []
  net.calls = []; net.once = []; net.pending = []; net.unknown = []
  refreshCount = 0; refreshImpl = async () => ({ status: 'published' })
  setLoadingCallback(null)
  installFetch()
}
const results = []
async function scenario(name, run) {
  await language('zh')
  reset()
  let error = ''
  try { await run() } catch (cause) { error = String(cause?.stack || cause) }
  finally {
    unmount()
    for (const item of net.pending.filter(item => !item.done)) item.resolve({ ok: false, error: 'fixture cleanup' })
    await settle(1)
    setLoadingCallback(null)
    window.fetch = originalFetch
    $('#root').classList.remove('dark')
  }
  if (net.unknown.length) error += ` 未模拟接口：${net.unknown.join(', ')}`
  results.push(error ? { name, ok: false, error } : { name, ok: true })
}

async function loadAndDirty() {
  const pending = hold(PATH, 'GET')
  mount(<Shell />)
  assert(button('应用到未分类元件').disabled, '加载中禁止应用')
  pending.resolve(readBody()); await settle()
  equal(draftText(), config().rules[0].pattern, '正则模式按原文加载')
  equal(label('精确特例 1 原文').value, '  K?  ', '特例保留前后空格和符号')
  assert(!button('应用到未分类元件').disabled, '健康且无草稿时可应用')
  dirty()
  assert(button('应用到未分类元件').disabled, '有草稿禁止应用')
  assert(!button('保存配置').disabled, '有效草稿可保存')
  assert($('.matching-modal').textContent.includes('PCRE2'), '显示宿主引擎语义')
}
async function samplePresence() {
  await modal()
  click(button('样本预览')); await settle()
  equal(posts(`${PATH}/preview`).at(-1).body.sample, {}, '缺失 Reference 不发送字段')
  click(label('提供 Reference'))
  click(button('样本预览')); await settle()
  equal(posts(`${PATH}/preview`).at(-1).body.sample, { reference: '' }, '显式空 Reference 保留空串')
  input(label('样本 Reference'), '  ')
  click(button('样本预览')); await settle()
  equal(posts(`${PATH}/preview`).at(-1).body.sample, { reference: '  ' }, '两个空格原样发送')
  equal(posts(`${PATH}/preview`).at(-1).body.config.exceptions[0].key, '  K?  ', '预览携带特例原文')
}
async function saveSingleWrite() {
  await modal(); dirty()
  input(label('精确特例 1 原文'), '  KEEP\\S  ')
  const pending = hold(PATH)
  click(button('保存配置'), 2)
  equal(posts(PATH).length, 1, '连续点击保存仅一个 POST')
  assert($('.matching-modal fieldset').disabled, '写入期间编辑冻结')
  close(); assert($('.matching-modal'), '写入期间关闭受保护')
  const sent = pending.call.body
  equal(sent.expected_revision, 7, '保存携带读到的版本')
  equal(sent.config.rules[0].pattern, '  ^SYNTH_NEW$  ', '保存正则原文')
  equal(sent.config.exceptions[0].key, '  KEEP\\S  ', '保存特例原文')
  equal(Object.keys(sent.config).sort(), ['exceptions', 'revision', 'rules', 'schema_version'], '保存仅带配置字段')
  backend.config = { ...sent.config, revision: 8 }
  pending.resolve({ ok: true, revision: 8 }); await settle()
  assert(!button('应用到未分类元件').disabled, '保存确认 GET 后开启独立应用')
  equal(posts(`${PATH}/apply`).length, 0, '保存不暗中应用')
  equal(refreshCount, 0, '保存不刷新已提交视图')
}
async function rejectedSave() {
  await modal(); dirty('  ^REJECTED$  ')
  once(PATH, () => ({ ok: false, error: 'revision mismatch synthetic' }))
  click(button('保存配置')); await settle()
  equal(draftText(), '  ^REJECTED$  ', '拒绝保留草稿')
  assert($('.matching-modal').textContent.includes('revision mismatch synthetic'), '拒绝原因可见')
  assert(button('保存配置').disabled && button('应用到未分类元件').disabled, '明确拒绝落屏障')
  close(); await reopen()
  assert(button('保存配置').disabled, '重开后的 GET 不清拒绝屏障')
  backend.config = config(9)
  click(button('重新加载配置')); await settle()
  equal(draftText(), '  ^REJECTED$  ', '显式 reload 保留草稿')
  assert(!button('保存配置').disabled, '显式 reload 恢复草稿保存')
  equal(posts(`${PATH}/reload`).at(-1).body, {}, 'reload 只发送空对象')
  click(button('保存配置')); await settle()
  equal(posts(PATH).at(-1).body.expected_revision, 9, '重试保存使用最新版本')
}
async function savedReadFailure() {
  await modal(); dirty()
  once(PATH, () => ({ ok: false, error: 'synthetic read unavailable' }), 'GET')
  click(button('保存配置')); await settle()
  equal(posts(PATH).length, 1, '仅提交一次保存')
  assert(button('应用到未分类元件').disabled, '保存后确认失败禁止应用')
  equal(draftText(), '  ^SYNTH_NEW$  ', '确认失败保留草稿')
  click(button('重试读取已保存配置')); await settle()
  equal(posts(PATH).length, 1, '重试读取不重写配置')
  assert(!button('应用到未分类元件').disabled, '读取确认成功可应用')
}
async function staleReadDraft() {
  await modal(); dirty('  ^LOCAL_DRAFT$  ')
  close()
  backend.config = config(12)
  backend.config.rules[0].pattern = '^REMOTE_NEW$'
  await reopen()
  equal(draftText(), '  ^LOCAL_DRAFT$  ', '配置变更时保留当前草稿')
  assert(button('保存配置').disabled && button('应用到未分类元件').disabled, 'GET 新版本设置 stale 屏障')
  assert($('.matching-modal').textContent.includes('已保存版本发生变化'), '告知草稿基线发生变化')
  click(button('重新加载配置')); await settle()
  equal(draftText(), '  ^LOCAL_DRAFT$  ', '显式 reload 后仍保留本地原文')
  assert(!button('保存配置').disabled, '显式核对后允许保存草稿')
  click(button('保存配置')); await settle()
  equal(posts(PATH).at(-1).body.expected_revision, 12, '核对后保存采用新基线版本')
}
async function saveReceiptOldRead() {
  await modal(); dirty('^RECEIPT$')
  const stale = readBody(config())
  once(PATH, () => stale, 'GET')
  click(button('保存配置')); await settle()
  assert(button('应用到未分类元件').disabled, '低于保存 receipt 的 GET 不能确认提交')
  assert(button('重试读取已保存配置'), '旧确认响应提供读取重试')
  click(button('重试读取已保存配置')); await settle()
  equal(posts(PATH).length, 1, '旧确认重试不重写配置')
  assert(!button('应用到未分类元件').disabled, '读取到 receipt 对应版本可确认')
}
async function unknownAcrossClose(kind) {
  await modal()
  if (kind === 'save') dirty('^UNKNOWN_WRITE$')
  const path = kind === 'save' ? PATH : `${PATH}/apply`
  once(path, () => { throw new Error('synthetic lost response') })
  click(button(kind === 'save' ? '保存配置' : '应用到未分类元件')); await settle()
  assert($('.matching-modal').textContent.includes('结果未知'), '断网结果明确标记未知')
  close(); await reopen()
  assert(button('保存配置').disabled && button('应用到未分类元件').disabled, '未知 POST 屏障跨关闭保留')
  equal(posts(path).length, 1, '重开不重放 POST')
  click(button('重新加载配置')); await settle()
  equal(refreshCount, 1, '未知写入恢复必须刷新已提交视图')
  assert(button('应用到未分类元件').disabled, '首次核对后仍须显式重新加载开启操作')
}
async function unknownSaveSecondReload() {
  await unknownAcrossClose('save')
  equal(draftText(), '^UNKNOWN_WRITE$', '第一次未知恢复后保留正确草稿')
  assert(button('保存配置').disabled, '首次核对不重放未知保存')
  click(button('重新加载配置')); await settle()
  equal(draftText(), '^UNKNOWN_WRITE$', '第二次显式核对仍保留草稿原文')
  assert(!button('保存配置').disabled, '第二次显式健康 reload 恢复正确草稿保存')
  equal(posts(PATH).length, 1, '两次 reload 都没有自动重发保存')
  equal(refreshCount, 1, '第二次 reload 仅核对配置，不重复提交后刷新')
  click(button('保存配置')); await settle()
  equal(posts(PATH).length, 2, '用户明确点击后才发送新保存')
  equal(posts(PATH).at(-1).body.config.rules[0].pattern, '^UNKNOWN_WRITE$', '新保存使用原有正确草稿')
}
async function appliedSameRevisionReload() {
  await modal()
  click(button('应用到未分类元件')); await settle()
  assert(button('应用到未分类元件').disabled, '已提交禁止立即重放应用')
  click(button('重新加载配置')); await settle()
  equal(backend.config.revision, 7, '显式核对不需要变更配置版本')
  assert(!button('应用到未分类元件').disabled, '健康显式 reload 后同版本可开始新应用')
  equal(posts(`${PATH}/apply`).length, 1, 'reload 不自动重放 apply')
  click(button('应用到未分类元件')); await settle()
  equal(posts(`${PATH}/apply`).length, 2, '只有明确新点击才应用同一已保存版本')
}
let matchingModel
function MatchingProbe() {
  matchingModel = useTypeMatching({ open: true, typeMap: TYPE_MAP,
    onRefreshCommitted: async () => ({ status: 'published' }) })
  return <output>{matchingModel.loading ? 'loading' : 'loaded'}</output>
}
async function reloadBlocksPreview() {
  await modal()
  const pending = hold(`${PATH}/reload`)
  click(button('重新加载配置')); await settle(1)
  assert(button('样本预览').disabled && button('数据库影响预览').disabled,
    '配置 reload 期间两种预览按钮禁用')
  const changed = config(9); changed.rules[0].pattern = '^NEW_READ$'
  pending.resolve(readBody(changed)); await settle()
  equal(draftText(), '^NEW_READ$', 'reload 采纳最新草稿')

  mount(<MatchingProbe />); await settle()
  const hookRead = hold(`${PATH}/reload`)
  start(() => matchingModel.reload()); await settle(1)
  const before = posts(`${PATH}/preview`).length
  start(() => matchingModel.previewSample()); start(() => matchingModel.previewDatabase()); await settle()
  equal(posts(`${PATH}/preview`).length, before, '真实 hook 也阻止读取期间绕过按钮启动预览')
  hookRead.resolve(readBody()); await settle()
}
async function readInvalidatesOldPreview() {
  await modal(); dirty('^READ_CONFIRM$')
  once(PATH, () => ({ ok: false, error: 'synthetic confirmation failed' }), 'GET')
  click(button('保存配置')); await settle()
  const preview = hold(`${PATH}/preview`)
  click(button('样本预览')); await settle(1)
  assert(preview.call, '保存确认失败时允许纯草稿诊断')
  click(button('重试读取已保存配置')); await settle()
  assert(!button('应用到未分类元件').disabled, '确认读取已采纳新版本')
  preview.resolve({ ...sampleReply('STALE_BEFORE_CONFIRMED_READ'), revision: 7 }); await settle()
  assert(!$('.matching-modal').textContent.includes('STALE_BEFORE_CONFIRMED_READ'),
    '读取采纳新配置后旧预览不可显示在新草稿下')
  assert(!$('.matching-report'), '读取完成不保留旧配置预览')
}
async function previewReverse() {
  await modal()
  const old = hold(`${PATH}/preview`)
  click(button('样本预览')); await settle(1)
  const recent = hold(`${PATH}/preview`)
  click(label('提供 Reference')); input(label('样本 Reference'), '  ')
  click(button('样本预览')); await settle(1)
  recent.resolve(sampleReply('NEW_PREVIEW')); await settle()
  old.resolve(sampleReply('OLD_PREVIEW')); await settle()
  assert($('.matching-report').textContent.includes('NEW_PREVIEW'), '较新预览发布')
  assert(!$('.matching-modal').textContent.includes('OLD_PREVIEW'), '逆序旧预览不能覆盖')
  assert(old.call.signal.aborted, '新预览中止旧信号')
}
async function previewClose() {
  await modal()
  const pending = hold(`${PATH}/preview`)
  click(button('样本预览')); await settle(1)
  close(); await reopen()
  pending.resolve(sampleReply('CLOSED_PREVIEW')); await settle()
  assert(!$('.matching-modal').textContent.includes('CLOSED_PREVIEW'), '关闭前预览不串到新会话')
  assert(!$('.matching-report'), '重开不显示旧预览')
  assert(!label('提供 Reference').checked, '样本提供状态重置')
}
async function readReverse() {
  const old = hold(PATH, 'GET')
  mount(<Shell />); await settle(1)
  start(() => setOpen(false)); start(() => setOpen(true)); await settle()
  equal(draftText(), config().rules[0].pattern, '重开较新 GET 已发布')
  const stale = config(1); stale.rules[0].pattern = 'STALE_GET'
  old.resolve(readBody(stale)); await settle()
  equal(draftText(), config().rules[0].pattern, '旧 GET 不能覆盖较新配置')
  assert(old.call.signal.aborted, '关闭中止旧 GET 信号')
}
async function applySingleWrite() {
  await modal()
  const pending = hold(`${PATH}/apply`)
  click(button('应用到未分类元件'), 2)
  equal(posts(`${PATH}/apply`).length, 1, '连续点击应用仅一个 POST')
  equal(pending.call.body, { expected_revision: 7 }, '应用仅发送已保存版本')
  close(); assert($('.matching-modal'), '应用期间不能关闭')
  pending.resolve(report()); await settle()
  equal(refreshCount, 1, '应用完成调用一次刷新')
  assert($('.matching-modal').textContent.includes('界面同步完成'), '刷新发布才显示同步完成')
  assert(button('应用到未分类元件').disabled, '已应用后屏障防止重复应用')
}
async function partialRefresh(status) {
  await modal()
  const pending = [deferred(), deferred(), deferred()]
  refreshImpl = () => Promise.allSettled(pending.map(item => item.promise))
    .then(items => committedRefreshOutcome(items, ['元件', '统计', '元件库']))
  once(`${PATH}/apply`, () => responsePartial(), 'POST')
  click(button('应用到未分类元件')); await settle()
  assert($('.matching-modal').textContent.includes('synthetic partial rollback'), 'HTTP 部分报告保留错误')
  const section = $('[aria-label="应用结果"]')
  assert(section?.textContent.includes('实际写入'), '部分报告计数可见')
  equal([...section.querySelectorAll('.matching-counts div')].find(row => row.textContent.includes('实际写入'))?.querySelector('dd').textContent,
    '1', '部分报告按实际提交计数')
  pending[0].resolve({ status: 'published' }); pending[1].resolve({ status: 'published' }); await settle()
  assert($('.matching-modal').textContent.includes('正在同步界面'), '第三项仍 pending 不提前成功')
  assert(!button('重试刷新视图'), '刷新在飞不出现重试写入口')
  assert(button('应用到未分类元件').disabled, '刷新 pending 保持应用屏障')
  pending[2].resolve({ status, ...(status === 'failed' ? { error: new Error('synthetic refresh failed') } : {}) })
  await settle()
  assert($('.matching-modal').textContent.includes('界面待刷新'), `${status} 不显示同步完成`)
  assert(button('重试刷新视图'), `${status} 提供读取重试`)
  click(button('重新加载配置')); await settle()
  assert(button('应用到未分类元件').disabled, '配置 reload 不能解除刷新失败的应用屏障')
  assert(button('重试刷新视图'), '配置 reload 后仍需读取刷新')
  const before = net.calls.filter(call => call.method === 'POST').length
  refreshImpl = async () => ({ status: 'published' })
  click(button('重试刷新视图'), 2); await settle()
  equal(net.calls.filter(call => call.method === 'POST').length, before, '刷新重试不发送保存或应用 POST')
  equal(refreshCount, 2, '双击刷新重试只调用一次回调')
  assert($('.matching-modal').textContent.includes('界面同步完成'), '读取重试发布成功')
}
function responsePartial() {
  return Promise.resolve(response(report({ ok: false, error: 'synthetic partial rollback' }), 500))
}
async function databasePreview() {
  await modal(); dirty('^DB_PREVIEW$')
  click(button('数据库影响预览')); await settle()
  const body = posts(`${PATH}/preview`).at(-1).body
  assert(!Object.hasOwn(body, 'sample'), '数据库预览不带 sample 字段')
  equal(body.config.rules[0].pattern, '^DB_PREVIEW$', '数据库预览允许使用草稿')
  equal(posts(`${PATH}/apply`).length, 0, '数据库预览不应用')
  equal(posts(PATH).length, 0, '数据库预览不保存')
  click($('[aria-label="预览结果"] details summary'))
  assert($('[aria-label="预览结果"]').textContent.includes('"  "'), '报告显示 Reference 两个空格原文')
}

// useRequest 仅暴露真实状态和调用入口，不复制请求代次或发布规则。
let requestModel, requestQueue
function RequestProbe() {
  requestModel = useRequest(() => {
    const pending = deferred(); requestQueue.push(pending); return pending.promise
  })
  return <output>{JSON.stringify({ data: requestModel.data, loading: requestModel.loading,
    error: requestModel.error?.message })}</output>
}
async function requestProbe(strict = false) {
  requestQueue = []; requestModel = null
  mount(strict ? <StrictMode><RequestProbe /></StrictMode> : <RequestProbe />)
  await settle(1)
}
async function trackedLegacy() {
  await requestProbe()
  requestQueue[0].resolve('initial'); await settle()
  const tracked = start(() => requestModel.loadTracked())
  const legacy = start(() => requestModel.load())
  requestQueue[1].resolve('old tracked'); await settle()
  equal((await tracked).status, 'superseded', 'legacy 请求与 tracked 共享代次')
  assert(requestModel.loading, '旧 finally 不能提前结束新请求 loading')
  equal(requestModel.data, 'initial', '旧结果不发布')
  requestQueue[2].resolve('new legacy'); await settle()
  equal(await legacy, 'new legacy', 'legacy 保留返回新数据契约')
  equal(requestModel.data, 'new legacy', 'legacy 当前结果发布')
  const oldLegacy = start(() => requestModel.load())
  const newTracked = start(() => requestModel.loadTracked())
  requestQueue[4].resolve('new tracked'); await settle()
  equal((await newTracked).status, 'published', 'tracked 当前结果发布')
  requestQueue[3].resolve('old legacy'); await settle()
  equal(await oldLegacy, 'old legacy', 'legacy 旧调用仍返回自己的数据')
  equal(requestModel.data, 'new tracked', 'legacy 旧数据不能反向覆盖 tracked')
}
async function requestFailures() {
  await requestProbe(); requestQueue[0].resolve('kept'); await settle()
  const failed = start(() => requestModel.loadTracked())
  requestQueue[1].reject(new Error('CURRENT_FAIL')); await settle()
  equal((await failed).status, 'failed', '当前失败回报 failed')
  equal(requestModel.error.message, 'CURRENT_FAIL', '当前失败原因发布')
  equal(requestModel.data, 'kept', '失败保留已发布数据')
  const old = start(() => requestModel.loadTracked())
  const recent = start(() => requestModel.loadTracked())
  requestQueue[2].reject(new Error('OLD_FAIL')); await settle()
  equal((await old).status, 'superseded', '旧失败回报 superseded')
  equal(requestModel.error.message, 'CURRENT_FAIL', '旧失败不覆盖当前错误')
  assert(requestModel.loading, '旧失败 finally 不结束新 loading')
  requestQueue[3].resolve('recovered'); await settle()
  equal((await recent).status, 'published', '成功恢复')
  equal(requestModel.error, null, '当前成功清除错误')
}
async function strictUnmount() {
  await requestProbe(true)
  assert(requestQueue.length === 2, `StrictMode 应触发两次 effect，实际 ${requestQueue.length}`)
  requestQueue[1].resolve('strict current'); await settle()
  requestQueue[0].resolve('strict stale'); await settle()
  equal(requestModel.data, 'strict current', 'StrictMode 清理后的旧响应不得发布')
  const held = start(() => requestModel.loadTracked())
  const snapshot = requestModel.data
  unmount(); requestQueue[2].resolve('after unmount'); await settle()
  equal((await held).status, 'superseded', '卸载后 tracked 结果 superseded')
  equal(requestModel.data, snapshot, '卸载后不发布状态')
}

async function openSettingsTools() {
  const settings = activityBarItem('设置')
  assert(settings, '夹具无效：活动栏里没有「设置」入口')
  click(settings)
  for (let index = 0; index < 160 && !$('.settings-page'); index++) {
    await act(async () => { await new Promise(resolve => setTimeout(resolve, 25)) })
  }
  assert($('.settings-page'), '设置页应作为中心页打开')
  const utilsTab = $$('.settings-page div').find((el) =>
    (el.textContent || '').trim().startsWith('🔧') && !el.querySelector('div'))
  assert(utilsTab, '夹具无效：设置页里没有「工具」页签')
  click(utilsTab)
  await settle(1)
}
async function appFlow() {
  mount(<App />)
  for (let index = 0; index < 110 && $('.modal-overlay'); index++) {
    await act(async () => { await new Promise(resolve => setTimeout(resolve, 25)) })
  }
  await settle()
  const card = $$('.symbol-card').find(node => node.querySelector('.symbol-name')?.textContent === 'SYNTH_SYMBOL')
  assert(card, '真实 App 显示合成元件')
  assert(card.textContent.includes(TYPES[0].name), '真实 App 起始卡片为 Unknown 类型')
  click(card); await settle()
  assert($('.inspector'), '真实 Inspector 已打开')
  assert($('.inspector').textContent.includes(TYPES[0].name), 'Inspector 起始同步 Unknown 类型')
  await openSettingsTools()
  click(button('元件类型')); await settle()
  click(button('匹配规则')); await settle()
  assert($('.matching-modal'), '设置→工具→元件类型→匹配规则可达')
  dirty('^APP_SYNTH$'); click(button('保存配置')); await settle()
  click(button('数据库影响预览')); await settle()
  equal(posts(`${PATH}/preview`).at(-1).body.config.revision, 8, '真实 App 使用保存后版本预览')
  const paths = ['/api/symbols', '/api/status', '/api/libraries', '/api/check', '/api/component-types']
  const before = paths.map(path => calls(path).length)
  click(button('应用到未分类元件')); await settle(6)
  paths.forEach((path, index) => assert(calls(path).length > before[index], `应用刷新 ${path}`))
  assert($('.matching-modal').textContent.includes('界面同步完成'), '五视图发布后提示同步完成')
  const refreshed = $$('.symbol-card').find(node => node.querySelector('.symbol-name')?.textContent === 'SYNTH_SYMBOL')
  assert(refreshed.textContent.includes(TYPES[1].name), 'Unknown 元件卡片更新为已知类型')
  assert($('.inspector').textContent.includes(TYPES[1].name), 'selected Inspector 同步相同 UUID 最新类型')
  assert(!$('.inspector').textContent.includes('Unknown'), 'Inspector 不保留旧 Unknown')
}
async function appNewUnknownSameRevision() {
  await appFlow()
  const typesTab = $$('.center-tab-label').find(node => node.textContent.trim() === '元件类型')
  assert(typesTab, '夹具无效：中心页签里没有「元件类型」')
  click(typesTab); await settle()
  backend.extraSymbols = [{ ...symbol(false), id: 'fixture-new-unknown', name: 'NEW_UNKNOWN_SYNTH' }]
  input($('.toolbar input'), 'SYNTH'); await settle()
  const added = $$('.symbol-card').find(node => node.querySelector('.symbol-name')?.textContent === 'NEW_UNKNOWN_SYNTH')
  assert(added?.textContent.includes('未分类'), '真实查询已显示新增的 Unknown 元件')
  click(added); await settle()
  const matchingTab = $$('.center-tab-label').find(node => node.textContent.trim() === '元件类型匹配规则')
  assert(matchingTab, '夹具无效：中心页签里没有「匹配规则」')
  click(matchingTab); await settle()
  assert(button('应用到未分类元件').disabled, '切走再切回保持上一次已应用屏障')
  click(button('重新加载配置')); await settle()
  assert(!button('应用到未分类元件').disabled, '显式核对后同版本可处理新增 Unknown')
  equal(backend.config.revision, 8, '同版本应用不要求编辑配置')
  once(`${PATH}/apply`, () => {
    backend.extraSymbols = [{ ...backend.extraSymbols[0], type_id: TYPE, type: TYPES[1].name }]
    const actual = report()
    return { ...actual, summary: { ...actual.summary, total: 2, protected_count: 1, matched: 1,
      unmatched: 0, would_update: 1, unchanged: 0, type_updated: 1 },
    details: [{ ...actual.details[0], symbol_id: 'fixture-new-unknown', name: 'NEW_UNKNOWN_SYNTH' }] }
  })
  click(button('应用到未分类元件')); await settle(6)
  equal(posts(`${PATH}/apply`).length, 2, '新增元件由用户明确发起第二次应用')
  equal(posts(`${PATH}/apply`).at(-1).body, { expected_revision: 8 }, '第二次应用使用同一保存版本')
  const updated = $$('.symbol-card').find(node => node.querySelector('.symbol-name')?.textContent === 'NEW_UNKNOWN_SYNTH')
  assert(updated?.textContent.includes(TYPES[1].name), '新增 Unknown 卡片分类后更新')
  assert($('.inspector').textContent.includes('NEW_UNKNOWN_SYNTH') && $('.inspector').textContent.includes(TYPES[1].name),
    '第二次应用按当前选中 UUID 同步 Inspector')
}
async function appClassifyReady() {
  mount(<App />)
  for (let index = 0; index < 110 && $('.modal-overlay'); index++) {
    await act(async () => { await new Promise(resolve => setTimeout(resolve, 25)) })
  }
  await settle()
  assert(button('自动分类'), '真实工具栏分类入口存在')
}
const classifyModal = () => $$('[role="dialog"]').find(node => node.getAttribute('aria-label') === '分类结果')
async function legacyPartialRefresh(httpStatus) {
  await appClassifyReady()
  click($('.symbol-card')); await settle()
  const actual = report({ ok: false, error: 'synthetic legacy partial commit' })
  actual.summary = { ...actual.summary, total: 1001, matched: 777, would_update: 700,
    type_updated: 500, not_attempted: 200 }
  actual.details = [actual.details[0], { ...actual.details[0], name: 'SYNTH_ROLLED_BACK',
    symbol_id: 'fixture-rolled-back', write_status: 'rolled_back', write_reason: 'synthetic row rollback' },
  { ...actual.details[0], name: 'SYNTH_NOT_ATTEMPTED', symbol_id: 'fixture-not-attempted',
    write_status: 'not_attempted', write_reason: 'synthetic row not attempted' }]
  once('/api/classify', () => {
    backend.known = true
    return Promise.resolve(response(actual, httpStatus))
  })
  const sources = ['/api/symbols', '/api/status', '/api/libraries', '/api/check', '/api/component-types']
  const gates = sources.map(path => hold(path, path === '/api/check' ? 'POST' : 'GET'))
  click(button('自动分类')); await settle()
  const modal = classifyModal()
  assert(modal, `HTTP ${httpStatus} 部分提交仍显示真实分类报告`)
  assert(modal.textContent.includes('synthetic legacy partial commit'), '报告保留部分失败原因')
  assert(modal.textContent.includes('类型更新：500'), '部分报告显示实际提交500而非matched777')
  assert(!modal.textContent.includes('类型更新：777'), '匹配计数不可冒充实际写入')
  equal(modal.querySelector('[data-write-status="updated"]')?.textContent.trim(), '已写入', '实际更新行明确显示已写入')
  equal(modal.querySelector('[data-write-status="rolled_back"]')?.textContent.trim(), '已回滚', '候选类型的回滚行明确显示未落库')
  equal(modal.querySelector('[data-write-status="not_attempted"]')?.textContent.trim(), '未尝试写入', '未尝试行不冒充已分类')
  assert(gates.every(gate => gate.call), '部分提交仍发起五个受影响视图读取')
  assert(modal.textContent.includes('正在刷新'), '写入已结束后保持刷新pending')
  const payloads = [[symbol(true)], { symbols: 1, footprints: 0 },
    [{ id: 'fixture-library', name: 'Synthetic', symbol_count: 1, component_library_id: '' }], [], TYPES]
  gates.forEach((gate, index) => { if (index !== 3) gate.resolve(payloads[index]) }); await settle()
  assert(classifyModal().textContent.includes('正在刷新'), '四个读取结束不能提前宣布同步完成')
  gates[3].reject(); await settle()
  assert(classifyModal().textContent.includes('写入已结束') && classifyModal().textContent.includes('待刷新'),
    '刷新失败明确写入已结束、界面待刷新')
  assert(button('重试刷新视图'), '提供读取重试入口')
  click(classifyModal().querySelector('.modal-close')); await settle()
  assert(!classifyModal(), '刷新失败的报告可关闭')
  click(button('自动分类'), 2); await settle()
  equal(posts('/api/classify').length, 1, '关闭后再点分类只重开待刷新报告，不再写入')
  assert(classifyModal()?.textContent.includes('synthetic legacy partial commit'), '重开保留原部分报告与原因')
  assert(button('重试刷新视图'), '重开仍提供只读刷新')
  const before = sources.map(path => calls(path).length)
  click(button('重试刷新视图'), 2); await settle(6)
  equal(posts('/api/classify').length, 1, '刷新失败重试不能重发classify')
  sources.forEach((path, index) => equal(calls(path).length - before[index], 1, `双击重试只读一次 ${path}`))
  assert(classifyModal().textContent.includes('界面同步完成'), '全部五读发布后才完成')
  assert($('.inspector').textContent.includes(TYPES[1].name), '旧分类部分提交也同步当前Inspector')
}
async function legacyClassifySingleWrite() {
  await appClassifyReady()
  const pending = hold('/api/classify')
  click(button('自动分类'), 2)
  await settle(1)
  equal(posts('/api/classify').length, 1, '正常分类并发点击仅一次POST')
  backend.known = true
  pending.resolve(report()); await settle(6)
  assert(classifyModal(), '正常分类结果仍可见')
  assert(classifyModal().textContent.includes('分类完成: 1/3 已匹配'), '保留正常匹配计数显示')
  assert(classifyModal().textContent.includes('类型更新：1'), '正常分类使用实际写入计数')
}
async function legacyClassifyPureError() {
  await appClassifyReady()
  const paths = ['/api/symbols', '/api/libraries', '/api/check', '/api/component-types']
  const before = paths.map(path => calls(path).length)
  once('/api/classify', () => ({ ok: false, error: 'synthetic no commit error' }))
  click(button('自动分类')); await settle()
  assert(!classifyModal(), '纯未提交错误不显示虚假已提交报告')
  assert($('#toast').textContent.includes('synthetic no commit error'), '纯错误保留实际原因')
  paths.forEach((path, index) => equal(calls(path).length, before[index], `未提交错误不刷新 ${path}`))
}
async function legacyClassifyUnknown() {
  await appClassifyReady()
  once('/api/classify', () => { throw new Error('synthetic lost legacy response') })
  click(button('自动分类')); await settle(6)
  equal(posts('/api/classify').length, 1, '网络未知不自动重放分类写入')
  assert($('#toast').textContent.includes('结果未知'), '网络失败不能断言尚未写入')
  assert(!classifyModal(), '未知响应不伪造实际提交计数')
}

async function language(value) {
  await act(async () => { await i18n.changeLanguage(value) })
  await settle(1)
}
async function englishMatchingSurface() {
  await modal()
  const initialCalls = net.calls.length
  await language('en')
  equal(net.calls.length, initialCalls, '切英语不重新请求匹配配置')
  assert(label('Component Type Matching Rules'), '英文弹窗标题和可访问名称')
  assert(label('Regex rule 1 pattern') && label('Exact exception 1 text'), '两类条目控件使用英文标签')
  assert(label('Provide Model / MPN') && label('Sample Value'), '四个样本字段使用英文')
  assert(button('Add Regex rule') && button('Add Exact exception'), '新增按钮使用英文')
  equal(label('Regex rule 1 pattern').value, config().rules[0].pattern, '模式原文保留')
  equal(label('Exact exception 1 text').value, '  K?  ', '特例原文保留')
  click(button('Preview sample')); await settle()
  const sample = label('Preview results')
  assert(sample.textContent.includes('Matched') && sample.textContent.includes('Draft revision 7'), '英文样本状态、来源、版本')
  assert(sample.textContent.includes('合成样本响应') && sample.textContent.includes(TYPES[1].name), '后端reason和用户类型名原样显示')
  click(button('Preview database changes')); await settle()
  const preview = label('Preview results')
  assert(preview.textContent.includes('Actually written') && preview.textContent.includes('Classified and protected'), '英文计数与保护状态')
  click(preview.querySelector('summary'))
  assert(preview.textContent.includes('Written') && preview.textContent.includes('合成后端诊断'), '英文写入状态与原始reason')
  assert(preview.textContent.includes('Entry ID: fixture-rule') && preview.textContent.includes('Priority 10'), '英文证据字段')
  assert(!$('.matching-modal').textContent.includes('matching.'), '没有暴露缺失翻译键')
}
async function languageKeepsDraftAndSample() {
  await modal(); dirty('  ^KEEP_DRAFT\\d+$  ')
  input(label('精确特例 1 原文'), '  原文 KEEP  ')
  click(label('提供 Reference')); input(label('样本 Reference'), '  R?  ')
  const before = net.calls.length
  await language('en')
  equal(label('Regex rule 1 pattern').value, '  ^KEEP_DRAFT\\d+$  ', '切语言保留模式与反斜杠')
  equal(label('Exact exception 1 text').value, '  原文 KEEP  ', '切语言保留精确文本')
  equal(label('Sample Reference').value, '  R?  ', '切语言保留样本字段和值')
  assert(label('Provide Reference').checked, '切语言保留provided状态')
  assert(button('Apply to unclassified components').disabled && !button('Save configuration').disabled, '切语言保留dirty及写屏障条件')
  equal(net.calls.length, before, '语言变化不触发GET/POST')
  await language('zh')
  equal(draftText(), '  ^KEEP_DRAFT\\d+$  ', '切回中文仍保留草稿')
  equal(net.calls.length, before, '切回同样不重新请求')
}
async function savingLanguageDoesNotReplay() {
  await modal(); dirty('^LANGUAGE_IN_FLIGHT$')
  const pending = hold(PATH)
  click(button('保存配置'))
  const before = net.calls.length
  await language('en')
  assert(button('Saving…')?.disabled, '在途保存按钮换语言且保持冻结')
  equal(net.calls.length, before, '在途切语言不重放GET/POST')
  const sent = clone(pending.call.body.config)
  backend.config = { ...sent, revision: 8 }
  pending.resolve({ ok: true, revision: 8 }); await settle()
  equal(posts(PATH).length, 1, '切语言前后只一次保存')
  equal(label('Regex rule 1 pattern').value, '^LANGUAGE_IN_FLIGHT$', '保存确认不丢当前原文')
  assert($('.matching-notice').textContent.includes('Configuration saved.'), '保存完成notice按当前语言显示')
  const confirmedCalls = net.calls.length
  await language('zh')
  assert($('.matching-notice').textContent.includes('配置已保存'), '已发布notice切语言即时更新')
  equal(net.calls.length, confirmedCalls, '重译notice不重复读取或保存')
}
async function publishedMessagesChangeLanguage() {
  await modal(); dirty(); click(button('放弃草稿'))
  assert($('.matching-notice').textContent.includes('草稿已放弃'), '中文notice已发布')
  await language('en')
  assert($('.matching-notice').textContent.includes('Draft discarded.'), '已发布notice变为英文')
  once(`${PATH}/preview`, () => ({ ok: true }))
  click(button('Preview sample')); await settle()
  assert($('.matching-alert').textContent.includes('Invalid preview response'), '本地坏响应错误英文')
  const before = net.calls.length
  await language('zh')
  assert($('.matching-alert').textContent.includes('预览结果格式无效'), '已发布错误变为中文')
  equal(net.calls.length, before, '重译错误不重复预览')
  click(button('重新加载配置')); await settle()
  input(label('正则规则 1 优先级'), '')
  assert($('.matching-alert').textContent.includes('int32'), '无效优先级显示校验')
  await language('en')
  assert($('.matching-alert').textContent.includes('Invalid regex pattern or priority'), '本地校验错误随语言切换')
  assert(button('Save configuration').disabled && button('Preview sample').disabled, '语言变化不绕过无效草稿保护')
}
async function unknownLanguagePreservesBarrier() {
  await modal(); dirty('^LANGUAGE_UNKNOWN$')
  once(PATH, () => { throw new Error('synthetic lost response') })
  click(button('保存配置')); await settle()
  const before = net.calls.length
  await language('en')
  assert($('.matching-modal').textContent.includes('The save result is unknown.'), '未知写入notice翻译')
  assert($('.matching-alert').textContent.includes('Network error'), '本地网络错误翻译')
  assert(button('Save configuration').disabled && button('Apply to unclassified components').disabled, '未知屏障保留')
  equal(label('Regex rule 1 pattern').value, '^LANGUAGE_UNKNOWN$', '未知写入草稿保留')
  await language('zh')
  assert($('.matching-alert').textContent.includes('网络错误'), '网络错误切中文')
  equal(posts(PATH).length, 1, '未知POST切语言不重放')
  equal(net.calls.length, before, '未知状态切语言不暗中GET/reload')
}
async function legacyReportLanguage() {
  const result = report({ ok: false, error: '后端原文/BACKEND-ONLY' })
  result.summary = { ...result.summary, matched: 2, total: 3, type_updated: 1 }
  result.details = [{ name: 'R1', type: 'Resistor', write_status: 'updated' },
    { name: 'R2', write_status: 'rolled_back', write_reason: '后端原因/RAW-REASON' }]
  const refresh = { status: 'failed', details: [
    { source: 'classifying.sources.symbols', status: 'superseded' },
    { source: 'classifying.sources.status', status: 'failed', error: 'DB-DOWN' },
    { source: 'classifying.sources.types', status: 'failed', error: '' }] }
  mount(<ClassifyResultModal result={result} refresh={refresh} onRetryRefresh={() => {}} onClose={() => {}} />)
  assert(label('分类结果').textContent.includes('匹配：2 / 3'), '中文旧分类报告')
  const before = net.calls.length
  await language('en')
  const modal = label('Classification Results')
  assert(modal.textContent.includes('Matched: 2 / 3') && modal.textContent.includes('Types updated: 1'), '旧报告计数原地翻译')
  equal(modal.querySelector('[data-write-status="updated"]').textContent, 'Written', '旧报告写入状态翻译')
  equal(modal.querySelector('[data-write-status="rolled_back"]').textContent, 'Rolled back', '回滚状态翻译')
  assert(modal.textContent.includes('Components: This read was superseded') && modal.textContent.includes('Statistics: DB-DOWN')
    && modal.textContent.includes('Types: Not published'), '刷新来源与兜底按当前语言显示')
  assert(modal.textContent.includes('后端原文/BACKEND-ONLY') && modal.textContent.includes('后端原因/RAW-REASON'), '后端原因原样保留')
  equal(net.calls.length, before, '报告切语言不请求任何接口')
}

const cases = [
  ['真实弹窗：加载、两类原文、dirty 禁止 apply', loadAndDirty],
  ['样本 Reference：缺失 / 空串 / 两个空格', samplePresence],
  ['保存：双击只写一次、原文与版本、确认 GET', saveSingleWrite],
  ['保存拒绝：保草稿、屏障跨重开、显式 reload 使用新版本', rejectedSave],
  ['保存成功确认失败：重试只读取、保留草稿', savedReadFailure],
  ['GET 新版本：保留草稿、stale 屏障、显式 reload 核对', staleReadDraft],
  ['保存 receipt：低版本 GET 不确认、重试只读取', saveReceiptOldRead],
  ['保存网络未知：屏障跨关闭且不重放 POST', () => unknownAcrossClose('save')],
  ['应用网络未知：屏障跨关闭且恢复刷新', () => unknownAcrossClose('apply')],
  ['未知保存恢复：第二次显式 reload 保草稿并允许新保存', unknownSaveSecondReload],
  ['已应用恢复：显式 reload 允许同版本新应用', appliedSameRevisionReload],
  ['配置读取：界面和 hook 都禁止启动预览', reloadBlocksPreview],
  ['确认读取：采纳新配置时作废旧样本预览', readInvalidatesOldPreview],
  ['预览逆序：旧响应不覆盖新诊断', previewReverse],
  ['预览关闭：旧响应不串会话、样本状态重置', previewClose],
  ['GET 逆序：关闭前旧读取不覆盖新配置', readReverse],
  ['应用：双击只写一次、阻止关闭、已提交屏障', applySingleWrite],
  ['部分 HTTP 报告：三项刷新等待全部、failed 读取重试', () => partialRefresh('failed')],
  ['部分 HTTP 报告：superseded 不当成功、读取重试', () => partialRefresh('superseded')],
  ['数据库草稿预览：报告原文、不保存不应用', databasePreview],
  ['真实 useRequest：tracked / legacy 共用代次、旧 finally', trackedLegacy],
  ['真实 useRequest：当前与旧失败、保留数据、恢复', requestFailures],
  ['真实 useRequest：StrictMode 及卸载隔离', strictUnmount],
  ['真实 App：设置入口→保存→DB 预览→应用→五视图与 Inspector', appFlow],
  ['真实 App：新增 Unknown 后显式 reload 以同版本再次应用', appNewUnknownSameRevision],
  ['旧分类真实 App：HTTP 500部分提交报告、五读等待、失败只读重试', () => legacyPartialRefresh(500)],
  ['旧分类真实 App：HTTP 200部分提交报告、五读等待、失败只读重试', () => legacyPartialRefresh(200)],
  ['旧分类真实 App：正常结果与并发点击单次写入', legacyClassifySingleWrite],
  ['旧分类真实 App：纯未提交错误保原因且不刷新', legacyClassifyPureError],
  ['旧分类真实 App：网络结果未知不自动重放', legacyClassifyUnknown],
  ['英文匹配界面：字段、预览、报告与证据，保留用户/后端原文', englishMatchingSurface],
  ['语言切换：草稿、样本与dirty保留，不发GET/POST', languageKeepsDraftAndSample],
  ['语言切换：在途保存保持单次写入、notice即时重译', savingLanguageDoesNotReplay],
  ['语言切换：已发布notice、本地错误、校验错误即时重译', publishedMessagesChangeLanguage],
  ['语言切换：未知写入屏障与草稿保留，不重放POST', unknownLanguagePreservesBarrier],
  ['旧分类报告：同一挂载原地切英语、刷新来源翻译与后端原文', legacyReportLanguage],
]
window.__harness = {
  async run(mode) {
    if (mode !== 'type-matching') throw new Error(`不支持模式：${mode}`)
    results.length = 0
    await i18n.changeLanguage('zh')
    for (const [name, run] of cases) await scenario(name, run)
    return { mode, results: [...results] }
  },
  async layout({ width, height, dark }) {
    results.length = 0
    for (const lang of ['zh', 'en']) {
      await scenario(`layout ${width}x${height} ${dark ? '暗' : '亮'} ${lang}`, async () => {
        await language(lang)
        $('#root').classList.toggle('dark', dark)
        backend.config.rules[0].pattern = 'LONG_LITERAL_'.repeat(35)
        backend.config.exceptions[0].key = '原文与空格  '.repeat(50)
        await modal()
        const longReport = report({ mode: 'database', source: 'draft' })
        longReport.details[0] = { ...longReport.details[0], name: 'LONG_NAME_'.repeat(80),
          reason: '长诊断原文_'.repeat(100), sample: { reference: '  LONG_REFERENCE_'.repeat(100) },
          structured_evidence: [{ ...evidence[0], text: 'LONG_EVIDENCE_'.repeat(100) }] }
        once(`${PATH}/preview`, () => longReport)
        click(button(i18n.t('matching.previewDatabase'))); await settle()
        click(label(i18n.t('matching.previewResult')).querySelector('details summary'))
        const node = $('.matching-modal'), bounds = node.getBoundingClientRect()
        equal(node.getAttribute('aria-label'), i18n.t('matching.title'), '当前语言真实标题生效')
        assert(button(i18n.t('matching.apply')), '当前语言应用按钮显示')
        assert(bounds.width > 0 && bounds.left >= -1 && bounds.right <= innerWidth + 1,
          `弹窗超出 viewport：${JSON.stringify({ left: bounds.left, right: bounds.right, innerWidth })}`)
        assert(node.scrollWidth <= node.clientWidth + 1, `弹窗横向溢出 ${node.scrollWidth - node.clientWidth}px`)
        for (const input of $$('.matching-modal input[type="checkbox"]')) {
          const rect = input.getBoundingClientRect()
          assert(rect.width > 0 && rect.width <= 24, `checkbox 宽度 ${rect.width}px`)
        }
        const style = getComputedStyle(node)
        equal(style.getPropertyValue('--card').trim(), dark ? '#242a2e' : '#ffffff', 'FW 亮暗主题变量生效')
        equal(style.getPropertyValue('--text').trim(), dark ? '#e5e9e9' : '#252a2d', 'FW 文字主题变量生效')
        equal(style.color, dark ? 'rgb(229, 233, 233)' : 'rgb(37, 42, 45)', '正文实际颜色使用主题')
        assert(style.color !== style.backgroundColor, `文字与背景不可相同：${style.color}`)
        const grids = $$('.matching-entry-grid, .matching-sample-grid')
        assert(grids.every(grid => grid.scrollWidth <= grid.clientWidth + 1), '来源/类型/样本网格不得横向溢出')
        if (width < 500) {
          const controls = [...$('.matching-entry-grid').children].map(item => item.getBoundingClientRect())
          assert(controls.some(item => item.top > controls[0].top + 10), '窄屏控件应换行')
        }
        $('#root').classList.remove('dark')
      })
    }
    return { mode: 'type-matching', results: [...results] }
  },
}
window.__ready = true
