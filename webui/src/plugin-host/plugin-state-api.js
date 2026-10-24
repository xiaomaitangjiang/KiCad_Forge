// Host plugin enable/disable state. The backend owns admission + revision; the
// frontend loader mirrors it. Calls are best-effort for pages without the API
// (e.g. the prototype mock server).
function encode(value) {
  return encodeURIComponent(String(value))
}

export function createPluginStateApi(fetchImpl = globalThis.fetch) {
  async function request(path, options = {}) {
    const response = await fetchImpl(path, {
      cache: 'no-store',
      ...options,
      headers: { 'content-type': 'application/json', ...(options.headers ?? {}) },
    })
    let body = null
    try {
      body = await response.json()
    } catch {
      body = null
    }
    if (!response.ok) {
      const error = new Error(body?.error || `plugin state request failed: HTTP ${response.status}`)
      error.status = response.status
      error.code = body?.error
      error.body = body
      throw error
    }
    return body
  }

  return {
    get(pluginId) {
      return request(`/api/plugins/${encode(pluginId)}/state`)
    },
    set(pluginId, enabled, ifRevision) {
      return request(`/api/plugins/${encode(pluginId)}/state`, {
        method: 'POST',
        body: JSON.stringify({
          enabled,
          ...(ifRevision != null ? { if_revision: ifRevision } : {}),
        }),
      })
    },
  }
}
