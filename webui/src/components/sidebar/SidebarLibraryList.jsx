import { useTranslation } from 'react-i18next'

/// 符号库列表：工作库范围内的一层平铺列表（workspaceId 为空 = 显示全部库，
/// 向后兼容）。删除只是「请求删除」意图：不在这里改任何状态，也不能连带
/// 触发选中，所以 × 必须自己阻止冒泡。
export default function SidebarLibraryList({ items, workspaceId, selectedId, onSelect, onDeleteRequest }) {
  const { t } = useTranslation()
  // Workspace filter: show only libraries of the active component library
  // (empty workspaceId → show everything, backwards compatible)
  const visibleLibraries = workspaceId
    ? items.filter(l => l.component_library_id === workspaceId)
    : items

  return (
    <div className="section sidebar-libraries">
      <div className="section-title">{t('sidebar.libraries')}</div>
      <div className="sidebar-library-list">
        {visibleLibraries.map(l => (
          <div key={l.id} className={`nav-item sidebar-library-row ${selectedId === l.id ? 'active' : ''}`}
            onClick={() => onSelect?.(l.id)}>
            <span className="sidebar-library-name">
              <span className="dot dot-ok" /> {l.locked ? '🔒 ' : ''}{l.name}
            </span>
            {/* Delete — symbol library: unregister by default, opt-in file removal */}
            <span className="sidebar-library-delete"
              onClick={e => { e.stopPropagation(); onDeleteRequest({ kind: 'symbol', lib: l }) }}>×</span>
          </div>
        ))}
        {visibleLibraries.length === 0 && (
          <div className="sidebar-empty">{t('sidebar.noLibraries')}</div>
        )}
      </div>
    </div>
  )
}
