import { useState, useEffect, useRef, useCallback } from 'react'
import { api } from '../api'

/// 请求状态：data / error / loading 三者都暴露 ——
/// 只吞掉异常会让「加载失败」和「本来就是空的」看起来一样。
/// load() 仍返回本次结果（失败为 null），方便调用方直接使用新值。
export function useRequest(fn, deps = []) {
  const [data, setData] = useState(null)
  const [error, setError] = useState(null)
  const [loading, setLoading] = useState(false)
  const loadId = useRef(0)

  const load = useCallback(async (...args) => {
    const id = ++loadId.current
    setLoading(true)
    try {
      const result = await fn(...args)
      if (id === loadId.current) { setData(result); setError(null) }
      return result
    } catch (cause) {
      if (id === loadId.current) setError(cause)
      return null
    } finally {
      if (id === loadId.current) setLoading(false)
    }
  }, deps) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { load() }, [load]) // eslint-disable-line react-hooks/exhaustive-deps

  return { data, error, loading, load, setData, setError }
}

export function useStatus() {
  const { data: status, load: loadStatus } = useRequest(api.status)
  return { status: status || { symbols: 0, footprints: 0 }, loadStatus }
}

export function useSymbols() {
  const { data: symbols, error, loading, load: loadSymbols, setData: setSymbols } = useRequest(api.symbols)
  return { symbols: Array.isArray(symbols) ? symbols : [], symbolsError: error,
           symbolsLoading: loading, loadSymbols, setSymbols }
}

export function useLibraries() {
  const { data: libraries, error, load: loadLibraries, setData: setLibraries } = useRequest(api.libraries)
  return { libraries: Array.isArray(libraries) ? libraries : [], librariesError: error,
           loadLibraries, setLibraries }
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
  const { data, load: loadSettings } = useRequest(api.loadConfig)
  const [settings, setSettings] = useState({})
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
