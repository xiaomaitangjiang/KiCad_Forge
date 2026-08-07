import { useState, useEffect, useRef, useCallback } from 'react'
import { api } from '../api'

export function useRequest(fn, deps = []) {
  const [data, setData] = useState(null)
  const loadId = useRef(0)

  const load = useCallback(async (...args) => {
    const id = ++loadId.current
    try {
      const result = await fn(...args)
      if (id === loadId.current) setData(result)
      return result
    } catch { return null }
  }, deps) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { load() }, [load]) // eslint-disable-line react-hooks/exhaustive-deps

  return { data, load, setData }
}

export function useStatus() {
  const { data: status, load: loadStatus } = useRequest(api.status)
  return { status: status || { symbols: 0, footprints: 0 }, loadStatus }
}

export function useSymbols() {
  const { data: symbols, load: loadSymbols, setData: setSymbols } = useRequest(api.symbols)
  return { symbols: Array.isArray(symbols) ? symbols : [], loadSymbols, setSymbols }
}

export function useLibraries() {
  const { data: libraries, load: loadLibraries, setData: setLibraries } = useRequest(api.libraries)
  return { libraries: Array.isArray(libraries) ? libraries : [], loadLibraries, setLibraries }
}

export function usePlugins() {
  const { data: plugins, load: loadPlugins, setData: setPlugins } = useRequest(api.plugins)
  return { plugins: Array.isArray(plugins) ? plugins : [], loadPlugins, setPlugins }
}

export function useIssues() {
  const { data: issues, load: loadIssues } = useRequest(() => api.check(), [])
  return { issues: Array.isArray(issues) ? issues : [], loadIssues }
}

export function useMatches() {
  const { data: matches, load: loadMatches } = useRequest(() => api.autoMatch(), [])
  return { matches: Array.isArray(matches) ? matches : [], loadMatches }
}

export function useSettings() {
  const { data, load: loadSettings } = useRequest(api.settings)
  const [settings, setSettings] = useState({ symbol_lib_path: '', footprint_lib_path: '', model_3d_path: '' })
  useEffect(() => { if (data) setSettings(data) }, [data])
  return { settings, setSettings, loadSettings }
}

export function useRules() {
  const { data: rules, load: loadRules } = useRequest(api.rules)
  return { rules: Array.isArray(rules) ? rules : [], loadRules }
}

export function useComponentLibraries() {
  const { data: libs, load: loadCompLibraries, setData: setCompLibraries } = useRequest(api.compLibraries)
  return { compLibraries: Array.isArray(libs) ? libs : [], loadCompLibraries, setCompLibraries }
}
