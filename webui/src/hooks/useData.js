import { useState, useEffect, useMemo, useCallback, useSyncExternalStore } from 'react'
import { api } from '../api'
import { createStatusModel } from '../lib/desktopSession'
import { createRequestModel } from '../models/request'

/// 请求状态：data / error / loading 三者都暴露 ——
/// 只吞掉异常会让「加载失败」和「本来就是空的」看起来一样。
/// load() 仍返回本次结果（失败为 null），方便调用方直接使用新值。
export function useRequest(fn, deps = [], validate, eager = true) {
  const [model] = useState(createRequestModel)
  const snapshot = useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)
  // Preserve the existing caller-specified dependency contract. An inline fn
  // with [] keeps its first closure, just as the previous useCallback did.
  const source = useMemo(() => ({ read: fn, validate }), deps) // eslint-disable-line react-hooks/exhaustive-deps
  const loadTracked = useCallback((...args) => model.loadTracked(source.read, args, source.validate), [model, source])
  const load = useCallback((...args) => model.load(source.read, args, source.validate), [model, source])

  useEffect(() => {
    model.activate()
    // eager=false：只登记状态，由调用方在用户动作时 load()。
    // 自动匹配是全量启发式计算，不适合在首屏无条件发起。
    if (eager) load()
    return model.invalidate
  }, [eager, load, model])

  return { data: snapshot.data, error: snapshot.error, loading: snapshot.status === 'loading',
    load, loadTracked, setData: model.setData, setError: model.setError }
}

/// 全库统计：专用状态模型，和普通请求不同。
/// 忙态/缺字段/读取失败都保留最后一份有效统计并标记过期；
/// 首次未知保持 known=false（显示占位符），绝不把未知伪装成 0。
/// 后台心跳的有效响应也经同一模型发布（beginHeartbeatStatus/publishHeartbeatStatus），
/// 因此统计不会在心跳恢复后还停留在忙态时被清空。
export function useStatus() {
  const [model] = useState(createStatusModel)
  const snapshot = useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)

  const loadStatusTracked = useCallback(async () => {
    const token = model.beginRead()
    try {
      const payload = await api.status()
      return model.publish(token, payload)
    } catch (error) {
      return model.fail(token, error)
    }
  }, [model])

  // 旧接口：返回本次发布的新统计；忙态/失败返回 null（调用方保留旧值）。
  const loadStatus = useCallback(async () => {
    const outcome = await loadStatusTracked()
    return outcome.status === 'published' ? outcome.data : null
  }, [loadStatusTracked])

  useEffect(() => {
    model.activate()
    loadStatusTracked()
    return model.deactivate
  }, [loadStatusTracked, model])

  const beginHeartbeatStatus = useCallback(() => model.beginHeartbeat(), [model])
  const publishHeartbeatStatus = useCallback((event) => model.observeHeartbeat(event), [model])

  return { status: snapshot, loadStatus, loadStatusTracked, beginHeartbeatStatus, publishHeartbeatStatus }
}

export function useSymbols() {
  const { data: symbols, error, loading, load: loadSymbols, loadTracked: loadSymbolsTracked, setData: setSymbols } = useRequest(api.symbols, [], Array.isArray)
  return { symbols: Array.isArray(symbols) ? symbols : [], symbolsError: error,
           symbolsLoading: loading, loadSymbols, loadSymbolsTracked, setSymbols }
}

export function useLibraries() {
  const { data: libraries, error, load: loadLibraries, loadTracked: loadLibrariesTracked, setData: setLibraries } = useRequest(api.libraries, [], Array.isArray)
  return { libraries: Array.isArray(libraries) ? libraries : [], librariesError: error,
           loadLibraries, loadLibrariesTracked, setLibraries }
}

export function usePlugins() {
  const { data: plugins, load: loadPlugins, setData: setPlugins } = useRequest(api.plugins)
  return { plugins: Array.isArray(plugins) ? plugins : [], loadPlugins, setPlugins }
}

export function useIssues() {
  const { data: issues, load: loadIssues, loadTracked: loadIssuesTracked } = useRequest(() => api.check(), [], Array.isArray)
  return { issues: Array.isArray(issues) ? issues : [], loadIssues, loadIssuesTracked }
}

export function useMatches() {
  const { data: matches, load: loadMatches } = useRequest(() => api.autoMatch(), [], undefined, false)
  return { matches: Array.isArray(matches) ? matches : [], loadMatches }
}

export function useSettings() {
  const { data, load: loadSettings } = useRequest(api.loadConfig)
  const [settings, setSettings] = useState({})
  useEffect(() => { if (data) setSettings(data) }, [data])
  // data 为 null 才代表配置还没回来；{} 是合法空配置，不能拿它判断就绪。
  const configReady = data !== null && data !== undefined
  return { settings, setSettings, loadSettings, configReady }
}

export function useRules() {
  const { data: rules, load: loadRules } = useRequest(api.rules)
  return { rules: Array.isArray(rules) ? rules : [], loadRules }
}

export function useComponentLibraries() {
  const { data: libs, load: loadCompLibraries, setData: setCompLibraries } = useRequest(api.compLibraries)
  return { compLibraries: Array.isArray(libs) ? libs : [], loadCompLibraries, setCompLibraries }
}
