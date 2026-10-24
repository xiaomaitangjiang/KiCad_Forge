import { useMemo } from 'react'
import { useTranslation } from 'react-i18next'
import SidebarWorkspaceSelector from './sidebar/SidebarWorkspaceSelector'
import SidebarNavigation from './sidebar/SidebarNavigation'
import SidebarTypeFilter from './sidebar/SidebarTypeFilter'
import SidebarLibraryList from './sidebar/SidebarLibraryList'
import { PluginSections } from './PluginSlot'
import { PluginEntry } from '../plugin-host/slot-host'
import { appPluginRuntime } from '../plugin-host/app-host'
import { WORKBENCH_WORKSPACE_VIEW } from '../workbench/layout'

/// 主侧栏只负责当前活动视图的内容；活动栏由 Workbench 独立渲染，不随侧栏宽度伸缩。
/// 工作空间视图仍是四组职责（导航 / 工作库 / 类型筛选 / 符号库列表 + 嵌入 sections）。
export default function Sidebar({
  workspace,
  navigation,
  types,
  libraries,
  views = null,
  activities = [],
  activeView = WORKBENCH_WORKSPACE_VIEW,
  activeEntry = null,
  host = appPluginRuntime.host,
}) {
  const { t } = useTranslation()
  const entry = activeEntry || activities.find(item => (item.options?.id || item.id) === activeView) || null
  const ownerProps = useMemo(() => ({
    workspaceId: workspace.selectedId || null,
    counts: { symbols: navigation.allCount, issues: navigation.issuesCount, matches: navigation.matchesCount },
    openView: views?.onOpenPlugin,
  }), [workspace.selectedId, navigation.allCount, navigation.issuesCount, navigation.matchesCount, views?.onOpenPlugin])
  // 第三方插件仍可用 app.view 中心页（WP-009）；内置 LCSC/Git 已改为只注册
  // sidebar.panel，因此不会出现在这里，也不会产生重复入口。
  const pluginViews = views?.items || []

  return (
    <nav className="sidebar">
      <div className="sidebar-header">
        {entry
          ? (entry.options?.label || entry.contribution?.localId || entry.options?.id)
          : t('sidebar.workspaceTitle')}
      </div>

      {entry ? (
        <div className="sidebar-plugin-view">
          <PluginEntry
            key={entry.options?.id || entry.id}
            host={host}
            slot="sidebar.panel"
            entry={entry}
            ownerProps={ownerProps}
          />
        </div>
      ) : (
        <>
          <SidebarNavigation {...navigation} />

          <SidebarWorkspaceSelector {...workspace} />

          <SidebarTypeFilter {...types} />

          <SidebarLibraryList {...libraries} />

          <div className="section sidebar-plugin-sections">
            <PluginSections slot="sidebar.section" ownerProps={ownerProps} host={host} />
          </div>

          {pluginViews.length > 0 && (
            <div className="section sidebar-plugin-views">
              <div className="section-title">{t('sidebar.views')}</div>
              {pluginViews.map((view) => {
                const viewId = view.options?.id || view.id
                return (
                  <div key={viewId}
                    className={`nav-item ${views.selectedId === viewId ? 'active' : ''}`}
                    onClick={() => views.onSelect?.(viewId)}>
                    {view.options?.label || view.contribution?.localId || viewId}
                  </div>
                )
              })}
            </div>
          )}

        </>
      )}
    </nav>
  )
}
