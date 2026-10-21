import { useEffect, useRef, useState } from 'react'
import { Group, Panel, Separator, usePanelRef } from 'react-resizable-panels'
import CenterWorkspace from './CenterWorkspace'

const COLLAPSE_BELOW_PX = 600

function CenterPanel({ panelId, size, groupId, panelRefs, children }) {
  const ref = usePanelRef()
  useEffect(() => {
    if (!groupId) return undefined
    const registry = panelRefs.current
    registry.set(groupId, ref)
    return () => { registry.delete(groupId) }
  }, [groupId, panelRefs, ref])
  return (
    <Panel panelRef={ref} id={`center-panel-${panelId}`} defaultSize={String(size ?? 100)}
      minSize="15" maxSize="100" className="center-group-panel">
      {children}
    </Panel>
  )
}

function GroupView({ group, center, tabFor, breadcrumbFor, renderPage, isActive, onDropTarget }) {
  const tabs = group.pages.map(tabFor)
  const [draggingOver, setDraggingOver] = useState(false)
  return (
    <div
      className={`center-group${isActive ? ' active' : ''}${draggingOver ? ' drop-target' : ''}`}
      data-center-group={group.id}
      data-center-group-active={isActive ? 'true' : 'false'}
      onPointerDown={() => center.activateGroup(group.id)}
      onDragOver={(event) => {
        event.preventDefault()
        event.dataTransfer.dropEffect = 'move'
        if (!draggingOver) { setDraggingOver(true); onDropTarget?.(group.id) }
      }}
      onDragLeave={(event) => {
        if (event.currentTarget.contains(event.relatedTarget)) return
        setDraggingOver(false)
        onDropTarget?.(null)
      }}
      onDrop={(event) => {
        event.preventDefault()
        const key = event.dataTransfer.getData('text/plain')
        setDraggingOver(false)
        onDropTarget?.(null)
        if (key) center.movePage(key, group.id)
      }}
    >
      <CenterWorkspace
        groupId={group.id}
        tabs={tabs}
        activeId={group.active}
        onSelect={(key) => center.selectInGroup(group.id, key)}
        onClose={(key) => center.close(key, group.id)}
        onBack={() => center.back(group.id)}
        canBack={group.index > 0}
        breadcrumb={breadcrumbFor(group.active)}
        onSplit={(direction) => center.splitGroup(direction)}
        onMoveNext={() => center.moveNext()}
        canSplit={center.canSplit}
        canMoveNext={center.canMoveNext}
      >
        {group.pages.map((key) => (
          <div key={key} className="center-stack-layer" hidden={key !== group.active}>
            {renderPage(key)}
          </div>
        ))}
      </CenterWorkspace>
    </div>
  )
}

function renderTreeNode(node, key, context, insideParent = false) {
  if (node.type === 'group') {
    const group = context.center.groups.find((item) => item.id === node.groupId)
    if (!group) return null
    const panel = (
      <CenterPanel key={key} panelId={node.groupId} size={node.size} groupId={node.groupId}
        panelRefs={context.panelRefs}>
        <GroupView group={group} center={context.center} tabFor={context.tabFor}
          breadcrumbFor={context.breadcrumbFor} renderPage={context.renderPage}
          isActive={group.id === context.center.activeGroupId}
          onDropTarget={context.onDropTarget} />
      </CenterPanel>
    )
    // Panel 必须挂在 Group 下；根节点只有一组时直接返回 Panel 会挂进
    // Workbench 外层 Group，挤掉主侧栏宽度。
    if (insideParent) return panel
    return (
      <Group key={'root-' + key} orientation="horizontal" className="center-groups-single"
        style={{ width: '100%', height: '100%' }}>
        {panel}
      </Group>
    )
  }
  const orientation = node.direction === 'row' ? 'horizontal' : 'vertical'
  const children = []
  node.children.forEach((child, index) => {
    if (index > 0) children.push(<Separator key={`separator-${key}-${index}`} className="center-split-separator" />)
    children.push(renderTreeNode(child, `${key}-${child.groupId || index}`, context, true))
  })
  const split = (
    <Group key={key} orientation={orientation} className={`center-split center-split-${node.direction}`}
      style={{ width: '100%', height: '100%' }}>
      {children}
    </Group>
  )
  if (!insideParent) return split
  return (
    <CenterPanel key={`panel-${key}`} panelId={key} size={node.size} panelRefs={context.panelRefs}>
      {split}
    </CenterPanel>
  )
}

/**
 * 中心区第二棵树：split/group 只描述布局，页面身份由 useCenterNavigation 持有。
 * 用 ResizeObserver 只量中心宽度；窄窗临时只显示活动组，不改持久化尺寸。
 */
export default function CenterGroups({ center, tabFor, breadcrumbFor, renderPage }) {
  const rootRef = useRef(null)
  const panelRefs = useRef(new Map())
  const [width, setWidth] = useState(0)
  const [dropTarget, setDropTarget] = useState(null)

  useEffect(() => {
    const node = rootRef.current
    if (!node || typeof ResizeObserver === 'undefined') return undefined
    const observer = new ResizeObserver((entries) => {
      const next = Math.round(entries[0]?.contentRect?.width || 0)
      setWidth((previous) => (previous === next ? previous : next))
    })
    observer.observe(node)
    return () => observer.disconnect()
  }, [])

  const collapsed = width > 0 && width < COLLAPSE_BELOW_PX
  const activeGroup = center.groups.find((group) => group.id === center.activeGroupId) || center.groups[0]
  const tree = collapsed && activeGroup ? { type: 'group', groupId: activeGroup.id } : center.tree

  const flushSizes = () => {
    if (collapsed) return
    const sizes = {}
    for (const [groupId, ref] of panelRefs.current) {
      const size = ref.current?.getSize()?.asPercentage
      if (Number.isFinite(size)) sizes[groupId] = size
    }
    center.resizeGroups(sizes)
  }

  if (!tree || !activeGroup) return null
  return (
    <div ref={rootRef} className="center-groups-root" data-center-collapsed={collapsed ? 'true' : 'false'}>
      <div className="center-groups" onPointerUp={flushSizes}>
        {renderTreeNode(tree, 'root', {
          center, tabFor, breadcrumbFor, renderPage, panelRefs, dropTarget,
          onDropTarget: setDropTarget,
        })}
        {collapsed && center.groups.filter((group) => group.id !== activeGroup.id).map((group) => (
          <div key={group.id} className="center-collapsed-hidden" hidden>
            {group.pages.map((key) => <div key={key}>{renderPage(key)}</div>)}
          </div>
        ))}
      </div>
    </div>
  )
}
