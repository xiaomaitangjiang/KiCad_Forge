import {
  normalizeLcscId, createRequestGuard, pluginOutcome, parseAnalyzeResult,
  validateTargetLibrary, targetSnapshot, sameTarget,
  candidateIdentity, candidateSelectable, candidateChoice, sanitizeCandidates,
  buildImportBody, newOperationId, refreshOutcome,
  parseSymbolLibraries, validateSymbolLibraryChoice,
} from './lcscImport.js'
import { createSnapshotStore } from '@kiforge/plugin-sdk'

// The only business-state owner for both the inline plugin and standalone view.
// Existing parsing, target validation, operation identity and task transport are
// reused; no React state, independent polling or result cache lives here.
export function createLcscModel(initialOptions) {
  let options = { ...initialOptions }
  const initial = {
    step: 'lookup',
    partNumber: '',
    inputError: '',
    looking: false,
    lookupError: '',
    device: null,
    targetId: '',
    target: null,
    symbolCatalog: null,
    symbolLoading: false,
    symbolError: '',
    symbolChoice: null,
    newSymbolName: '',
    choice: null,
    choiceFrom: '',
    manualQuery: '',
    manualResults: null,
    manualLoading: false,
    manualError: '',
    manualDropped: 0,
    importError: '',
    busy: false,
    completed: false,
    refreshing: false,
    refreshError: '',
    refreshSucceeded: false,
    frozen: false,
  }
  const store = (options.createStore || createSnapshotStore)({ ...initial,
    compLibraries: options.compLibraries || [], task: null, toast: '', pluginAction: options.pluginAction })
  let disposed = false, active = true, viewEpoch = 0, started = false
  let intentOperationId = null, pendingLookup = null, taskHandle = null, taskOff = null
  const lookupGuard = createRequestGuard(), manualGuard = createRequestGuard(), symbolGuard = createRequestGuard()
  const t = (...args) => options.t ? options.t(...args) : args[0]
  const patch = values => { if (!disposed) store.update(previous =>
    Object.entries(values).every(([key, value]) => Object.is(previous[key], value))
      ? previous : { ...previous, ...values }) }
  const { setStep, setPartNumber, setInputError, setLooking, setLookupError, setDevice, setTargetId, setTarget, setSymbolCatalog, setSymbolLoading, setSymbolError, setSymbolChoice, setNewSymbolName, setChoice, setChoiceFrom, setManualQuery, setManualResults, setManualLoading, setManualError, setManualDropped, setImportError, setBusy, setCompleted, setRefreshing, setRefreshError, setFrozen } = Object.fromEntries(Object.keys(initial).map(key => [
    `set${key[0].toUpperCase()}${key.slice(1)}`, value => patch({ [key]: value }),
  ]))
  let lastRaw, snapshot
  function getSnapshot() {
    const raw = store.getSnapshot()
    if (raw === lastRaw) return snapshot
    lastRaw = raw
    const { compLibraries, targetId, target, symbolChoice, symbolCatalog, newSymbolName, choice, manualResults, device, busy, refreshing, completed } = raw
    const targetOptions = (Array.isArray(compLibraries) ? compLibraries : []).map(lib => ({ lib, check: validateTargetLibrary(lib) }))
    const selectableTargets = targetOptions.filter(o => o.check.ok)
    const selectedTarget = targetOptions.find(o => o.lib.id === targetId) || null
    const draftSymbolChoice = symbolChoice?.mode === 'new' ? { mode: 'new', name: newSymbolName } : symbolChoice
    const symbolCheck = validateSymbolLibraryChoice(draftSymbolChoice, symbolCatalog)
    const manualChoice = choice?.mode === 'reuse' && manualResults ? manualResults.find(r => candidateIdentity(r) === choice.id) : null
    const deviceLabel = device ? device.symbolNames[0] || device.footprintName || device.id : ''
    const liveTarget = target ? (Array.isArray(compLibraries) ? compLibraries : []).find(l => l.id === target.id) || null : null
    const liveTargetCheck = target ? validateTargetLibrary(liveTarget) : { ok: false, reason: 'missing' }
    snapshot = { ...raw, targetOptions, selectableTargets, selectedTarget, selectedTargetUsable: !!selectedTarget?.check.ok,
      draftSymbolChoice, symbolCheck, manualChoice, deviceLabel, liveTargetCheck,
      canImport: !!device && !!target && !!symbolChoice && !!choice && !busy && !refreshing && !completed && liveTargetCheck.ok,
      frozenEdits: busy || refreshing || completed,
      canBeginNewIntent: completed && raw.refreshSucceeded && !busy && !refreshing }
    return snapshot
  }
  const isCurrent = epoch => !disposed && active && epoch === viewEpoch
  const isFrozen = () => disposed || !active || getSnapshot().frozen
  function invalidateReads() { lookupGuard.invalidate(); manualGuard.invalidate(); symbolGuard.invalidate(); pendingLookup = null }
  function requestClose() {
    if (disposed || getSnapshot().busy || getSnapshot().refreshing) return false
    invalidateReads()
    options.onClose?.()
    return true
  }
  async function loadCompLibraries() {
    const ownEpoch = viewEpoch
    try {
      const items = await (options.loadCompLibraries ? options.loadCompLibraries() : options.api.compLibraries())
      if (!isCurrent(ownEpoch)) return null
      if (Array.isArray(items) && items !== getSnapshot().compLibraries) patch({ compLibraries: items })
      return items
    } catch (error) {
      if (isCurrent(ownEpoch)) patch({ toast: error?.message || String(error) })
      return null
    }
  }
  function releaseTask() {
    taskOff?.(); taskOff = null; taskHandle = null
  }
  function watchTask(handle, ownsTask) {
    if (disposed || !ownsTask()) return
    releaseTask(); taskHandle = handle
    const update = () => {
      if (disposed || taskHandle !== handle || !ownsTask()) return
      const current = handle.getSnapshot()
      if (!current?.state) return
      patch({ task: current })
      if (['succeeded', 'failed', 'cancelled'].includes(current.state)) releaseTask()
    }
    const off = handle.subscribe(update)
    if (disposed || taskHandle !== handle || !ownsTask()) off()
    else { taskOff = off; update() }
    options.onTask?.(handle)
  }
  async function cancelTask() {
    if (disposed || !taskHandle || !getSnapshot().task?.cancelable) return
    try { await taskHandle.cancel() } catch (error) { if (!disposed) patch({ toast: error?.message || String(error) }) }
  }
  function clearChoice() { setChoice(null); setChoiceFrom('') }

  function clearFootprintIntent() {
    manualGuard.invalidate()
    clearChoice()
    setManualQuery(''); setManualResults(null); setManualLoading(false)
    setManualError(''); setManualDropped(0)
    setImportError('')
    intentOperationId = null
  }

  function clearSymbolSelection() {
    symbolGuard.invalidate()
    setSymbolCatalog(null); setSymbolLoading(false); setSymbolError('')
    setSymbolChoice(null); setNewSymbolName('')
    clearFootprintIntent()
  }

  function clearDerived() {
    releaseTask()
    patch({ task: null })
    clearSymbolSelection()
    manualGuard.invalidate()
    setManualQuery(''); setManualResults(null); setManualLoading(false)
    setManualError(''); setManualDropped(0)
    setDevice(null); setLookupError('')
    setTargetId(''); setTarget(null)
    clearChoice()
    setImportError('')
    intentOperationId = null
  }

  function changePartNumber(value) {
    if (isFrozen()) return
    setPartNumber(value)
    setInputError('')
    lookupGuard.invalidate()   // 编号一变，在途查找与 finally 全部作废
    pendingLookup = null   // 改了编号就允许（也必须）重新查找
    setLooking(false)
    clearDerived()
    setStep('lookup')
  }

  async function runLookup() {
    const { partNumber } = getSnapshot()
    const { api } = options
    const { plugin, action } = options.pluginAction || {}
    if (isFrozen()) return
    const normalized = normalizeLcscId(partNumber)
    if (!normalized.ok) {
      setInputError(t(normalized.reason === 'empty' ? 'lcsc.errorEmptyId' : 'lcsc.errorBadId'))
      return
    }
    // 按钮的 disabled 要等重渲染才生效：同一个编号的重复 Enter/双击在这里同步挡掉
    const pending = pendingLookup
    if (pending && lookupGuard.isCurrent(pending.token) && pending.value === normalized.value) return
    const token = lookupGuard.begin()
    const ownEpoch = viewEpoch
    const ownsTask = () => isCurrent(ownEpoch) && lookupGuard.isCurrent(token)
    pendingLookup = { token, value: normalized.value }
    clearDerived()
    setInputError('')
    setLooking(true)
    try {
      const wrapper = await api.executePlugin(plugin.id, {
        action: action?.analyze_action || 'analyze',
        lcsc_id: normalized.value,
      }, handle => { if (ownsTask()) watchTask(handle, ownsTask) })
      if (!lookupGuard.isCurrent(token)) return
      const parsed = parseAnalyzeResult(wrapper)
      if (!parsed.ok) { setLookupError(parsed.error); return }
      setDevice(parsed.data)
      setStep('target')
      loadCompLibraries?.()   // 进入目标库步取最新列表（失败不影响已完成的查找）
    } catch (cause) {
      if (!lookupGuard.isCurrent(token)) return
      setLookupError(cause?.message || String(cause))
    } finally {
      if (lookupGuard.isCurrent(token)) setLooking(false)
      // 只由当前请求身份清防重标记：同编号 ≠ 同请求。旧 A1 晚返回时 A2 仍在飞，
      // 按编号比较会把 A2 的防重保护清掉，下一次 Enter 就会重复发请求。
      if (pendingLookup?.token === token) pendingLookup = null
    }
  }

  function selectTarget(lib) {
    const { targetId } = getSnapshot()
    if (isFrozen()) return
    if (targetId === lib.id) return
    clearSymbolSelection()
    setTargetId(lib.id)
    clearChoice()               // 换目标 → 旧的封装确认不再适用
    setImportError('')
    intentOperationId = null
  }

  function confirmTarget() {
    const { selectableTargets, targetId } = getSnapshot()
    if (isFrozen()) return
    const picked = selectableTargets.find(o => o.lib.id === targetId)
    if (!picked) return
    setTarget(targetSnapshot(picked.lib))
    setImportError('')
    intentOperationId = null
    setStep('symbol')
    loadSymbolLibraries(picked.lib.id)
  }

  function backToTargets() {
    if (isFrozen()) return
    clearSymbolSelection()
    setStep('target')
  }

  async function loadSymbolLibraries(id) {
    const { api } = options
    const { target } = getSnapshot()
    if (id === undefined) id = target?.id
    if (isFrozen() || !id) return
    const token = symbolGuard.begin()
    setSymbolLoading(true); setSymbolError('')
    // 读取期间保留用户选择，阻断下一步；读取失败不能沿用旧列表。
    setSymbolCatalog(null)
    try {
      const parsed = parseSymbolLibraries(await api.componentSymbolLibraries(id))
      if (!symbolGuard.isCurrent(token)) return
      if (!parsed.ok) { setSymbolError(t('lcsc.symbolReason.badResponse')); return }
      setSymbolCatalog(parsed)
    } catch (cause) {
      if (symbolGuard.isCurrent(token)) setSymbolError(cause?.message || String(cause))
    } finally {
      if (symbolGuard.isCurrent(token)) setSymbolLoading(false)
    }
  }

  function selectSymbolLibrary(next) {
    if (isFrozen()) return
    setSymbolChoice(next)
    clearFootprintIntent()
  }

  function confirmSymbolLibrary() {
    const { symbolLoading, symbolCheck } = getSnapshot()
    if (isFrozen() || symbolLoading || !symbolCheck.ok) return
    symbolGuard.invalidate()
    setSymbolChoice(symbolCheck.choice)
    if (symbolCheck.choice.mode === 'new') setNewSymbolName(symbolCheck.choice.name)
    clearFootprintIntent()
    setStep('footprint')
  }

  function backToSymbols() {
    if (isFrozen()) return
    clearFootprintIntent()
    setStep('symbol')
    loadSymbolLibraries()
  }

  function pickCandidate(candidate, source) {
    if (isFrozen()) return
    if (!candidateSelectable(candidate)) return
    const next = candidateChoice(candidate)
    if (!next) return
    setChoice(next)
    setChoiceFrom(source)
    setImportError('')
    intentOperationId = null
  }

  function pickNew() {
    if (isFrozen()) return
    setChoice({ mode: 'new' })
    setChoiceFrom('new')
    setImportError('')
    intentOperationId = null
  }

  async function runManualSearch(value) {
    const { api } = options
    if (isFrozen()) return
    setManualQuery(value)
    const token = manualGuard.begin()
    setManualLoading(true)
    setManualError('')
    try {
      const results = await api.searchFootprints(value, '')
      if (!manualGuard.isCurrent(token)) return
      const sanitized = sanitizeCandidates(results)
      // 损坏的响应（null / 非数组）要说出来，不能显示成“没有结果”
      if (!sanitized.ok) setManualError(t('lcsc.manualBadResponse'))
      setManualResults(sanitized.items)
      setManualDropped(sanitized.dropped)
    } catch (cause) {
      if (!manualGuard.isCurrent(token)) return
      setManualResults([])
      setManualDropped(0)
      setManualError(cause?.message || String(cause))
    } finally {
      if (manualGuard.isCurrent(token)) setManualLoading(false)
    }
  }

  function reasonText(reason) {
    return t(`lcsc.reason.${reason}`)
  }

  async function refreshAfterImport() {
    const { onRefresh, refreshSymbols, loadLibraries, loadStatus } = options
    const ownEpoch = viewEpoch
    if (!isCurrent(ownEpoch)) return false
    if (getSnapshot().refreshing) return false   // 同步防重：双击重试只刷新一次

    setFrozen(true)

    setRefreshing(true)
    setRefreshError('')
    patch({ refreshSucceeded: false })
    try {
      // Public commands resolve void only after the host has published all
      // required reads; legacy modal callers still provide three raw loaders.
      if (onRefresh) {
        await onRefresh()
        if (!isCurrent(ownEpoch)) return false
        patch({ refreshSucceeded: true })
        return true
      }
      const steps = [
        ['symbols', refreshSymbols],
        ['libraries', loadLibraries],
        ['status', loadStatus],
      ]
      for (const [kind, load] of steps) {
        const result = await load?.()
        if (!isCurrent(ownEpoch)) return false
        const check = refreshOutcome(kind, result)
        if (!check.ok) {
          setRefreshError(t(`lcsc.refreshReason.${check.reason}`))
          return false
        }
      }
      patch({ refreshSucceeded: true })
      return true
    } catch (cause) {
      if (!isCurrent(ownEpoch)) return false
      setRefreshError(cause?.message || String(cause))
      return false
    } finally {
      if (isCurrent(ownEpoch)) setRefreshing(false)
    }
  }

  async function runImport() {
    const { completed, device, target, symbolChoice, choice, partNumber } = getSnapshot()
    const { api } = options
  const toastMsg = options.toastMsg || (toast => patch({ toast }))
    const { plugin } = options.pluginAction || {}
    const ownEpoch = viewEpoch
    if (!isCurrent(ownEpoch)) return false
    if (getSnapshot().frozen || (getSnapshot().busy || getSnapshot().refreshing) || completed) return
    if (!device || !target || !symbolChoice || !choice) return
    // 同步冻结：从这一刻起到关闭为止，编辑/查找/返回/关闭全部拒绝（React 重渲染
    // 之前用户就能再点一次，只靠 disabled 挡不住）。
    setFrozen(true)

    setBusy(true)
    setImportError('')
    let submitted = false
    try {
      // 提交前刷新元件库列表：包可能已被删除/锁定/改路径。刷新失败（null）
      // 不能当成空库继续，也不能沿用缓存。
      const fresh = await loadCompLibraries?.()
      if (!isCurrent(ownEpoch)) return
      if (!Array.isArray(fresh)) { setImportError(t('lcsc.errorRefresh')); return }
      const freshLib = fresh.find(l => l.id === target.id)
      if (!freshLib) { setImportError(t('lcsc.errorTargetGone')); return }
      const check = validateTargetLibrary(freshLib)
      if (!check.ok) { setImportError(reasonText(check.reason)); return }
      const snapshot = targetSnapshot(freshLib)
      if (!sameTarget(target, snapshot)) {
        // 名称/路径/保护状态变了：更新显示，要求用户按新信息重新确认
        setTarget(snapshot)
        intentOperationId = null
        clearSymbolSelection()
        setStep('target')
        setImportError(t('lcsc.errorTargetChanged'))
        return
      }
      // 已发出的意图可能已在宿主成功写入，但返回途中断网。此时新建文件已经存在，
      // 仍须原样发送同一个 operation_id，由宿主优先重放已完成结果。
      const freshCatalog = parseSymbolLibraries(await api.componentSymbolLibraries(target.id))
      if (!isCurrent(ownEpoch)) return
      if (!freshCatalog.ok) { setImportError(t('lcsc.symbolReason.badResponse')); return }
      const symbolValidation = validateSymbolLibraryChoice(symbolChoice, freshCatalog,
        { replay: !!intentOperationId })
      if (!symbolValidation.ok) {
        setSymbolCatalog(freshCatalog)
        setImportError(t(`lcsc.symbolReason.${symbolValidation.reason}`))
        return
      }
      const operationId = intentOperationId || newOperationId()
      intentOperationId = operationId   // 失败后重试沿用同一个意图
      const body = buildImportBody({
        lcscId: device.id || normalizeLcscId(partNumber).value,
        targetComponentLibraryId: target.id,
        symbolLibraryChoice: symbolValidation.choice,
        choice,
        operationId,
      })
      const ownsTask = () => isCurrent(ownEpoch) && intentOperationId === operationId
      const wrapper = await api.executePlugin(plugin.id, body,
        handle => { if (ownsTask()) watchTask(handle, ownsTask) })
      if (!isCurrent(ownEpoch)) return
      const outcome = pluginOutcome(wrapper)
      if (!outcome.ok) { setImportError(outcome.error); return }
      submitted = true
      // 写入已经发生：先把意图标记为完成，再刷新。刷新失败也不能重发 import ——
      // completed 与 operation_id 都保留，重试只重试刷新。
      setCompleted(true)
      const out = outcome.data || {}
      toastMsg?.(t(out.mode === 'reuse' ? 'plugin.footprintReused' : 'plugin.footprintNew',
        { name: out.footprint_reference || out.reused_from || '' }))
    } catch (cause) {
      if (!isCurrent(ownEpoch)) return false
      setImportError(cause?.message || String(cause))
    } finally {
      if (isCurrent(ownEpoch)) {
        // A committed intent stays frozen even when its refresh fails.
        if (!submitted) setFrozen(false)
        setBusy(false)
      }
    }
    // 刷新完成之后才关闭；刷新失败时弹窗留着，并给出重试刷新的入口
    if (submitted && await refreshAfterImport()) options.onClose?.()
  }

  function beginNewIntent() {
    if (disposed || !active || !getSnapshot().canBeginNewIntent) return false
    viewEpoch++
    invalidateReads()
    releaseTask()
    intentOperationId = null
    patch({ ...initial, task: null, toast: '' })
    return true
  }

  function configure(next) {
    if (disposed) return
    options = { ...options, ...next }
    if (next.pluginAction !== undefined && next.pluginAction !== getSnapshot().pluginAction) patch({ pluginAction: next.pluginAction })
    if (next.compLibraries !== undefined && next.compLibraries !== getSnapshot().compLibraries) patch({ compLibraries: next.compLibraries })
  }
  function activate() { if (!disposed) active = true }
  function suspend() {
    if (disposed) return
    active = false; viewEpoch++; invalidateReads()
    patch({ looking: false, symbolLoading: false, manualLoading: false })
  }
  function start() {
    if (disposed) return Promise.resolve(null)
    activate()
    if (!started) { started = true; return loadCompLibraries() }
    return Promise.resolve(getSnapshot().compLibraries)
  }
  function dispose() {
    if (disposed) return
    active = false; viewEpoch++; invalidateReads(); releaseTask()
    options.api?.dispose?.(); disposed = true; store.dispose()
  }
  return { getSnapshot, subscribe: store.subscribe, configure, activate, suspend, start, dispose, requestClose, beginNewIntent,
    loadCompLibraries, cancelTask, clearChoice, clearFootprintIntent, clearSymbolSelection, clearDerived, changePartNumber, runLookup, selectTarget, confirmTarget, backToTargets, loadSymbolLibraries, selectSymbolLibrary, confirmSymbolLibrary, backToSymbols, pickCandidate, pickNew, runManualSearch, reasonText, refreshAfterImport, runImport, setNewSymbolName, isFrozen }
}
