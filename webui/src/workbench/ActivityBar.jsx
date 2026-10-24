import { useTranslation } from 'react-i18next'
import { AllSymbolsIcon, PluginIcon, SettingsIcon } from '../components/sidebar/SidebarIcons'
import { PluginContributionIcon } from '../plugin-host/slot-host'
import { WORKBENCH_WORKSPACE_VIEW } from './layout'

/**
 * 活动栏独立成 48px 常驻列，不随主侧栏伸缩。
 * 插件入口仍然只来自 sidebar.panel 槽，宿主不认插件 id。
 */
export default function ActivityBar({ activities = [], activeId, onSelect, onOpenSettings, host = null }) {
  const { t } = useTranslation()
  return (
    <div className="activitybar" role="tablist" aria-label={t('sidebar.workspaceTitle')}>
      <button
        type="button"
        role="tab"
        aria-selected={activeId === WORKBENCH_WORKSPACE_VIEW}
        className={`nav-item activity-item${activeId === WORKBENCH_WORKSPACE_VIEW ? ' selected' : ''}`}
        title={t('sidebar.workspaceTitle')}
        onClick={() => onSelect(WORKBENCH_WORKSPACE_VIEW)}
      >
        <AllSymbolsIcon />
        <span className="activity-label">{t('sidebar.workspaceTitle')}</span>
      </button>

      {activities.map(entry => {
        const id = entry.options?.id || entry.id
        return (
          <button
            key={entry.id}
            type="button"
            role="tab"
            aria-selected={activeId === id}
            className={`nav-item activity-item${activeId === id ? ' selected' : ''}`}
            title={entry.options?.label || entry.id}
            onClick={() => onSelect(id)}
          >
            <PluginContributionIcon
              host={host}
              slot="sidebar.panel"
              entry={entry}
              fallback={<PluginIcon />}
            />
            <span className="activity-label">{entry.options?.label || entry.id}</span>
          </button>
        )
      })}

      <div className="activitybar-spacer" />
      <button
        type="button"
        className="nav-item activity-item"
        title={t('sidebar.settings')}
        aria-label={t('sidebar.settings')}
        onClick={onOpenSettings}
      >
        <SettingsIcon />
        <span className="activity-label">{t('sidebar.settings')}</span>
      </button>
    </div>
  )
}
