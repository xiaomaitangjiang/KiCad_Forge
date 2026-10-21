import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { api } from '../api'
import { buildTypeMap, parseTypeList } from '../lib/typeMetadata'

/// 元件类型表：全应用唯一的一份快照 + 唯一的写入入口。
///
/// 规则（迟到响应、写入保护与失败语义都在这里收敛，界面不各写一套）：
///   * 每次读取开一代（generation）：回包时代次不是最新的就丢弃 ——
///     慢的旧请求永远不能覆盖保存之后的新快照，较老的 finally 也不能把
///     最新一次读取的 loading 关掉；
///   * 读取结果整份校验通过才发布（见 parseTypeList）：坏行不发布，
///     保留最后一次有效快照，也不把读取失败当成「已知为空」；
///   * 写入（add/update/remove）在任何 await 之前同步判定与占位：双击只发
///     一次请求，并且立刻作废在飞的读取（旧回包不能盖掉刚写下的状态）；
///   * 写入一旦确认就进入「已保存、待刷新」：在类型表真的重新读回来之前
///     冻结所有写入（含 UI 之外的直接调用），只允许重新读取，读成功才解冻。
///     这不是写入失败 —— 返回 ok:true + refreshError，界面不得再写一次；
///   * 连第一次有效快照都没有（含首次读取失败）时同样不可写：
///     「不知道」不等于「已知为空」。
export function useComponentTypes() {
  const [types, setTypes] = useState([])
  const [loading, setLoading] = useState(false)
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState(null)   // 最近一次读取失败的原因（快照仍保留）
  const [stale, setStale] = useState(false)  // 已保存但快照还没刷新到位

  const generation = useRef(0)
  const mutating = useRef(false)
  const alive = useRef(true)
  const loadedRef = useRef(false)
  const staleRef = useRef(false)
  const lastError = useRef(null)

  /// 一次读取。返回：
  ///   { status:'published' }  —— 代次仍是最新且整份校验通过，已发布
  ///   { status:'failed' }     —— 代次仍是最新但读取/校验失败，保留旧快照
  ///   { status:'superseded' } —— 已被更新的读取取代：什么都不改，
  ///                              既不标错误也不清最新那次读取的 loading
  const read = useCallback(async (gen) => {
    try {
      const data = await api.compTypes()
      if (!alive.current || gen !== generation.current) return { status: 'superseded' }
      const parsed = parseTypeList(data)
      if (!parsed.ok) throw new Error(parsed.error)
      setTypes(parsed.list)
      setError(null)
      lastError.current = null
      setLoaded(true)
      loadedRef.current = true
      setStale(false)
      staleRef.current = false
      return { status: 'published', types: parsed.list }
    } catch (cause) {
      if (!alive.current || gen !== generation.current) return { status: 'superseded' }
      const message = cause?.message || String(cause)
      setError(message)
      lastError.current = message
      return { status: 'failed', error: message }
    } finally {
      if (alive.current && gen === generation.current) setLoading(false)
    }
  }, [])

  /// 开始一代读取：同步推进代次并进入 loading（await 之前就生效）
  const startRead = useCallback(() => {
    const gen = ++generation.current
    setLoading(true)
    return gen
  }, [])

  const reload = useCallback(() => read(startRead()), [read, startRead])

  /// 写入类型表。返回：
  ///   { ok:false, busy:true }                —— 已有写入在飞
  ///   { ok:false, blocked:true, error }      —— 快照未就绪/待刷新：没有发出请求
  ///   { ok:false, error }                    —— 写失败（界面保留草稿）
  ///   { ok:true, id, refreshError }          —— 已写入；refreshError 非空 = 已保存但没刷新到
  const mutate = useCallback(async (payload) => {
    if (mutating.current) return { ok: false, busy: true }   // await 之前同步占位
    // 写入保护同样在任何 await 之前判定：调用方（含绕过按钮的直接调用）
    // 都不能在「已保存待刷新」或「还没有有效快照」时再写一次
    if (staleRef.current || !loadedRef.current) {
      return { ok: false, blocked: true, error: lastError.current || '' }
    }
    mutating.current = true
    generation.current += 1        // 作废所有在飞的读取：旧回包不能盖住这次写入
    setLoading(false)
    try {
      let response
      try {
        response = await api.saveComponentType(payload)
      } catch (cause) {
        return { ok: false, error: cause?.message || String(cause) }
      }
      if (!alive.current) return { ok: false, error: '' }    // 已卸载：不再起新读取
      if (!response?.ok) {
        return {
          ok: false,
          error: response?.error || '',
          references: response?.references || 0,
        }
      }
      const id = response.id || payload?.id || ''
      // 写入已确认：新表读回来之前冻结写入（这不是写入失败，界面只重读）
      staleRef.current = true
      setStale(true)
      const fresh = await read(startRead())
      if (fresh.status === 'published') return { ok: true, id }
      if (fresh.status === 'superseded') {
        // 被更新的读取取代：那一条读成功发布会自己解冻；它还没落地/失败时
        // 保持冻结，也不把上一轮的旧错误冒充成本次失败
        return { ok: true, id, refreshError: staleRef.current ? (lastError.current || '') : '' }
      }
      return { ok: true, id, refreshError: fresh.error || lastError.current || '' }
    } finally {
      mutating.current = false
    }
  }, [read, startRead])

  useEffect(() => {
    alive.current = true
    reload()
    // 卸载：在飞的读取/写入回包都不再作用，也不会由旧写入再起一笔无归属的读取
    return () => {
      alive.current = false
      generation.current += 1
    }
  }, [reload])

  const typeMap = useMemo(() => buildTypeMap(types), [types])

  return {
    types,
    typeMap,
    loading,
    loaded,
    error,
    stale,
    // 当前快照是否允许写入（有有效快照，且没有「已保存待刷新」）
    canWrite: loaded && !stale,
    reload,
    mutate,
  }
}
