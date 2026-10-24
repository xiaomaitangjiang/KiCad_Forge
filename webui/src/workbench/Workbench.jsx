import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { Group, Panel, Separator, usePanelRef } from 'react-resizable-panels'
import Sidebar from '../components/Sidebar'
import ActivityBar from './ActivityBar'
import BottomPanel from './BottomPanel'
import {
  WORKBENCH_CENTER_MIN_HEIGHT,
  WORKBENCH_CENTER_MIN_WIDTH,
  WORKBENCH_DEFAULT_SIZES,
  WORKBENCH_LIMITS,
  WORKBENCH_WORKSPACE_VIEW,
  resolveWorkbenchLayout,
} from './layout'

/**
 * 固定工作台外壳：活动栏常驻，主侧栏/中心/辅助侧栏水平分栏，中心列可再分底部面板。
 * 尺寸只作为 Panel 的初始 px 值；拖动结束后才通过 controller 持久化。
 */
export default function Workbench({
  controller,
  activities = [],
  sidebarProps,
  main,
  inspector,
  statusbar,
  bottomTasks = [],
  bottomProblems = [],
  onOpenSettings,
  host = null,
}) {
  const { t } = useTranslation()
  const rootRef = useRef(null)
  const primaryRef = usePanelRef()
  const auxiliaryRef = usePanelRef()
  const bottomRef = usePanelRef()
  const primaryElementRef = useRef(null)
  const auxiliaryElementRef = useRef(null)
  const bottomElementRef = useRef(null)
  const [viewportWidth, setViewportWidth] = useState(() => (
    typeof window === 'undefined' ? 1280 : Math.round(window.innerWidth)
  ))
  const [viewportHeight, setViewportHeight] = useState(() => (
    typeof window === 'undefined' ? 900 : Math.round(window.innerHeight)
  ))
  const [drawerOpen, setDrawerOpen] = useState(false)

  useEffect(() => {
    const node = rootRef.current
    if (!node) return undefined
    let frame = 0
    const measure = () => {
      frame = 0
      const rect = node.getBoundingClientRect()
      const width = Math.round(rect.width)
      const height = Math.round(rect.height)
      if (width > 0) setViewportWidth(previous => (Math.abs(previous - width) < 1 ? previous : width))
      if (height > 0) setViewportHeight(previous => (Math.abs(previous - height) < 1 ? previous : height))
    }
    const schedule = () => {
      if (!frame) frame = window.requestAnimationFrame(measure)
    }
    measure()
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(schedule)
    observer?.observe(node)
    window.addEventListener('resize', schedule)
    return () => {
      if (frame) window.cancelAnimationFrame(frame)
      observer?.disconnect()
      window.removeEventListener('resize', schedule)
    }
  }, [])

  const resolved = useMemo(
    () => resolveWorkbenchLayout(controller.layout, viewportWidth, viewportHeight),
    [controller.layout, viewportWidth, viewportHeight],
  )

  const activityIds = useMemo(
    () => new Set([WORKBENCH_WORKSPACE_VIEW, ...activities.map(entry => entry.options?.id || entry.id)]),
    [activities],
  )
  const savedViewId = controller.layout.primarySidebar.activeViewId
  const activeView = activityIds.has(savedViewId) ? savedViewId : WORKBENCH_WORKSPACE_VIEW
  const activeEntry = activities.find(entry => (entry.options?.id || entry.id) === activeView) || null

  useEffect(() => {
    if (savedViewId !== activeView) controller.setPrimaryActiveView(activeView)
  }, [savedViewId, activeView, controller])

  useEffect(() => {
    if (resolved.primaryVisible) setDrawerOpen(false)
  }, [resolved.primaryVisible])

  useEffect(() => {
    if (!drawerOpen) return undefined
    const onKeyDown = event => {
      if (event.key === 'Escape') setDrawerOpen(false)
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [drawerOpen])
  // 布局快捷键只在非输入控件里生效，且不拦截 Shift 组合。
  useEffect(() => {
    const onKeyDown = event => {
      if (!(event.ctrlKey || event.metaKey) || event.shiftKey) return
      const target = event.target
      if (target instanceof Element
        && target.closest('input, textarea, select, [contenteditable="true"]')) return
      const key = event.key.toLowerCase()
      if (event.altKey) {
        if (key !== 'b') return
        event.preventDefault()
        controller.toggleAuxiliarySidebar()
        return
      }
      if (key === 'b') {
        event.preventDefault()
        controller.togglePrimarySidebar()
      } else if (key === 'j') {
        event.preventDefault()
        controller.toggleBottomPanel()
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [controller])

  // defaultSize 只在挂载时生效：配置异步水合或窗口收敛后，把实际像素宽度校准一次。
  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      const panel = primaryRef.current
      if (!panel || !resolved.primaryVisible) return
      try {
        const actual = panel.getSize().inPixels
        if (Number.isFinite(actual) && Math.abs(actual - resolved.primaryWidth) > 0.75) panel.resize(resolved.primaryWidth)
      } catch { /* 面板显隐切换的同一帧里还没注册进 Group */ }
    })
    return () => cancelAnimationFrame(frame)
  }, [resolved.primaryWidth, resolved.primaryVisible])

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      const panel = auxiliaryRef.current
      if (!panel || !resolved.auxiliaryVisible) return
      try {
        const actual = panel.getSize().inPixels
        if (Number.isFinite(actual) && Math.abs(actual - resolved.auxiliaryWidth) > 0.75) panel.resize(resolved.auxiliaryWidth)
      } catch { /* 面板显隐切换的同一帧里还没注册进 Group */ }
    })
    return () => cancelAnimationFrame(frame)
  }, [resolved.auxiliaryWidth, resolved.auxiliaryVisible])

  useEffect(() => {
    const frame = requestAnimationFrame(() => {
      const panel = bottomRef.current
      if (!panel || !resolved.bottomVisible) return
      try {
        const actual = panel.getSize().inPixels
        if (Number.isFinite(actual) && Math.abs(actual - resolved.bottomHeight) > 0.75) panel.resize(resolved.bottomHeight)
      } catch { /* 面板显隐切换的同一帧里还没注册进 Group */ }
    })
    return () => cancelAnimationFrame(frame)
  }, [resolved.bottomHeight, resolved.bottomVisible])

  const selectActivity = useCallback((viewId) => {
    if (!controller.layout.primarySidebar.visible) controller.togglePrimarySidebar()
    if (!resolved.primaryVisible) setDrawerOpen(true)
    controller.setPrimaryActiveView(viewId)
  }, [controller, resolved.primaryVisible])

  // 尺寸只在一次拖动/键盘调整结束时读 DOM 提交，拖动过程中不触发业务树重渲染。
  const persistHorizontal = useCallback(() => {
    const primary = primaryElementRef.current?.getBoundingClientRect().width
    if (Number.isFinite(primary)) controller.setPrimaryWidth(primary)
    const auxiliary = auxiliaryElementRef.current?.getBoundingClientRect().width
    if (Number.isFinite(auxiliary)) controller.setAuxiliaryWidth(auxiliary)
  }, [controller])

  const persistVertical = useCallback(() => {
    const height = bottomElementRef.current?.getBoundingClientRect().height
    if (Number.isFinite(height)) controller.setBottomHeight(height)
  }, [controller])

  // defaultSize 只在挂载时生效；双击复位必须先让分栏真正回到默认像素宽度。
  const resetPrimaryWidth = useCallback(() => {
    primaryRef.current?.resize?.(WORKBENCH_DEFAULT_SIZES.primary)
    controller.setPrimaryWidth(WORKBENCH_DEFAULT_SIZES.primary)
  }, [controller])
  const resetAuxiliaryWidth = useCallback(() => {
    auxiliaryRef.current?.resize?.(WORKBENCH_DEFAULT_SIZES.auxiliary)
    controller.setAuxiliaryWidth(WORKBENCH_DEFAULT_SIZES.auxiliary)
  }, [controller])
  const resetBottomHeight = useCallback(() => {
    bottomRef.current?.resize?.(WORKBENCH_DEFAULT_SIZES.bottom)
    controller.setBottomHeight(WORKBENCH_DEFAULT_SIZES.bottom)
  }, [controller])

  const sidebar = (
    <Sidebar
      {...sidebarProps}
      activities={activities}
      activeView={activeView}
      activeEntry={activeEntry}
      host={host}
    />
  )

  const centerContent = resolved.bottomVisible ? (
    <Group orientation="vertical" className="workbench-vgroup" onPointerUp={persistVertical}>
      <Panel id="workbench-center" minSize={WORKBENCH_CENTER_MIN_HEIGHT}>
        <div className="workbench-main-slot">{main}</div>
      </Panel>
      <Separator
        id="workbench-separator-bottom"
        className="workbench-resizer workbench-resizer-horizontal"
        disableDoubleClick
        onDoubleClick={resetBottomHeight}
        onKeyUp={persistVertical}
      />
      <Panel
        id="workbench-bottom"
        panelRef={bottomRef}
        elementRef={bottomElementRef}
        groupResizeBehavior="preserve-pixel-size"
        defaultSize={resolved.bottomHeight}
        minSize={WORKBENCH_LIMITS.bottom.min}
        maxSize={WORKBENCH_LIMITS.bottom.max}
      >
        <BottomPanel
          activeViewId={controller.layout.bottomPanel.activeViewId}
          onChange={controller.setBottomActiveView}
          onClose={controller.toggleBottomPanel}
          tasks={bottomTasks}
          problems={bottomProblems}
        />
      </Panel>
    </Group>
  ) : (
    <div className="workbench-main-slot">{main}</div>
  )

  return (
    <div className="workbench" ref={rootRef}>
      <div className="workbench-body">
        <ActivityBar
          activities={activities}
          activeId={activeView}
          onSelect={selectActivity}
          onOpenSettings={onOpenSettings}
          host={host}
        />
        <Group orientation="horizontal" className="workbench-hgroup" onPointerUp={persistHorizontal}>
          {resolved.primaryVisible && (
            <>
              <Panel
                id="workbench-primary"
                panelRef={primaryRef}
                elementRef={primaryElementRef}
                groupResizeBehavior="preserve-pixel-size"
                defaultSize={resolved.primaryWidth}
                minSize={WORKBENCH_LIMITS.primary.min}
                maxSize={WORKBENCH_LIMITS.primary.max}
              >
                <div className="workbench-side-panel">{sidebar}</div>
              </Panel>
              <Separator
                id="workbench-separator-primary"
                className="workbench-resizer workbench-resizer-vertical"
                disableDoubleClick
                onDoubleClick={resetPrimaryWidth}
                onKeyUp={persistHorizontal}
              />
            </>
          )}
          <Panel id="workbench-center-column" minSize={WORKBENCH_CENTER_MIN_WIDTH}>
            {centerContent}
          </Panel>
          {resolved.auxiliaryVisible && (
            <>
              <Separator
                id="workbench-separator-auxiliary"
                className="workbench-resizer workbench-resizer-vertical"
                disableDoubleClick
                onDoubleClick={resetAuxiliaryWidth}
                onKeyUp={persistHorizontal}
              />
              <Panel
                id="workbench-auxiliary"
                panelRef={auxiliaryRef}
                elementRef={auxiliaryElementRef}
                groupResizeBehavior="preserve-pixel-size"
                defaultSize={resolved.auxiliaryWidth}
                minSize={WORKBENCH_LIMITS.auxiliary.min}
                maxSize={WORKBENCH_LIMITS.auxiliary.max}
              >
                <div className="workbench-side-panel">{inspector}</div>
              </Panel>
            </>
          )}
        </Group>
      </div>
      {statusbar}
      {drawerOpen && !resolved.primaryVisible && (
        <div className="workbench-overlay">
          <button
            type="button"
            className="workbench-overlay-backdrop"
            aria-label={t('workbench.closeSidebar')}
            onClick={() => setDrawerOpen(false)}
          />
          <div
            className="workbench-overlay-panel"
            role="dialog"
            aria-modal="true"
            aria-label={t('sidebar.workspaceTitle')}
            style={{ width: Math.max(WORKBENCH_LIMITS.primary.min, Math.min(WORKBENCH_LIMITS.primary.max, resolved.primaryWidth)) }}
          >
            <button
              type="button"
              className="workbench-overlay-close"
              aria-label={t('workbench.closeSidebar')}
              onClick={() => setDrawerOpen(false)}
            >×</button>
            {sidebar}
          </div>
        </div>
      )}
    </div>
  )
}