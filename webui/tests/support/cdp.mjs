// Shared CDP client and same-process Edge shutdown for browser / WebView2 fixtures.
import { spawn } from 'node:child_process'

const CDP_TIMEOUT_MS = 20000
const EDGE_EXIT_TIMEOUT_MS = 15000

function waitForExit(child, timeoutMs) {
  if (child.exitCode !== null || child.signalCode !== null) return Promise.resolve(true)
  return new Promise((resolve) => {
    let done = false
    const finish = (value) => { if (!done) { done = true; clearTimeout(timer); resolve(value) } }
    const timer = setTimeout(() => finish(false), timeoutMs)
    child.once('exit', () => finish(true))
    child.once('error', () => finish(true))
  })
}

/// 给本次进程自己的 PID 发 taskkill /T /F（只杀自己启动的进程树，绝不按镜像名杀别的 Edge）
function killProcessTree(pid) {
  return spawn('taskkill', ['/pid', String(pid), '/T', '/F'], { stdio: 'ignore', windowsHide: true })
}

/// 只停自己启动的那个 Edge：Windows 先 taskkill 自己的 PID（/T /F）并等它退出，
/// 之后才用 child.kill 兜底。绝不按镜像名杀别的 Edge 进程。
/// 可选参数只是测试注入点（默认就是真实 taskkill 与既定的等待上限）。
export async function stopEdge(child, { spawnKill = killProcessTree, exitTimeoutMs = EDGE_EXIT_TIMEOUT_MS } = {}) {
  if (!child || typeof child.pid !== 'number') return
  // 本次子进程已经退出（exitCode / signalCode 任一非 null）：直接返回，绝不再拿旧 PID
  // 发 taskkill —— PID 可能已被系统复用给无关进程，那会杀掉别人的进程树。
  if (child.exitCode !== null || child.signalCode !== null) return
  if (process.platform === 'win32') {
    await new Promise((resolve, reject) => {
      const t = spawnKill(child.pid)
      t.on('exit', (code) => (code === 0 || child.exitCode !== null
        ? resolve() : reject(new Error(`taskkill exited ${code} for pid ${child.pid}`))))
      t.on('error', reject)
    })
  }
  if (await waitForExit(child, exitTimeoutMs)) return
  // 兜底：进程树没退干净时只对自己的子进程补一刀
  try { child.kill() } catch { /* 已经退出 */ }
  if (!await waitForExit(child, exitTimeoutMs))
    throw new Error(`edge pid ${child.pid} did not exit`)
}

// ---- CDP ----
export class Cdp {
  constructor(socket, { timeoutMs = CDP_TIMEOUT_MS, onConsoleError } = {}) {
    this.socket = socket
    this.timeoutMs = timeoutMs
    this.onConsoleError = onConsoleError || (() => {})
    this.id = 0
    this.pending = new Map()
    this.closed = false
    socket.addEventListener('message', (event) => {
      const msg = JSON.parse(event.data)
      if (msg.id && this.pending.has(msg.id)) {
        const entry = this.pending.get(msg.id)
        this.pending.delete(msg.id)
        clearTimeout(entry.timer)
        if (msg.error) entry.reject(new Error(msg.error.message))
        else entry.resolve(msg.result)
        return
      }
      if (msg.method === 'Runtime.exceptionThrown') {
        this.onConsoleError(msg.params?.exceptionDetails?.exception?.description || 'page exception')
      }
      if (msg.method === 'Runtime.consoleAPICalled' && msg.params?.type === 'error') {
        this.onConsoleError((msg.params.args || []).map(a => a.value ?? a.description).join(' '))
      }
    })
    // 断线必须让所有在途命令立刻失败：否则 waitFor 会一直 await 到天荒地老
    socket.addEventListener('close', () => this.failAll(new Error('cdp socket closed')))
    socket.addEventListener('error', () => this.failAll(new Error('cdp socket error')))
  }

  failAll(error) {
    this.closed = true
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer)
      entry.reject(error)
    }
    this.pending.clear()
  }

  /// options.timeoutMs 只放宽/收紧这一次调用：缺省仍是本实例的 timeoutMs
  /// （默认 20s）。给整批场景用的求值放宽，不能顺带把其它命令的上限也放大。
  send(method, params = {}, options = {}) {
    if (this.closed) return Promise.reject(new Error('cdp socket closed'))
    const id = ++this.id
    const limitMs = typeof options.timeoutMs === 'number' ? options.timeoutMs : this.timeoutMs
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`cdp ${method} timed out after ${limitMs}ms`))
      }, limitMs)
      this.pending.set(id, { resolve, reject, timer })
      try {
        this.socket.send(JSON.stringify({ id, method, params }))
      } catch (cause) {
        clearTimeout(timer)
        this.pending.delete(id)
        reject(cause)
      }
    })
  }

  async eval(expression, options = {}) {
    const result = await this.send('Runtime.evaluate', {
      expression, awaitPromise: true, returnByValue: true,
    }, options)
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.exception?.description ||
        result.exceptionDetails.text || 'evaluate failed')
    }
    return result.result?.value
  }
}
