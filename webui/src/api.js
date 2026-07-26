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
  settings:     ()              => request('/settings'),
  saveSettings: (s)             => request('/settings', { method: 'POST', body: JSON.stringify(s) }),
  rules:        ()              => request('/rules'),
  compTypes:    ()              => request('/component-types'),
  pkgTypes:     ()              => request('/package-types'),
  manageType:   (url, action, name, extra) => request(url, { method: 'POST', body: JSON.stringify({ action, name, ...extra }) }),
  resetDb:      ()              => request('/db/reset', { method: 'POST' }),
  importDir:    (dir)           => request(`/import?dir=${encodeURIComponent(dir)}`),
}
