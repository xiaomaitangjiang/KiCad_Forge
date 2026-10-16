import { useTranslation } from 'react-i18next'

// Vertical card list — one card per component (name / footprint / type).
export default function SymbolTable({ symbols, filter, issueFilter, selected, setSelected }) {
  const { t } = useTranslation()
  const filtered = (() => {
    let list = symbols
    if (filter) list = list.filter(s => (s.type_id || s.type) === filter)
    if (issueFilter) list = list.filter(s => !s.footprint || !s.has_3d_model)
    return list
  })()

  if (filtered.length === 0) return (
    <div className="content">
      <div className="empty-state">
        <div className="icon"><svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg></div>
        <p style={{ fontWeight:600 }}>{issueFilter ? t('toast.noIssues') : t('app.noSymbols')}</p>
        <p>{issueFilter ? '' : t('app.noSymbolsHint')}</p>
      </div>
    </div>
  )

  return (
    <div className="content">
      <div className="symbol-list">
        {filtered.map(s => {
          const status = s.has_3d_model
            ? { cls: 'ok', title: '3D model linked' }
            : s.has_footprint
              ? { cls: 'warn', title: 'Footprint assigned, no 3D' }
              : { cls: 'bad', title: 'No footprint' }
          return (
            <div key={s.id} className={`symbol-card ${selected?.id === s.id ? 'selected' : ''}`}
              role="button" tabIndex={0}
              onClick={() => setSelected(s)}
              onKeyDown={e => {
                if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); setSelected(s) }
              }}>
              <span className={`symbol-status ${status.cls}`} title={status.title} />
              <div className="symbol-main">
                <div className="symbol-name">{s.name}</div>
                <div className="symbol-sub">
                  <span className="symbol-fp" title={s.footprint || ''}>{s.footprint || '—'}</span>
                  {s.pins ? <span className="symbol-meta">{s.pins}p</span> : null}
                  {s.mpn ? <span className="symbol-meta">{s.mpn}</span> : null}
                </div>
              </div>
              <span className="type-tag">{s.type || 'Unknown'}</span>
            </div>
          )
        })}
      </div>
    </div>
  )
}
