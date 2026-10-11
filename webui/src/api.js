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
  loadingCb?.(true)
  let response
  try {
    response = await fetch(`${BASE}${url}`, opts)
  } catch (cause) {
    loadingCb?.(false)
    throw requestError('Network error', 'network', { cause })
  }
  loadingCb?.(false)

  if (!response.ok) {
    // 服务端的失败原因通常放在 JSON 的 error 字段里，别丢
    let detail = ''
    try { detail = (await response.json())?.error || '' } catch { /* 无 JSON 体 */ }
    throw requestError(detail || `HTTP ${response.status}`, 'http',
      { status: response.status, detail })
  }

  try {
    return await response.json()
  } catch (cause) {
    throw requestError('Invalid JSON response', 'parse', { cause })
  }
}

export const api = {
  status:       ()              => request('/status'),
  symbols:      (q, lib)        => request(`/symbols?${q ? 'q=' + encodeURIComponent(q) : ''}${lib ? '&library=' + lib : ''}`),
  libraries:    ()              => request('/libraries'),
  createLibrary:(name)          => request('/libraries', { method: 'POST', body: JSON.stringify({ name }) }),
  // delete_file=false → unregister only (the .kicad_sym file on disk stays)
  deleteLibrary:(id, deleteFile = true) => request('/libraries', { method: 'POST', body: JSON.stringify({ action: 'delete', id, delete_file: deleteFile }) }),
  deleteSymbol: (id)            => request('/libraries', { method: 'POST', body: JSON.stringify({ action: 'delete_symbol', id }) }),
  plugins:      ()              => request('/plugins'),
  executePlugin:(id, body)      => request(`/plugins/execute?id=${id}`, { method: 'POST', body: JSON.stringify(body) }),
  classify:     ()              => request('/classify', { method: 'POST' }),
  check:        ()              => request('/check', { method: 'POST' }),
  autoMatch:    ()              => request('/automatch', { method: 'POST' }),
  loadConfig:   ()              => request('/config'),
  saveConfig:   (s)             => request('/config', { method: 'POST', body: JSON.stringify(s) }),
  rules:        ()              => request('/rules'),
  compTypes:    ()              => request('/component-types'),
  pkgTypes:     ()              => request('/package-types'),
  manageType:   (url, action, name, extra) => request(url, { method: 'POST', body: JSON.stringify({ action, name, ...extra }) }),
  resetDb:      ()              => request('/db/reset', { method: 'POST', body: JSON.stringify({ confirm: true }) }),
  pickFolder:   ()              => request('/pick-folder', { method: 'POST' }),
  importDir:    (dir)           => request(`/import?dir=${encodeURIComponent(dir)}`, { method: 'POST' }),
  compLibraries:()              => request('/component-libraries'),
  saveCompLibrary:(body)        => request('/component-libraries', { method: 'POST', body: JSON.stringify(body) }),
  getBinding:   (symbolId)      => request(`/symbols/${symbolId}/binding`),
  symbolMatches:(symbolId)      => request(`/symbols/${symbolId}/matches`, { method: 'POST' }),
  bindFootprint:(symbolId, fpId)=> request(`/symbols/${symbolId}/bind-footprint`, { method: 'POST', body: JSON.stringify({ footprint_id: fpId }) }),
  bindModel:    (fpId, modelId) => request(`/footprints/${fpId}/bind-model`, { method: 'POST', body: JSON.stringify({ model_id: modelId }) }),
  searchFootprints:(q, near)    => request(`/footprints/search?q=${encodeURIComponent(q)}${near ? '&near=' + encodeURIComponent(near) : ''}`),
  searchModels: (q, near)       => request(`/models/search?q=${encodeURIComponent(q)}${near ? '&near=' + encodeURIComponent(near) : ''}`),
}
