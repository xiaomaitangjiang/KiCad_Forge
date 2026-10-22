import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { api } from '../api'
import {
  WORKBENCH_LAYOUT_KEY,
  WORKBENCH_WORKSPACE_VIEW,
  createDefaultWorkbenchLayout,
  parseWorkbenchLayout,
  serializeWorkbenchLayout,
} from './layout'
import { serializeCenterState } from './useCenterNavigation'

const SAVE_DELAY_MS = 300

/**
 * 布局状态只在这里维护：内存状态用于渲染，防抖后的完整快照写入宿主配置。
 * 保存失败保留待写快照，retrySave 可重试；拖动中的 pointermove 不触发保存。
 */
export function useWorkbenchLayout(config, configReady) {
  const [layout, setLayout] = useState(createDefaultWorkbenchLayout)
  const [recovered, setRecovered] = useState(false)
  const [saveState, setSaveState] = useState('idle')
  const [saveError, setSaveError] = useState('')
  const [hydrated, setHydrated] = useState(false)
  const layoutRef = useRef(layout)
  const configRef = useRef(config)
  configRef.current = config
  const pendingRef = useRef(null)
  const timerRef = useRef(null)
  const chainRef = useRef(Promise.resolve())
  const mountedRef = useRef(true)
  const hydratedRef = useRef(false)

  layoutRef.current = layout

  useEffect(() => {
    mountedRef.current = true
    return () => {
      mountedRef.current = false
      if (timerRef.current) clearTimeout(timerRef.current)
      timerRef.current = null
    }
  }, [])

  useEffect(() => {
    if (hydratedRef.current || !configReady) return undefined
    // 等一帧让同一次 render 的 config 引用落到 ref 后再水合；useSettings 已把
    // 服务端快照与 configReady 对齐，避免读到上一帧的空配置。
    const frame = requestAnimationFrame(() => {
      if (hydratedRef.current) return
      hydratedRef.current = true
      const parsed = parseWorkbenchLayout(configRef.current?.[WORKBENCH_LAYOUT_KEY])
      setLayout(parsed.layout)
      setRecovered(parsed.recovered)
      setHydrated(true)
    })
    return () => cancelAnimationFrame(frame)
  }, [config, configReady])

  const write = useCallback((payload) => {
    pendingRef.current = payload
    chainRef.current = chainRef.current.catch(() => {}).then(async () => {
      // 队列前面的快照已被覆盖时跳过，避免旧布局晚于新布局落盘。
      if (!mountedRef.current || pendingRef.current !== payload) return
      setSaveState('saving')
      try {
        await api.saveConfig({ [WORKBENCH_LAYOUT_KEY]: payload }, { silent: true })
        if (!mountedRef.current) return
        if (pendingRef.current === payload) {
          pendingRef.current = null
          setSaveState('saved')
          setSaveError('')
        }
      } catch (error) {
        if (!mountedRef.current) return
        setSaveState('error')
        setSaveError(error?.message || String(error))
      }
    })
  }, [])

  const schedulePersist = useCallback((nextLayout) => {
    const payload = serializeWorkbenchLayout(nextLayout)
    pendingRef.current = payload
    if (timerRef.current) clearTimeout(timerRef.current)
    timerRef.current = setTimeout(() => {
      timerRef.current = null
      write(payload)
    }, SAVE_DELAY_MS)
  }, [write])

  const commit = useCallback((updater) => {
    const previous = layoutRef.current
    const next = updater(previous)
    if (next === previous) return
    layoutRef.current = next
    setLayout(next)
    schedulePersist(next)
  }, [schedulePersist])

  const setPrimaryWidth = useCallback((width) => {
    commit(previous => {
      const next = serializeWorkbenchLayout({ ...previous, primarySidebar: { ...previous.primarySidebar, width } })
      const parsed = JSON.parse(next)
      if (parsed.primarySidebar.width === previous.primarySidebar.width) return previous
      return parsed
    })
  }, [commit])

  const setAuxiliaryWidth = useCallback((width) => {
    commit(previous => {
      const next = serializeWorkbenchLayout({ ...previous, auxiliarySidebar: { ...previous.auxiliarySidebar, width } })
      const parsed = JSON.parse(next)
      if (parsed.auxiliarySidebar.width === previous.auxiliarySidebar.width) return previous
      return parsed
    })
  }, [commit])

  const setBottomHeight = useCallback((height) => {
    commit(previous => {
      const next = serializeWorkbenchLayout({ ...previous, bottomPanel: { ...previous.bottomPanel, height } })
      const parsed = JSON.parse(next)
      if (parsed.bottomPanel.height === previous.bottomPanel.height) return previous
      return parsed
    })
  }, [commit])

  const togglePrimarySidebar = useCallback(() => {
    commit(previous => serializeAndParse({ ...previous, primarySidebar: { ...previous.primarySidebar, visible: !previous.primarySidebar.visible } }))
  }, [commit])

  const toggleAuxiliarySidebar = useCallback(() => {
    commit(previous => serializeAndParse({ ...previous, auxiliarySidebar: { ...previous.auxiliarySidebar, visible: !previous.auxiliarySidebar.visible } }))
  }, [commit])

  const toggleBottomPanel = useCallback(() => {
    commit(previous => serializeAndParse({ ...previous, bottomPanel: { ...previous.bottomPanel, visible: !previous.bottomPanel.visible } }))
  }, [commit])

  const setPrimaryActiveView = useCallback((viewId) => {
    const nextView = typeof viewId === 'string' && viewId.trim() ? viewId : WORKBENCH_WORKSPACE_VIEW
    commit(previous => {
      if (previous.primarySidebar.activeViewId === nextView) return previous
      return serializeAndParse({ ...previous, primarySidebar: { ...previous.primarySidebar, activeViewId: nextView } })
    })
  }, [commit])

  const setBottomActiveView = useCallback((viewId) => {
    commit(previous => {
      if (previous.bottomPanel.activeViewId === viewId) return previous
      return serializeAndParse({ ...previous, bottomPanel: { ...previous.bottomPanel, activeViewId: viewId } })
    })
  }, [commit])

  const openBottomPanel = useCallback((viewId) => {
    commit(previous => {
      const bottomPanel = {
        ...previous.bottomPanel,
        visible: true,
        ...(typeof viewId === 'string' && viewId ? { activeViewId: viewId } : {}),
      }
      if (bottomPanel.visible === previous.bottomPanel.visible
        && bottomPanel.activeViewId === previous.bottomPanel.activeViewId) return previous
      return serializeAndParse({ ...previous, bottomPanel })
    })
  }, [commit])

  const setCenterState = useCallback((centerState) => {
    commit(previous => {
      const center = serializeCenterState(centerState)
      if (JSON.stringify(previous.center) === JSON.stringify(center)) return previous
      return { ...previous, center }
    })
  }, [commit])

  const resetLayout = useCallback(() => {
    if (timerRef.current) clearTimeout(timerRef.current)
    timerRef.current = null
    const next = createDefaultWorkbenchLayout()
    setLayout(next)
    setRecovered(false)
    write(serializeWorkbenchLayout(next))
  }, [write])

  const retrySave = useCallback(() => {
    if (pendingRef.current) write(pendingRef.current)
    else write(serializeWorkbenchLayout(layoutRef.current))
  }, [write])

  return useMemo(() => ({
    layout,
    recovered,
    hydrated,
    saveState,
    saveError,
    setCenterState,
    setPrimaryWidth,
    setAuxiliaryWidth,
    setBottomHeight,
    togglePrimarySidebar,
    toggleAuxiliarySidebar,
    toggleBottomPanel,
    setPrimaryActiveView,
    setBottomActiveView,
    openBottomPanel,
    resetLayout,
    retrySave,
  }), [
    layout, recovered, hydrated, saveState, saveError, setCenterState,
    setPrimaryWidth, setAuxiliaryWidth, setBottomHeight,
    togglePrimarySidebar, toggleAuxiliarySidebar, toggleBottomPanel,
    setPrimaryActiveView, setBottomActiveView, openBottomPanel, resetLayout, retrySave,
  ])
}

function serializeAndParse(layout) {
  return JSON.parse(serializeWorkbenchLayout(layout))
}
