const BASE = '/api'

let loadingCb = null
export function setLoadingCallback(cb) { loadingCb = cb }

async function request(url, opts = {}) {
  loadingCb?.(true)
  try {
    const r = await fetch(`${BASE}${url}`, opts)
    loadingCb?.(false)
    if (!r.ok) throw new Error(`HTTP ${r.status}`)
    return r.json()
  } catch {
    loadingCb?.(false)
    throw new Error('Network error')
  }
}

export const api = {
  status:       ()              => request('/status'),
  symbols:      (q, lib)        => request(`/symbols?${q ? 'q=' + encodeURIComponent(q) : ''}${lib ? '&library=' + lib : ''}`),
  libraries:    ()              => request('/libraries'),
  createLibrary:(name)          => request('/libraries', { method: 'POST', body: JSON.stringify({ name }) }),
  deleteLibrary:(id)            => request('/libraries', { method: 'POST', body: JSON.stringify({ action: 'delete', id }) }),
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
