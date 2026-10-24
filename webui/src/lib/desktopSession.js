/// 桌面会话：「本地后台还活着吗」的心跳。
///
/// 之前每次页面卸载（beforeunload）都会给 /api/bye 发一次信标。问题是浏览器
/// 里刷新页面和真正关闭本地窗口无法区分：按 F5 就会让后台服务以为用户要退出，
/// 本地应用直接关掉。所以卸载时默认**什么都不发**。Windows 嵌入窗口下真正的
/// 窗口关闭由 native 生命周期处理（ApiServer 自己会停）；Unix 走外部浏览器时
/// 没有这条信号，仍然只能靠心跳超时兜底，并非原生即时退出。
///
/// 依赖全部注入（fetch / setTimeout / clearTimeout / window / navigator），
/// 测试可以直接驱动真实调度，而不是匹配源码字符串。

/// 全库统计快照的校验：只有完整成功响应才能成为新快照。
/// 忙态（HTTP 200 + busy:true）、缺字段、类型错误一律返回 null，
/// 由调用方保留最后一份有效统计；真实 0 是合法值。
export function readStatusCounts(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return null
  if (payload.ok === false || payload.busy === true) return null
  const { symbols, footprints } = payload
  if (!Number.isInteger(symbols) || symbols < 0) return null
  if (!Number.isInteger(footprints) || footprints < 0) return null
  return { symbols, footprints }
}

/// 展示口径：已知统计才显示数字（真实 0 显示 0）；未知或格式错误显示占位符，
/// 不把「还没读到」伪装成 0 或空白。
export function statusCountText(snapshot, key) {
  const value = snapshot ? snapshot[key] : null
  return snapshot?.known === true && Number.isInteger(value) && value >= 0 ? String(value) : '…'
}

const BUSY_MESSAGE = '后台忙，统计暂不可用'
const INVALID_MESSAGE = '状态响应缺少有效的元件/封装统计'

function asError(cause, fallback) {
  if (cause instanceof Error) return cause
  if (cause && typeof cause === 'object' && typeof cause.message === 'string') return new Error(cause.message)
  if (typeof cause === 'string' && cause) return new Error(cause)
  return new Error(fallback)
}

function isBusyPayload(payload) {
  return Boolean(payload) && typeof payload === 'object' && !Array.isArray(payload) && payload.busy === true
}

/// 全库统计的专用状态模型：
/// - alive 只表示心跳最近一次是否可达；统计是否可用由 known/stale 单独表达；
/// - 忙态、缺字段、解析/网络失败都保留旧值并标记 stale，不发布新快照；
/// - 显式刷新与心跳共用 issued/applied 归属，晚到的旧响应不会覆盖新结果；
/// - 停用（组件卸载）之后晚到的响应既不落地也不通知。
export function createStatusModel() {
  let alive = null
  let known = false
  let counts = null
  let stale = false
  let updating = false
  let lastError = null
  let reason = null
  let revision = 0
  let active = false
  let issued = 0
  let applied = 0
  let latestRead = 0
  const listeners = new Set()

  function createSnapshot() {
    return Object.freeze({
      alive,
      known,
      symbols: counts ? counts.symbols : null,
      footprints: counts ? counts.footprints : null,
      stale,
      updating,
      error: lastError,
      reason,
      revision,
    })
  }
  let snapshot = createSnapshot()

  function emit() {
    revision += 1
    snapshot = createSnapshot()
    if (!active) return
    for (const listener of [...listeners]) listener(snapshot)
  }

  function currentOutcome() {
    return { status: 'published', data: counts ? { ...counts } : null }
  }

  function settle(token, payload, heartbeat) {
    if (!active) return { status: 'aborted' }
    if (token === null || token === undefined) return { status: 'aborted' }
    if (token < applied) {
      // 旧请求迟到：仍要把「刷新结束」落下来，但不改统计值。
      if (!heartbeat && token === latestRead) {
        updating = false
        emit()
      }
      return { status: 'superseded' }
    }
    applied = token
    if (!heartbeat && token === latestRead) updating = false
    alive = true

    const next = readStatusCounts(payload)
    if (next) {
      counts = next
      known = true
      stale = false
      reason = null
      lastError = null
      emit()
      return { status: 'published', data: next }
    }

    // 存活但统计未知：保留最后有效值，只标记过期；不是一次成功刷新。
    const busy = isBusyPayload(payload)
    if (known) stale = true
    reason = busy ? 'busy' : 'invalid'
    lastError = busy ? null : INVALID_MESSAGE
    emit()
    return { status: 'failed', reason, error: new Error(busy ? BUSY_MESSAGE : INVALID_MESSAGE) }
  }

  function observeHeartbeat(event) {
    if (!active) return { status: 'aborted' }
    const token = event && event.token !== null && event.token !== undefined ? event.token : ++issued
    if (token < applied) return { status: 'superseded' }

    if (event && event.error) {
      if (event.error.name === 'AbortError') return { status: 'aborted' }
      applied = token
      alive = false
      emit()
      return { status: 'failed', reason: 'error', error: asError(event.error, '心跳失败') }
    }

    // 没有响应体（测试桩 / 旧调用）只能证明后台可达，不参与统计快照。
    if (!event || event.payload === null || event.payload === undefined) {
      applied = token
      if (alive !== true) {
        alive = true
        emit()
      }
      return currentOutcome()
    }

    return settle(token, event.payload, true)
  }

  return {
    getSnapshot() {
      return snapshot
    },
    subscribe(listener) {
      listeners.add(listener)
      return () => { listeners.delete(listener) }
    },
    activate() { active = true },
    deactivate() {
      active = false
      // 卸载即作废旧归属：即使很快重挂，晚到的旧响应也不会落在新会话上。
      applied = ++issued
      updating = false
    },
    dispose() {
      active = false
      applied = ++issued
      updating = false
      listeners.clear()
    },
    beginRead() {
      const token = ++issued
      latestRead = token
      updating = true
      if (active) emit()
      return token
    },
    beginHeartbeat() {
      if (!active) return null
      // 显式刷新在途时复用它的归属：心跳不能因为「更晚发起」就把一次
      // 正在进行的读取判成过期。两路结果都能落到同一快照上。
      if (updating && latestRead > 0 && latestRead > applied) return latestRead
      return ++issued
    },
    publish(token, payload) {
      return settle(token, payload, false)
    },
    fail(token, cause) {
      if (!active) return { status: 'aborted' }
      if (token === null || token === undefined) return { status: 'aborted' }
      if (token < applied) {
        if (token === latestRead) {
          updating = false
          emit()
        }
        return { status: 'superseded' }
      }
      applied = token
      if (token === latestRead) updating = false
      const failure = asError(cause, '统计读取失败')
      if (failure.kind === 'network' || cause?.kind === 'network') alive = false
      if (known) stale = true
      reason = 'error'
      lastError = failure.message
      emit()
      return { status: 'failed', reason: 'error', error: failure }
    },
    observeHeartbeat,
  }
}

export function startDesktopSession({
  fetchFn = globalThis.fetch,
  setTimer = globalThis.setTimeout,
  clearTimer = globalThis.clearTimeout,
  intervalMs = 1000,
  statusUrl = '/api/status',
  byeUrl = null,
  windowLike = typeof window !== 'undefined' ? window : null,
  navigatorLike = typeof navigator !== 'undefined' ? navigator : null,
  issueStatusToken = null,
  onStatus = null,
} = {}) {
  let active = true
  let timer = null
  let controller = null

  function halt() {
    active = false
    if (timer !== null) {
      clearTimer(timer)
      timer = null
    }
    if (controller) {
      controller.abort() // 在途请求不再悬挂
      controller = null
    }
  }

  function publishStatus(event) {
    if (!active || typeof onStatus !== 'function') return
    try {
      onStatus(event)
    } catch {
      // 统计发布方抛错不能影响下一次心跳；回调方自己负责展示失败。
    }
  }

  function adoptStatus(response, token) {
    if (!active) return
    if (!response || response.ok === false) {
      const suffix = response && response.status ? `（HTTP ${response.status}）` : ''
      publishStatus({ token, error: new Error(`后台状态读取失败${suffix}`) })
      return
    }
    if (typeof response.json !== 'function') {
      // 无响应体的桩（只证明后台可达）；不解析、不错误化。
      publishStatus({ token, payload: null })
      return
    }
    let body
    try {
      body = response.json()
    } catch (error) {
      publishStatus({ token, error })
      return
    }
    Promise.resolve(body).then(
      (payload) => { if (active) publishStatus({ token, payload }) },
      (error) => { if (active) publishStatus({ token, error }) },
    )
  }

  // 递归 setTimeout：全程只维护一个可清理的 timer（halt() 一次 clearTimeout 就能停），
  // 节奏固定按 intervalMs，不等 fetch 返回（后台标签页对 setTimeout 同样节流，并不指望绕开）
  function beat() {
    if (!active) return
    // 上一拍还没回来就先中止它：controller 每拍覆盖，若不先 abort，早先那笔
    // 请求就再也没人取消（stop() 只够得着最后一个）。
    if (controller) {
      controller.abort()
      controller = null
    }
    let token = null
    try {
      if (typeof onStatus === 'function' && typeof issueStatusToken === 'function') {
        token = issueStatusToken()
      }
    } catch {
      token = null
    }
    try {
      controller = typeof AbortController !== 'undefined' ? new AbortController() : null
      const pending = fetchFn(statusUrl, controller ? { signal: controller.signal } : undefined)
      if (pending && typeof pending.then === 'function') {
        pending.then(
          (response) => adoptStatus(response, token),
          (error) => {
            // abort 是主动取消，不算后台失联；stop 之后的晚到错误直接忽略。
            if (!active || error?.name === 'AbortError') return
            publishStatus({ token, error })
          },
        )
      }
    } catch {
      // 心跳失败无所谓：下一拍继续，调度不依赖请求结果
    }
    if (active) timer = setTimer(beat, intervalMs)
  }

  const onUnload = () => {
    if (byeUrl) navigatorLike?.sendBeacon?.(byeUrl, '{}')
  }
  windowLike?.addEventListener?.('beforeunload', onUnload)

  const session = {
    stop() {
      windowLike?.removeEventListener?.('beforeunload', onUnload)
      halt()
    },
    get active() {
      return active
    },
  }

  beat()
  return session
}
