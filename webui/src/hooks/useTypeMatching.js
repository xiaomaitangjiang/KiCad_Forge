import { useCallback, useEffect, useRef, useState } from 'react'
import { useTranslation } from 'react-i18next'
import { api } from '../api'
import { configFrom, emptySample, isMatchingReport, isMatchingSamplePreview, parseMatchingRead, sameMatchingContent,
  sampleBody, validateMatchingDraft, matchingError, matchingErrorMessage, matchingMessage as message, translateMatchingMessage } from '../lib/typeMatching'

const initial = () => ({ saved: null, draft: null, healthy: false, loading: false, busy: '',
  barrier: '', error: '', notice: '', preview: null, previewing: false, report: null,
  refresh: null, sample: emptySample() })

// 稳定挂载的独立编辑会话。写屏障跨关闭保留，旧读/预览按 token 丢弃。
export function useTypeMatching({ open, typeMap, onRefreshCommitted }) {
  const { t } = useTranslation()
  const [state, setState] = useState(initial)
  const current = useRef(state)
  const mounted = useRef(true)
  const opened = useRef(open)
  opened.current = open
  const readId = useRef(0)
  const previewId = useRef(0)
  const editId = useRef(0)
  const readAbort = useRef(null)
  const previewAbort = useRef(null)
  const writeLock = useRef(false)
  const saveReceipt = useRef(null)
  const refreshLock = useRef(false)
  const refreshCallback = useRef(onRefreshCommitted)
  refreshCallback.current = onRefreshCommitted
  const map = useRef(typeMap)
  map.current = typeMap

  const update = useCallback(patch => {
    if (!mounted.current) return
    current.current = { ...current.current, ...patch }
    setState(current.current)
  }, [])

  const invalidatePreview = useCallback(() => {
    previewId.current += 1
    previewAbort.current?.abort()
    update({ preview: null, previewing: false })
  }, [update])

  const read = useCallback(async ({ explicit = false, accepted = null, minRevision } = {}) => {
    if (writeLock.current && !accepted) return { status: 'superseded' }
    const id = ++readId.current
    readAbort.current?.abort()
    const controller = new AbortController()
    readAbort.current = controller
    const atStart = current.current
    const confirmed = accepted || (atStart.barrier === 'save' ? saveReceipt.current?.config : null)
    const requiredRevision = minRevision ?? (atStart.barrier === 'save' ? saveReceipt.current?.revision : undefined)
    const dirty = atStart.draft && !sameMatchingContent(atStart.draft, atStart.saved)
    const edits = editId.current
    update({ loading: true, error: '' })
    const valid = () => mounted.current && opened.current && id === readId.current
    try {
      const raw = await (explicit ? api.reloadTypeMatching(controller.signal) : api.typeMatching(controller.signal))
      if (!valid()) return { status: 'superseded' }
      const result = parseMatchingRead(raw)
      if (result.healthy && requiredRevision !== undefined && result.config.revision < requiredRevision) {
        throw matchingError('savedVersionUnread')
      }
      const latest = current.current
      let draft = latest.draft
      const canAdopt = editId.current === edits && (!dirty || (confirmed && sameMatchingContent(draft, confirmed)))
      if (result.config) {
        draft = canAdopt || !draft ? result.config : { ...draft, revision: result.config.revision }
      }
      // 读取会采用/重订草稿版本；旧配置发出的预览不能留在新草稿下面。
      if (draft !== latest.draft) invalidatePreview()
      // GET 不能证明未知 POST 没有提交；必须由用户显式重新加载恢复。
      const canRecover = latest.barrier === 'save'
        || (explicit && latest.barrier !== 'apply')
      const changedUnderDraft = result.config && latest.saved && latest.draft
        && !sameMatchingContent(latest.draft, latest.saved)
        && latest.saved.revision !== result.config.revision && !confirmed && !explicit
      const barrier = changedUnderDraft ? 'stale' : result.healthy && canRecover ? '' : latest.barrier
      update({ saved: result.config || latest.saved, draft, healthy: result.healthy,
        error: result.errorMessage || result.error, barrier, engine: result.engine, limits: result.limits,
        notice: changedUnderDraft ? message('notices.changedUnderDraft')
          : result.healthy && latest.barrier === 'save' ? message('notices.saved')
          : result.healthy && explicit ? (dirty ? message('notices.reloadedDraft') : message('notices.reloaded')) : latest.notice })
      if (result.healthy && latest.barrier === 'save' && barrier === '') saveReceipt.current = null
      return { status: result.healthy ? 'published' : 'failed' }
    } catch (cause) {
      if (!valid()) return { status: 'superseded' }
      update({ error: matchingErrorMessage(cause), healthy: false })
      return { status: 'failed', error: cause }
    } finally { if (valid()) update({ loading: false }) }
  }, [invalidatePreview, update])

  const edit = useCallback((kind, change) => {
    if (writeLock.current || refreshLock.current || !current.current.draft) return
    const draft = current.current.draft
    editId.current += 1
    invalidatePreview()
    update({ draft: { ...draft, [kind]: change(draft[kind]) }, notice: '',
      barrier: current.current.barrier === 'applied' ? '' : current.current.barrier })
  }, [invalidatePreview, update])

  const setSample = useCallback((field, patch) => {
    const sample = current.current.sample
    invalidatePreview()
    update({ sample: { ...sample, [field]: { ...sample[field], ...patch } } })
  }, [invalidatePreview, update])

  const discard = useCallback(() => {
    if (writeLock.current || refreshLock.current || !current.current.saved) return
    editId.current += 1
    invalidatePreview()
    update({ draft: current.current.saved, notice: message('notices.discarded') })
  }, [invalidatePreview, update])

  const preview = useCallback(async mode => {
    const s = current.current
    if (writeLock.current || refreshLock.current || s.loading || !s.draft || (mode === 'database' && !s.healthy)) return
    const error = validateMatchingDraft(s.draft, map.current, true)
    if (error) { update({ error }); return }
    const id = ++previewId.current
    previewAbort.current?.abort()
    const controller = new AbortController()
    previewAbort.current = controller
    const body = { config: configFrom(s.draft), ...(mode === 'sample' ? { sample: sampleBody(s.sample) } : {}) }
    update({ previewing: true, error: '', preview: null })
    const valid = () => mounted.current && opened.current && id === previewId.current
    try {
      const response = await api.previewTypeMatching(body, controller.signal)
      if (!valid()) return
      if (!response?.ok) throw (response?.error ? new Error(response.error) : matchingError('previewFailed'))
      if (mode === 'sample' ? !isMatchingSamplePreview(response) : !isMatchingReport(response)) throw matchingError('invalidPreview')
      update({ preview: response })
    } catch (cause) { if (valid()) update({ error: matchingErrorMessage(cause) }) }
    finally { if (valid()) update({ previewing: false }) }
  }, [update])

  const refreshCommitted = useCallback(async () => {
    if (refreshLock.current) return
    refreshLock.current = true
    update({ busy: 'refresh', barrier: 'apply', refresh: { status: 'pending' } })
    try {
      const result = await refreshCallback.current()
      if (!mounted.current) return
      const published = result?.status === 'published'
      update({ refresh: result?.status ? result : { status: 'failed' },
        barrier: published ? 'applied' : 'apply', notice: published
          ? message('notices.synchronized')
          : message('notices.refreshRequired') })
    } catch (cause) {
      update({ refresh: { status: 'failed', details: [{ source: 'interface', error: matchingErrorMessage(cause) }] },
        notice: message('notices.refreshFailed') })
    } finally {
      refreshLock.current = false
      update({ busy: '' })
    }
  }, [update])

  const save = useCallback(async () => {
    const s = current.current
    if (writeLock.current || refreshLock.current || s.loading || !s.healthy || s.barrier || !s.saved
        || sameMatchingContent(s.draft, s.saved)) return
    const error = validateMatchingDraft(s.draft, map.current, true)
    if (error) { update({ error }); return }
    writeLock.current = true
    readId.current += 1
    invalidatePreview()
    const config = configFrom({ ...s.draft, revision: s.saved.revision })
    update({ busy: 'save', error: '', notice: '' })
    try {
      const response = await api.saveTypeMatching({ config, expected_revision: s.saved.revision })
      if (!mounted.current) return
      if (response?.ok === false) {
        // 服务端明确未提交，仍要求显式 reload 消除 CAS/健康状态不明。
        update({ error: response.error || message('errors.saveRejected'), barrier: 'rejected' })
        return
      }
      if (response?.ok !== true || !Number.isSafeInteger(response.revision) || response.revision <= s.saved.revision) {
        throw matchingError('invalidSave')
      }
      saveReceipt.current = { config, revision: response.revision }
      update({ barrier: 'save', notice: message('notices.savingRead', { revision: response.revision }) })
      await read({ accepted: config, minRevision: response.revision })
    } catch (cause) {
      if (!mounted.current) return
      update({ barrier: 'unknown', error: matchingErrorMessage(cause),
        notice: message('notices.saveUnknown') })
    } finally { writeLock.current = false; update({ busy: '' }) }
  }, [invalidatePreview, read, update])

  const apply = useCallback(async () => {
    const s = current.current
    if (writeLock.current || refreshLock.current || s.loading || !s.healthy || s.barrier || !s.saved
        || !sameMatchingContent(s.draft, s.saved) || validateMatchingDraft(s.draft, map.current, true)) return
    writeLock.current = true
    readId.current += 1
    invalidatePreview()
    update({ busy: 'apply', error: '', notice: '', report: null, refresh: null })
    try {
      let response
      try { response = await api.applyTypeMatching({ expected_revision: s.saved.revision }) }
      catch (cause) {
        if (isMatchingReport(cause.payload)) response = cause.payload
        else throw cause
      }
      if (!mounted.current) return
      if (!isMatchingReport(response)) {
        if (response?.ok === false && typeof response.error === 'string') {
          update({ error: response.error, barrier: 'rejected' })
          return
        }
        throw matchingError('invalidApply')
      }
      // 包括部分提交：先落屏障和实际计数，再等待全部读取。
      update({ report: response, barrier: 'apply', error: response.ok ? '' : response.error || message('errors.partialApply'),
        notice: message('notices.appliedRefreshing', { count: response.summary.type_updated }) })
      await refreshCommitted()
    } catch (cause) {
      update({ barrier: 'unknown', error: matchingErrorMessage(cause),
        notice: message('notices.applyUnknown') })
    } finally { writeLock.current = false; update({ busy: '' }) }
  }, [invalidatePreview, refreshCommitted, update])

  // 显式 reload 是恢复动作；保留合法草稿，不借 GET 暗中重试写请求。
  const reload = useCallback(async () => {
    if (writeLock.current || refreshLock.current) return
    invalidatePreview()
    const wasUnknown = current.current.barrier === 'unknown'
    const result = await read({ explicit: true })
    if (wasUnknown && result.status === 'published') {
      await refreshCommitted()
      // 先保持 applied 屏障，让核对后的用户通过下一次显式 reload 开启新操作。
      if (current.current.barrier === 'applied') {
        update({ notice: message('notices.recovered') })
      }
    }
  }, [invalidatePreview, read, refreshCommitted, update])

  useEffect(() => {
    mounted.current = true
    return () => {
      mounted.current = false
      readId.current += 1; previewId.current += 1
      readAbort.current?.abort(); previewAbort.current?.abort()
    }
  }, [])

  useEffect(() => {
    if (open) {
      update({ sample: emptySample(), preview: null, previewing: false })
      read()
    } else {
      readId.current += 1; previewId.current += 1
      readAbort.current?.abort(); previewAbort.current?.abort()
      update({ loading: false, previewing: false, sample: emptySample(), preview: null })
    }
  }, [open, read, update])

  const dirty = !!state.draft && !sameMatchingContent(state.draft, state.saved)
  const validation = state.draft ? validateMatchingDraft(state.draft, typeMap, true) : ''
  return { ...state, error: translateMatchingMessage(state.error, t), notice: translateMatchingMessage(state.notice, t),
    dirty, validation: translateMatchingMessage(validation, t), edit, setSample, discard, previewSample: () => preview('sample'),
    previewDatabase: () => preview('database'), save, apply, reload, retryRead: () => read(), refreshCommitted,
    canSave: state.healthy && dirty && !state.loading && !state.busy && !state.barrier && !validation,
    canApply: state.healthy && !!state.saved && !dirty && !state.loading && !state.busy && !state.barrier && !validation,
    canClose: () => !writeLock.current && !refreshLock.current }
}
