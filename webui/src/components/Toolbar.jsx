import { useTranslation } from 'react-i18next'
import { api } from '../api'

const DEFAULT_ICON = <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>

export default function Toolbar({ plugins, loadLibraries, setPluginAction, setPluginFormData, classify, checkCorrespondence, autoMatch, status, search, setSearch, loadSymbols }) {
  const { t } = useTranslation()

  return (
    <div className="toolbar">
      <button className="btn-primary" onClick={classify}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>
        {t('toolbar.autoClassify')}
      </button>
      <button className="btn-ghost" onClick={checkCorrespondence}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
        {t('toolbar.check')}
      </button>
      <button className="btn-ghost" onClick={autoMatch}>
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>
        {t('toolbar.autoMatch')}
      </button>
      <span className="sep" />

      {/* Dynamic plugin buttons */}
      {plugins.filter(p => p.actions?.length > 0).map(p =>
        p.actions.filter(a => a.button?.show !== false).map(action => {
          const btn = action.button || {}
          const style = btn.style || 'both'
          const iconSrc = action.icon_url || p.icon_url
          const iconEl = iconSrc
            ? <img src={iconSrc} alt="" width="14" height="14" style={{ objectFit:'contain' }} />
            : DEFAULT_ICON
          return (
            <button key={`${p.id}/${action.id}`}
              className={style === 'icon' ? 'btn-icon' : 'btn-primary'}
              title={btn.tooltip || action.description}
              onClick={() => { loadLibraries(); setPluginAction({ plugin:p, action }); setPluginFormData({}) }}>
              {(style === 'icon' || style === 'both') && iconEl}
              {style !== 'icon' && <span>{action.name}</span>}
            </button>
          )
        })
      )}
      <span className="sep" />

      <input placeholder={t('toolbar.searchPlaceholder')} value={search}
        onChange={e => { setSearch(e.target.value); loadSymbols(e.target.value) }} />
      <span className="sep" />
      <span style={{ fontSize:11, color:'var(--text2)', whiteSpace:'nowrap' }}>
        {t('toolbar.symbolsFootprints', { symbols: status.symbols, footprints: status.footprints })}
      </span>
    </div>
  )
}
