const BASE = '/api'

let loadingCb = null
export function setLoadingCallback(cb) { loadingCb = cb }

/// 结构化错误：区分网络失败、HTTP 失败与响应解析失败，
/// 并保留服务端给出的原因 —— 统一压成 "Network error" 会让
/// 「被拒绝的写入」和「断网」看起来一模一样。
function requestError(message, kind, extra = {}) {
  const error = new Error(message)
  error.kind = kind
  return Object.assign(error, extra)
}

async function request(url, opts = {}) {
  // 布局保存这类后台写入不该点亮全局加载条；silent 只跳过指示器，不改请求语义。
  const { silent = false, ...fetchOptions } = opts
  if (!silent) loadingCb?.(true)
  let response
  try {
    response = await fetch(`${BASE}${url}`, fetchOptions)
  } catch (cause) {
    if (!silent) loadingCb?.(false)
    throw requestError('Network error', 'network', { cause })
  }
  if (!silent) loadingCb?.(false)

  if (!response.ok) {
    // 服务端的失败原因通常放在 JSON 的 error 字段里，别丢
    let detail = ''
    let payload
    try { payload = await response.json(); detail = payload?.error || '' } catch { /* 无 JSON 体 */ }
    throw requestError(detail || `HTTP ${response.status}`, 'http',
      { status: response.status, detail, payload })
  }

  try {
    return await response.json()
  } catch (cause) {
    throw requestError('Invalid JSON response', 'parse', { cause })
  }
}

export const api = {
  status:       ()              => request('/status'),
  symbols:      (q, lib)        => request(`/symbols?${q ? 'q=' + encodeURIComponent(q) : ''}${lib ? '&library=' + encodeURIComponent(lib) : ''}`),
  symbol:       (id)            => request(`/symbols/${encodeURIComponent(id)}`),
  // 宿主只读导出：返回所选符号的完整顶层 S-expression 与来源摘要（ED-030）
  exportSymbols:(ids)            => request('/editing/export', { method: 'POST', body: JSON.stringify({ ids }) }),
  // 对象粘贴：dry_run 只预检（目标摘要/冲突/依赖），false 才走一次 SourceMutation 提交（ED-031/032/035）
  pasteSymbols:(body)           => request('/editing/paste', { method: 'POST', body: JSON.stringify(body) }),
  // 剪切意图：宿主发一次性 token，不删源；粘贴/移动成功才移除源（ED-040/041）
  cutSymbols:   (ids)           => request('/editing/cut', { method: 'POST', body: JSON.stringify({ ids }) }),
  clearCut:     (token)         => request('/editing/cut/clear', { method: 'POST', body: JSON.stringify({ token }) }),
  // 跨库移动：dry_run 预检，false 才在一次 SourceMutation 中提交源移除+目标插入（ED-041..045）
  moveSymbols:  (body)          => request('/editing/move', { method: 'POST', body: JSON.stringify(body) }),
  // 会话历史：只订阅可用状态与动作标签，不做轮询（ED-050..056）
  history:      ()              => request('/editing/history'),
  undoEdit:     ()              => request('/editing/history/undo', { method: 'POST', body: '{}' }),
  redoEdit:     ()              => request('/editing/history/redo', { method: 'POST', body: '{}' }),
  libraries:    ()              => request('/libraries'),
  createLibrary:(name)          => request('/libraries', { method: 'POST', body: JSON.stringify({ name }) }),
  // delete_file=false → unregister only (the .kicad_sym file on disk stays)
  deleteLibrary:(id, deleteFile = true) => request('/libraries', { method: 'POST', body: JSON.stringify({ action: 'delete', id, delete_file: deleteFile }) }),
  deleteSymbol: (id)            => request('/libraries', { method: 'POST', body: JSON.stringify({ action: 'delete_symbol', id }) }),
  plugins:      ()              => request('/plugins'),
  classify:     ()              => request('/classify', { method: 'POST' }),
  check:        ()              => request('/check', { method: 'POST' }),
  autoMatch:    ()              => request('/automatch', { method: 'POST' }),
  loadConfig:   ()              => request('/config'),
  saveConfig:   (s, opts = {})  => request('/config', { ...opts, method: 'POST', body: JSON.stringify(s) }),
  rules:        ()              => request('/rules'),
  compTypes:    ()              => request('/component-types'),
  typeMatching: (signal)        => request('/component-type-matching', { signal }),
  saveTypeMatching: (body)      => request('/component-type-matching', { method: 'POST', body: JSON.stringify(body) }),
  reloadTypeMatching: (signal)  => request('/component-type-matching/reload', { method: 'POST', body: '{}', signal }),
  previewTypeMatching: (body, signal) => request('/component-type-matching/preview', { method: 'POST', body: JSON.stringify(body), signal }),
  applyTypeMatching: (body)     => request('/component-type-matching/apply', { method: 'POST', body: JSON.stringify(body) }),
  pkgTypes:     ()              => request('/package-types'),
  manageType:   (url, action, name, extra) => request(url, { method: 'POST', body: JSON.stringify({ action, name, ...extra }) }),
  // 类型保存：body 就是真实请求体（省略的字段 = 服务端保持原值）——
  // 编辑只提交真正变化的字段，空 name 不会被 manageType 强行塞回请求里
  saveComponentType: (body) => request('/component-types', { method: 'POST', body: JSON.stringify(body) }),
  resetDb:      ()              => request('/db/reset', { method: 'POST', body: JSON.stringify({ confirm: true }) }),
  pickFolder:   ()              => request('/pick-folder', { method: 'POST' }),
  appInfo:      ()              => request('/app-info'),
  openLogDir:   ()              => request('/open-log-dir', { method: 'POST' }),
  openUrl:      (url)           => request('/open-url', { method: 'POST', body: JSON.stringify({ url }) }),
  importDir:    (dir)           => request(`/import?dir=${encodeURIComponent(dir)}`, { method: 'POST' }),
  compLibraries:()              => request('/component-libraries'),
  componentSymbolLibraries:(id) => request(`/component-libraries/${encodeURIComponent(id)}/symbol-libraries`),
  saveCompLibrary:(body)        => request('/component-libraries', { method: 'POST', body: JSON.stringify(body) }),
  getBinding:   (symbolId, options) => request(`/symbols/${encodeURIComponent(symbolId)}/binding`, options),
  symbolMatches:(symbolId, options) => request(`/symbols/${encodeURIComponent(symbolId)}/matches`, { ...options, method: 'POST' }),
  bindFootprint:(symbolId, fpId)=> request(`/symbols/${encodeURIComponent(symbolId)}/bind-footprint`, { method: 'POST', body: JSON.stringify({ footprint_id: fpId }) }),
  bindModel:    (fpId, modelId) => request(`/footprints/${encodeURIComponent(fpId)}/bind-model`, { method: 'POST', body: JSON.stringify({ model_id: modelId }) }),
  searchFootprints:(q, near, options) => request(`/footprints/search?q=${encodeURIComponent(q)}${near ? '&near=' + encodeURIComponent(near) : ''}`, options),
  searchModels: (q, near, options) => request(`/models/search?q=${encodeURIComponent(q)}${near ? '&near=' + encodeURIComponent(near) : ''}`, options),
}