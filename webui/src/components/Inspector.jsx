import { useTranslation } from 'react-i18next'

const TYPE_ICONS = {
  Resistor:'⊟',Capacitor:'⊞',Inductor:'◠',Diode:'▷',LED:'☀',
  Microcontroller:'⬡',Connector:'▣',Crystal:'◇',VoltageRegulator:'▦',
  Transistor:'△',MOSFET:'△',FPGA:'◰',Memory:'▤',Oscillator:'◆',
  Fuse:'◎',Switch:'⏣',Relay:'⏻',Jumper:'⌸',TestPoint:'◎',
}

export default function Inspector({ selected, classify, autoMatch, onDeleteSymbol }) {
  const { t } = useTranslation()
  if (!selected) return (
    <div className="inspector">
      <div className="inspector-empty">
        <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" opacity="0.3"><circle cx="12" cy="12" r="10"/><path d="M8 14s1.5 2 4 2 4-2 4-2"/><line x1="9" y1="9" x2="9.01" y2="9"/><line x1="15" y1="9" x2="15.01" y2="9"/></svg>
        <p>{t('app.selectComponent')}<br/>{t('app.selectComponentHint')}</p>
      </div>
    </div>
  )

  return (
    <div className="inspector">
      <div className="inspector-header">
        <div className="inspector-icon">{TYPE_ICONS[selected.type] || '○'}</div>
        <div><h4>{selected.name}</h4><div className="inspector-type">{selected.type || 'Unknown'}</div></div>
      </div>
      <div className="inspector-section">
        {[
          ['footprint', selected.footprint || t('inspector.notAssigned')],
          ['3dModel', selected.has_3d_model ? `✓ ${t('inspector.linked')}` : `⚠ ${t('inspector.notLinked')}`],
          ['pinCount', selected.pins || '—'],
          ['mpn', selected.mpn || '—'],
          ['value', selected.value || '—'],
        ].map(([key, val]) => (
          <div className="field" key={key}>
            <label>{t(`inspector.${key}`)}</label>
            <div className={`val ${key === 'footprint' ? (selected.footprint ? 'ok' : 'warn') : key === '3dModel' ? (selected.has_3d_model ? 'ok' : 'warn') : ''}`} style={{ fontSize: (key === 'mpn' || key === 'value') ? 12 : undefined }}>
              {val}
            </div>
          </div>
        ))}
      </div>
      <div className="inspector-section">
        <h5>{t('inspector.quickActions')}</h5>
        <div className="actions">
          <button className="primary" onClick={classify}>{t('toolbar.autoClassify')}</button>
          <button onClick={autoMatch}>{t('inspector.findFootprint')}</button>
          <button style={{ color:'var(--red)', borderColor:'var(--red)' }}
            onClick={() => onDeleteSymbol(selected)}>{t('inspector.deleteSymbol')}</button>
        </div>
      </div>
    </div>
  )
}
