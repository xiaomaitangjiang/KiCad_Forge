import { useTranslation } from 'react-i18next'

// 中心区外壳：页签、面包屑和返回只处理页面身份，不持有页面业务状态。
export default function CenterWorkspace({
  tabs = [], activeId, onSelect, onClose, onBack, canBack = false, breadcrumb = [], children,
  groupId, onSplit, onMoveNext, canSplit = false, canMoveNext = false,
}) {
  const { t } = useTranslation()
  const current = tabs.find(tab => tab.id === activeId)
  return (
    <div className="center-workspace">
      <div className="center-tabbar" role="tablist" aria-label={t('workbench.centerTabs')}
        data-center-group-tabbar={groupId || undefined}>
        <button className="center-back" onClick={onBack} disabled={!canBack}
          aria-label={t('workbench.back')} title={t('workbench.back')}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
            <path d="M15 18l-6-6 6-6" />
          </svg>
        </button>
        <div className="center-tabs">
          {tabs.map(tab => (
            <div key={tab.id} className={`center-tab${tab.id === activeId ? ' active' : ''}`} draggable
              onDragStart={(event) => {
                event.dataTransfer.setData('text/plain', tab.id)
                event.dataTransfer.effectAllowed = 'move'
              }}>
              <button className="center-tab-label" role="tab" aria-selected={tab.id === activeId}
                title={tab.label} onClick={() => onSelect?.(tab.id)}>
                {tab.icon}{tab.label}
              </button>
              {tab.closable && (
                <button className="center-tab-close" data-center-tab-close={tab.id}
                  aria-label={t('workbench.closeTab', { name: tab.label })}
                  title={t('workbench.closeTab', { name: tab.label })}
                  onClick={(event) => { event.stopPropagation(); onClose?.(tab.id) }}>
                  <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
                    <line x1="18" y1="6" x2="6" y2="18" /><line x1="6" y1="6" x2="18" y2="18" />
                  </svg>
                </button>
              )}
            </div>
          ))}
          {tabs.length === 0 && <span className="center-group-empty-label">{t('workbench.emptyGroup')}</span>}
        </div>
        <details className="center-group-menu" data-center-group-menu={groupId || ''}>
          <summary aria-label={t('workbench.groupMenu')} title={t('workbench.groupMenu')}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true">
              <circle cx="12" cy="5" r="1" /><circle cx="12" cy="12" r="1" /><circle cx="12" cy="19" r="1" />
            </svg>
          </summary>
          <div className="center-group-menu-popover" role="menu">
            <button role="menuitem" data-center-split="row" disabled={!canSplit} onClick={() => onSplit?.('row')}>
              {t('workbench.splitRight')}
            </button>
            <button role="menuitem" data-center-split="column" disabled={!canSplit} onClick={() => onSplit?.('column')}>
              {t('workbench.splitDown')}
            </button>
            <button role="menuitem" data-center-move-next disabled={!canMoveNext} onClick={() => onMoveNext?.()}>
              {t('workbench.moveToNextGroup')}
            </button>
          </div>
        </details>
      </div>
      {breadcrumb.length > 0 && (
        <nav className="center-breadcrumb" aria-label={t('workbench.breadcrumb')}>
          {breadcrumb.map((item, index) => {
            const label = typeof item === 'string' ? item : item?.label
            const page = typeof item === 'object' ? item?.page : undefined
            const last = index === breadcrumb.length - 1
            return (
              <span key={`${page || label}:${index}`} className={last ? 'current' : ''}>
                {index > 0 && <span className="center-breadcrumb-sep" aria-hidden="true">/</span>}
                {!last && page ? (
                  <button className="center-breadcrumb-link" onClick={() => onSelect?.(page)}>{label}</button>
                ) : label}
              </span>
            )
          })}
        </nav>
      )}
      <div className="center-page" role="tabpanel" aria-label={current?.label || ''}>
        {current ? children : (
          <div className="center-group-placeholder">
            <p>{t('workbench.emptyGroupHint')}</p>
          </div>
        )}
      </div>
    </div>
  )
}
