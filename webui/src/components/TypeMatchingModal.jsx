import { useTranslation } from 'react-i18next'
import { Modal } from './Modals'
import CenterPage from '../workbench/CenterPage'
import { useTypeMatching } from '../hooks/useTypeMatching'
import { MATCH_FIELDS, newMatchingEntry, translateMatchingMessage } from '../lib/typeMatching'
import { typeColorOf, typeIconOf, typeLabelOf } from '../lib/typeMetadata'

function TypeLabel({ id, typeMap }) {
  const { t } = useTranslation()
  return <span style={{ color: typeColorOf(typeMap, id) }}>
    {typeIconOf(typeMap, id)} {typeLabelOf(typeMap, id) || t('matching.unclassified')}</span>
}

function TypeSelect({ value, types, onChange, label }) {
  const { t } = useTranslation()
  const found = types.some(item => item.id === value)
  return <select className="kf-select" aria-label={label} value={value} onChange={event => onChange(event.target.value)}>
    <option value="">{t('matching.selectType')}</option>
    {value && !found && <option value={value}>{t('matching.missingType', { id: value })}</option>}
    {types.map(item => <option key={item.id} value={item.id}>{item.icon || '○'} {item.name || item.id}</option>)}
  </select>
}

function EntryList({ kind, entries, types, edit, disabled }) {
  const { t } = useTranslation()
  const isRule = kind === 'rules'
  const noun = t(`matching.kind.${kind}`)
  const change = (id, patch) => edit(kind, list => list.map(row => row.id === id ? { ...row, ...patch } : row))
  return <section className="matching-section" aria-label={noun}>
    <div className="matching-section-title"><h4>{noun}</h4>
      <button className="btn-ghost" disabled={disabled || entries.length >= 500}
        onClick={() => edit(kind, list => [...list, newMatchingEntry(kind)])}>{t('matching.addEntry', { kind: noun })}</button></div>
    {!entries.length && <p className="matching-hint">{t('matching.noEntries', { kind: noun })}</p>}
    {entries.map((row, index) => {
      const prefix = t('matching.entry', { kind: noun, index: index + 1 })
      return <fieldset className="matching-entry" key={row.id} disabled={disabled} data-entry-id={row.id}>
        <legend>{prefix}</legend>
        <div className="matching-entry-heading">
          <label className="matching-check"><input type="checkbox" aria-label={t('matching.entryEnabled', { entry: prefix })} checked={row.enabled}
            onChange={event => change(row.id, { enabled: event.target.checked })} />{t('matching.enabled')}</label>
          <button className="btn-ghost" aria-label={t('matching.deleteEntry', { entry: prefix })}
            onClick={() => edit(kind, list => list.filter(entry => entry.id !== row.id))}>{t('matching.delete')}</button>
        </div>
        <div className="matching-entry-grid">
          <label>{t('matching.sourceField')}<select className="kf-select" aria-label={t('matching.entryField', { entry: prefix })} value={row.field}
            onChange={event => change(row.id, { field: event.target.value })}>
            {MATCH_FIELDS.map(field => <option key={field} value={field}>{t(`matching.field.${field}`)}</option>)}
          </select></label>
          <label>{t('matching.targetType')}<TypeSelect value={row.type_id} types={types} label={t('matching.entryType', { entry: prefix })}
            onChange={type_id => change(row.id, { type_id })} /></label>
          {isRule && <label>{t('matching.priorityHint')}<input type="number" step="1" min="-2147483648" max="2147483647"
            aria-label={t('matching.entryPriority', { entry: prefix })} value={row.priority}
            onChange={event => change(row.id, { priority: event.target.value === '' ? '' : Number(event.target.value) })} /></label>}
        </div>
        <label className="matching-text-label">{t(isRule ? 'matching.patternHint' : 'matching.exactTextHint')}
          <textarea aria-label={t(isRule ? 'matching.entryPattern' : 'matching.entryText', { entry: prefix })} rows={2} spellCheck={false}
            value={isRule ? row.pattern : row.key}
            onChange={event => change(row.id, { [isRule ? 'pattern' : 'key']: event.target.value })} /></label>
        {isRule && <label className="matching-check"><input type="checkbox" aria-label={t('matching.entryCaseSensitive', { entry: prefix })}
          checked={row.case_sensitive} onChange={event => change(row.id, { case_sensitive: event.target.checked })} />{t('matching.caseSensitiveHint')}</label>}
      </fieldset>
    })}
  </section>
}

function Evidence({ entries, typeMap }) {
  const { t } = useTranslation()
  if (!entries?.length) return null
  return <ul className="matching-evidence">{entries.map((item, index) => <li key={`${item.id}:${index}`}>
    {t(`matching.kind.${item.source === 'exception' ? 'exceptions' : 'rules'}`)} · {t(`matching.field.${item.field}`, { defaultValue: item.field })}
    {' → '}<TypeLabel id={item.type_id} typeMap={typeMap} />
    {item.source === 'rule' && ` · ${t('matching.priority', { priority: item.priority })}`}
    <code>{JSON.stringify(item.text)}</code><small>{t('matching.entryId', { id: item.id })}</small>
  </li>)}</ul>
}

const COUNT_KEYS = ['total', 'protected_count', 'matched', 'unmatched', 'conflicts', 'input_too_long',
  'evaluation_limited', 'evaluation_errors', 'would_update', 'type_updated', 'unchanged', 'source_changed',
  'cas_skipped', 'deleted', 'rolled_back', 'not_attempted']

function Report({ report, typeMap, title }) {
  const { t } = useTranslation()
  if (!report) return null
  return <section className="matching-report" aria-label={title}>
    <h4>{title}</h4>
    {report.mode === 'sample' ? <>
      <p><strong>{t(`matching.status.${report.status}`, { defaultValue: report.status })}</strong>{' · '}
        <TypeLabel id={report.type_id} typeMap={typeMap} /> · {t('matching.sourceRevision', {
          source: t(`matching.source.${report.source}`), revision: report.revision })}</p>
      <p className="matching-reason">{report.reason}</p>
      <Evidence entries={report.structured_evidence} typeMap={typeMap} />
    </> : <>
      <dl className="matching-counts">{COUNT_KEYS.map(key =>
        <div key={key}><dt>{t(`matching.counts.${key}`)}</dt><dd>{report.summary[key]}</dd></div>)}</dl>
      <p className="matching-hint">{t('matching.reportHint', { revision: report.summary.revision })}</p>
      {report.error && <p role="alert">{report.error}</p>}
      {report.details.map((item, index) => <details key={`${item.symbol_id}:${index}`}>
        <summary>{item.name || t('matching.unnamedSymbol')} · {t(`matching.status.${item.status}`, { defaultValue: item.status })}
          {item.is_protected && ` · ${t('matching.counts.protected_count')}`} · <TypeLabel id={item.type_id || item.candidate_type_id} typeMap={typeMap} />
          {item.write_status && ` · ${t(`matching.writeStatus.${item.write_status}`, { defaultValue: item.write_status })}`}</summary>
        <p className="matching-reason">{item.reason}</p>
        {item.write_reason && <p className="matching-reason">{item.write_reason}</p>}
        <dl className="matching-raw">{Object.entries(item.sample || {}).map(([key, value]) =>
          <div key={key}><dt>{t(`matching.field.${key}`, { defaultValue: key })}</dt><dd><code>{JSON.stringify(value)}</code></dd></div>)}</dl>
        <Evidence entries={item.structured_evidence} typeMap={typeMap} />
      </details>)}
    </>}
  </section>
}

// 兼容已有调用方的中文来源名；新调用方可传稳定 ID。
const SOURCE_KEYS = { 元件: 'symbols', 统计: 'status', 元件库: 'libraries', 问题: 'issues', 类型: 'types', 界面: 'interface' }

export default function TypeMatchingModal({ open, onClose, types, typeMap, onRefreshCommitted, inline = false }) {
  const { t } = useTranslation()
  const model = useTypeMatching({ open, typeMap, onRefreshCommitted })
  if (!open) return null
  const frozen = !!model.busy
  const close = () => { if (model.canClose()) onClose() }
  const retryConfig = model.barrier === 'save'
  const Frame = inline ? CenterPage : Modal
  return <Frame title={t('matching.title')} className={inline ? 'matching-page matching-modal' : 'matching-modal'} onClose={close}>
    <p className="matching-scope">{t('matching.scope')}</p>
    <p className="matching-hint">{t('matching.orderHint')}</p>
    <p className="matching-hint">{t('matching.engineHint', {
      pattern: model.limits?.max_pattern_bytes ?? 512, source: model.limits?.max_source_field_bytes ?? 4096 })}</p>
    <div className="matching-toolbar"><span>{t('matching.savedRevision', { revision: model.saved?.revision ?? t('matching.unread') })}{model.dirty ? ` · ${t('matching.dirty')}` : ''}</span>
      <button className="btn-ghost" disabled={frozen || model.loading}
        onClick={retryConfig ? model.retryRead : model.reload}>{t(retryConfig ? 'matching.retryRead' : 'matching.reload')}</button>
      <button className="btn-ghost" disabled={frozen || !model.dirty} onClick={model.discard}>{t('matching.discard')}</button></div>
    {model.loading && <p role="status">{t('matching.loading')}</p>}
    {(model.error || model.validation) && <p role="alert" className="matching-alert">{model.error || model.validation}</p>}
    {model.notice && <p role="status" className="matching-notice">{model.notice}</p>}
    {model.barrier === 'rejected' && <p className="matching-alert">{t('matching.barrier.rejected')}</p>}
    {model.barrier === 'save' && !model.busy && <p className="matching-notice">{t('matching.barrier.save')}</p>}
    {model.barrier === 'applied' && !model.busy && <p className="matching-notice">{t('matching.barrier.applied')}</p>}
    {!model.healthy && <p className="matching-hint">{t('matching.unhealthy')}</p>}
    {model.draft && <>
      <EntryList kind="rules" entries={model.draft.rules} types={types} edit={model.edit} disabled={frozen} />
      <EntryList kind="exceptions" entries={model.draft.exceptions} types={types} edit={model.edit} disabled={frozen} />
      <section className="matching-section" aria-label={t('matching.manualSample')}>
        <h4>{t('matching.manualSample')}</h4><p className="matching-hint">{t('matching.sampleHint')}</p>
        <div className="matching-sample-grid">{MATCH_FIELDS.map(field => <div key={field}>
          <label className="matching-check"><input type="checkbox" aria-label={t('matching.provideField', { field: t(`matching.field.${field}`) })}
            checked={model.sample[field].provided} disabled={frozen}
            onChange={event => model.setSample(field, { provided: event.target.checked })} />{t('matching.provideField', { field: t(`matching.field.${field}`) })}</label>
          <input aria-label={t('matching.sampleField', { field: t(`matching.field.${field}`) })} value={model.sample[field].text}
            disabled={frozen || !model.sample[field].provided} placeholder={t(model.sample[field].provided ? 'matching.explicitEmpty' : 'matching.notProvided')}
            onChange={event => model.setSample(field, { text: event.target.value })} />
        </div>)}</div>
      </section>
    </>}
    <div className="matching-actions">
      <button className="btn-ghost" disabled={frozen || model.loading || !model.draft || !!model.validation} onClick={model.previewSample}>{t('matching.previewSample')}</button>
      <button className="btn-ghost" disabled={frozen || model.loading || !model.healthy || !model.draft || !!model.validation} onClick={model.previewDatabase}>{t('matching.previewDatabase')}</button>
      <button className="btn-primary" disabled={!model.canSave} onClick={model.save}>{t(model.busy === 'save' ? 'matching.saving' : 'matching.save')}</button>
      <button className="btn-primary" disabled={!model.canApply} onClick={model.apply}>{t(model.busy === 'apply' ? 'matching.applying' : 'matching.apply')}</button>
    </div>
    <p className="matching-hint">{t('matching.actionHint')}</p>
    {model.previewing && <p role="status">{t('matching.previewing')}</p>}
    <Report report={model.preview} typeMap={typeMap} title={t('matching.previewResult')} />
    <Report report={model.report} typeMap={typeMap} title={t('matching.applyResult')} />
    {model.refresh && <div className="matching-notice" role="status">
      <span>{t(model.refresh.status === 'published' ? 'matching.refresh.published' : model.refresh.status === 'pending' ? 'matching.refresh.pending' : 'matching.refresh.required')}</span>
      {model.refresh.details?.filter(item => item.status !== 'published').map(item => <p key={item.source}>{t('classifying.refreshDetail', {
        source: t(item.source.startsWith('classifying.sources.') ? item.source : `classifying.sources.${SOURCE_KEYS[item.source] || item.source}`, { defaultValue: item.source }),
        reason: item.status === 'superseded' ? t('matching.refresh.superseded')
          : translateMatchingMessage(item.errorMessage || item.error, t) || t('matching.refresh.unpublished') })}</p>)}
      {model.refresh.status !== 'published' && model.refresh.status !== 'pending' && <button className="btn-ghost" disabled={frozen}
        onClick={model.refreshCommitted}>{t('matching.refresh.retry')}</button>}
    </div>}
  </Frame>
}
