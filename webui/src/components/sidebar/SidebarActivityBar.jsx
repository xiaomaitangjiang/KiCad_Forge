import { useTranslation } from 'react-i18next'
import { AllSymbolsIcon, PluginIcon, SettingsIcon } from './SidebarIcons'
import { PluginContributionIcon } from '../../plugin-host/slot-host'

/// 活动栏：工作空间 + 每个插件通过 sidebar.panel 注册的独占视图 + 底部设置。
/// 只决定选中哪个视图；插件图标/标题全部来自注册项，宿主不认识具体插件。
export const WORKSPACE_VIEW = 'kf:workspace'

export default function SidebarActivityBar({ activities = [], activeId, onSelect, settings, host = null }) {
  const { t } = useTranslation()
  const workspaceLabel = t('sidebar.workspaceTitle')

  return (
    <div className="activitybar">
      <button type="button"
        className={`nav-item ${activeId === WORKSPACE_VIEW ? 'selected' : ''}`}
        title={workspaceLabel} aria-label={workspaceLabel}
        onClick={() => onSelect(WORKSPACE_VIEW)}>
        <AllSymbolsIcon />
        <span className="activity-label">{workspaceLabel}</span>
      </button>
      {activities.map((entry) => {
        const id = entry.options.id
        const label = entry.options.label || entry.contribution?.localId || id
        return (
          <button type="button" key={id}
            className={`nav-item ${activeId === id ? 'selected' : ''}`}
            title={label} aria-label={label}
            onClick={() => onSelect(id)}>
            <PluginContributionIcon host={host} slot="sidebar.panel" entry={entry}
              fallback={<PluginIcon />} />
            <span className="activity-label">{label}</span>
          </button>
        )
      })}
      <div className="activitybar-spacer" />
      <button type="button" className="nav-item"
        title={t('sidebar.settings')} aria-label={t('sidebar.settings')}
        onClick={() => settings?.onOpen?.()}>
        <SettingsIcon />
        <span className="activity-label">{t('sidebar.settings')}</span>
      </button>
    </div>
  )
}
