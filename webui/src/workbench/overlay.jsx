import { createContext, useContext, useEffect, useRef, useSyncExternalStore } from 'react'

/**
 * 统一的浮层栈：只保存当前打开层级的 Escape 处理函数，不保存 ReactNode。
 * 栈顶先处理 Escape，避免多个弹窗各自监听导致一次按键关闭全部层级。
 * Provider 使用稳定的 store，注册/注销不重渲染业务树。
 */
const OverlayContext = createContext(null)

function createOverlayStore() {
  const layers = []
  const listeners = new Set()
  const notify = () => { for (const listener of listeners) listener() }
  const top = () => layers[layers.length - 1] || null
  return {
    push(handler) {
      const layer = { handler }
      layers.push(layer)
      notify()
      return () => {
        const index = layers.lastIndexOf(layer)
        if (index >= 0) { layers.splice(index, 1); notify() }
      }
    },
    subscribe(listener) {
      listeners.add(listener)
      return () => listeners.delete(listener)
    },
    top,
    get size() { return layers.length },
  }
}

export function OverlayProvider({ children }) {
  const storeRef = useRef(null)
  if (!storeRef.current) storeRef.current = createOverlayStore()

  useEffect(() => {
    const onKeyDown = (event) => {
      if (event.key !== 'Escape' || event.defaultPrevented) return
      const layer = storeRef.current.top()
      if (!layer) return
      event.preventDefault()
      event.stopPropagation()
      layer.handler()
    }
    document.addEventListener('keydown', onKeyDown)
    return () => document.removeEventListener('keydown', onKeyDown)
  }, [])

  return <OverlayContext.Provider value={storeRef.current}>{children}</OverlayContext.Provider>
}

/** 当前浮层栈深度；用于「有没有更顶层要先处理 Escape」这类判断。 */
export function useOverlayDepth() {
  const store = useContext(OverlayContext)
  return useSyncExternalStore(
    store ? store.subscribe : () => () => {},
    () => (store ? store.size : 0),
    () => 0,
  )
}

/** 注册一层浮层；返回 false 表示当前没有 Provider（测试里单独挂 Modal 时的回退路径）。 */
export function useOverlayLayer(onEscape, active = true) {
  const store = useContext(OverlayContext)
  const handlerRef = useRef(onEscape)
  handlerRef.current = onEscape
  useEffect(() => {
    if (!store || !active) return undefined
    return store.push(() => handlerRef.current?.())
  }, [store, active])
  return !!store
}