import { useTranslation } from 'react-i18next'

export default function SymbolTable({ symbols, filter, issueFilter, selected, setSelected }) {
  const { t } = useTranslation()
  const filtered = (() => {
    let list = symbols
    if (filter) list = list.filter(s => s.type === filter)
    if (issueFilter) list = list.filter(s => !s.footprint || !s.has_3d_model)
    return list
  })()

  if (filtered.length === 0) return (
    <div className="content">
      <table><tbody><tr><td colSpan={6}>
        <div className="empty-state">
          <div className="icon"><svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg></div>
          <p style={{ fontWeight:600 }}>{issueFilter ? t('toast.noIssues') : t('app.noSymbols')}</p>
          <p>{issueFilter ? '' : t('app.noSymbolsHint')}</p>
        </div>
      </td></tr></tbody></table>
    </div>
  )

  return (
    <div className="content">
      <table>
        <thead><tr>
          <th style={{ width:36 }}></th>
          <th>Name</th><th style={{ width:140 }}>Type</th>
          <th style={{ width:200 }}>Footprint</th>
          <th style={{ width:64, textAlign:'center' }}>Pins</th>
          <th style={{ width:160 }}>MPN</th>
        </tr></thead>
        <tbody>
          {filtered.map(s => (
            <tr key={s.id} className={selected?.id === s.id ? 'selected' : ''} onClick={() => setSelected(s)}>
              <td>
                {s.has_3d_model
                  ? <span style={{ color:'var(--green)', fontSize:14 }} title="3D model linked">●</span>
                  : s.has_footprint
                    ? <span style={{ color:'var(--orange)', fontSize:14 }} title="Footprint assigned, no 3D">●</span>
                    : <span style={{ color:'var(--red)', fontSize:14 }} title="No footprint">●</span>}
              </td>
              <td className="name">{s.name}</td>
              <td><span className="type-tag">{s.type || 'Unknown'}</span></td>
              <td style={{ fontFamily:'var(--mono)', fontSize:11 }}>{s.footprint || '—'}</td>
              <td style={{ textAlign:'center', fontFamily:'var(--mono)' }}>{s.pins || '—'}</td>
              <td style={{ fontFamily:'var(--mono)', fontSize:11 }}>{s.mpn || '—'}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  )
}
