import { useTranslation } from 'react-i18next'

/// 工作库选择：items 是可选工作库，selectedId 为空 = 不限定工作库（下游显示全部），
/// 具体 ID = 只看归属这个工作库的符号库。没有工作库时整块不画。
/// onSelect 只发选择意图，切库带来的查询重置与保存由 App 做。
export default function SidebarWorkspaceSelector({ items, selectedId, onSelect }) {
  const { t } = useTranslation()
  if ((items || []).length === 0) return null

  return (
    <div className="sidebar-workspace">
      <select className="kf-select" value={selectedId || ''} onChange={e => onSelect(e.target.value)}
        title={t('sidebar.workspace')} aria-label={t('sidebar.workspace')}>
        <option value="">{t('sidebar.allWorkspaces')}</option>
        {items.map(c => (
          <option key={c.id} value={c.id}>{c.locked ? '🔒 ' : ''}{c.name}</option>
        ))}
      </select>
    </div>
  )
}
