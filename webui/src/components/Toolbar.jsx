import { useTranslation } from 'react-i18next'
import { PluginSlot } from './PluginSlot'

/**
 * 首页工具栏：宿主批量操作、插件贡献入口和搜索框。
 * 窄容器时次要动作只留图标（CSS container query），所以每个图标按钮都必须
 * 有无障碍名称与 title，不能只靠可见文字（ED 界面验收：可访问名称不裁剪）。
 */
export default function Toolbar({ classify, checkCorrespondence, autoMatch, workspaceId, status, search, setSearch, loadSymbols, onSearch }) {
  const { t } = useTranslation()
  const autoClassifyLabel = t('toolbar.autoClassify')
  const checkLabel = t('toolbar.check')
  const autoMatchLabel = t('toolbar.autoMatch')
  const searchLabel = t('toolbar.searchPlaceholder')

  return (
    <div className="toolbar" role="toolbar" aria-label={t('toolbar.label')}>
      <button type="button" className="btn-primary" onClick={classify}
        aria-label={autoClassifyLabel} title={autoClassifyLabel}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" aria-hidden="true"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
        <span className="toolbar-action-label">{autoClassifyLabel}</span>
      </button>
      <button type="button" className="btn-ghost" onClick={checkCorrespondence}
        aria-label={checkLabel} title={checkLabel}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
        <span className="toolbar-action-label">{checkLabel}</span>
      </button>
      <button type="button" className="btn-ghost" onClick={autoMatch}
        aria-label={autoMatchLabel} title={autoMatchLabel}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" aria-hidden="true"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
        <span className="toolbar-action-label">{autoMatchLabel}</span>
      </button>
      <span className="sep" aria-hidden="true" />

      <PluginSlot slot="toolbar.action" ownerProps={{ workspaceId: workspaceId || null, status }} />

      <input type="search" className="toolbar-search" aria-label={searchLabel} placeholder={searchLabel}
        autoComplete="off" spellCheck="false" value={search}
        onChange={e => {
          setSearch(e.target.value)
          // Search stays within the current library context (unified query state)
          if (onSearch) onSearch(e.target.value)
          else loadSymbols(e.target.value)
        }} />
    </div>
  )
}
