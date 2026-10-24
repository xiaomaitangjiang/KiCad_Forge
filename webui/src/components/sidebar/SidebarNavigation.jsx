import { useTranslation } from 'react-i18next'
import { AllSymbolsIcon, IssuesIcon, MatchIcon } from './SidebarIcons'

/// 工作空间视图内的筛选导航：全部 / 问题 / 匹配。
/// 这里只发起意图 —— 查询、筛选与选中怎么变都由 App 的 handler 决定；
/// 徽标只显示计数，activeAll / activeIssues 是 App 传下来的高亮状态。
export default function SidebarNavigation({
  activeAll, activeIssues, allCount, allCountTitle, allCountLabel, issuesCount, matchesCount,
  onSelectAll, onSelectIssues, onAutoMatch,
}) {
  const { t } = useTranslation()

  return (
    <div className="section sidebar-navigation">
      <div className={`nav-item ${activeAll ? 'active' : ''}`} onClick={() => onSelectAll?.()}>
        <AllSymbolsIcon />
        {t('sidebar.allSymbols')}{' '}
        <span className="badge" data-testid="sidebar-all-symbols-count"
          title={allCountTitle || undefined} aria-label={allCountLabel || undefined}>{allCount}</span>
      </div>
      <div className={`nav-item ${activeIssues ? 'active' : ''}`} onClick={() => onSelectIssues()}>
        <IssuesIcon />
        {t('sidebar.issues')} <span className="badge" style={activeIssues ? {background:'var(--orange)',color:'#fff'} : (issuesCount > 0 ? {background:'var(--orange)',color:'#fff'} : undefined)}>{issuesCount}</span>
      </div>
      <div className="nav-item" onClick={() => onAutoMatch()}>
        <MatchIcon />
        {t('sidebar.match')} <span className="badge">{matchesCount}</span>
      </div>
    </div>
  )
}
