import type {
  ComponentLibrary, LibrariesContract, ModelApi, TaskHandle, TaskSnapshot, TasksContract,
} from '@kiforge/plugin-sdk'
import { translate } from './i18n-text.ts'

// ---------------------------------------------------------------------------
// Wire payloads (locked with backend-git, task-5)
// ---------------------------------------------------------------------------

export interface GitStatusFile { status: string; path: string }
export interface GitStatusResult {
  ok?: boolean; initialized?: boolean; root?: string; branch?: string
  last_commit?: string; changes?: string; files?: GitStatusFile[]; ahead?: string; error?: string
}

export type GitTab = 'changes' | 'history' | 'diff'
export type GitRange = 'all' | 'branch'
export type LoadPhase = 'idle' | 'loading' | 'ready' | 'empty' | 'error' | 'stale'
export type GitRefKind = 'local' | 'remote' | 'tag'

export interface GitRef {
  name: string
  full_name?: string
  kind: GitRefKind
  target_oid: string
  is_head?: boolean
}

export interface GitCommitSummary {
  oid: string
  parents: string[]
  subject: string
  body?: string
  author_name: string
  author_time: string
  committer_time: string
}

export interface GitHistoryPage {
  snapshot_id: string
  next_cursor: string | null
  has_more: boolean
}

export interface GitHistoryResult {
  ok?: boolean
  code?: string
  error?: string
  schema_version?: string
  repository_identity?: string
  head_oid?: string | null
  head_ref?: string | null
  detached?: boolean
  state?: string
  refs?: GitRef[]
  refs_truncated?: boolean
  commits?: GitCommitSummary[]
  page?: GitHistoryPage
  total?: number | null
  truncated?: boolean
}

export interface GitHistoryData {
  repositoryIdentity: string
  headOid: string
  headRef: string
  detached: boolean
  state: string
  refs: GitRef[]
  commits: GitCommitSummary[]
  page: GitHistoryPage | null
  total: number | null
  truncated: boolean
  refsTruncated: boolean
}

export interface GitCommitFile {
  path: string
  status?: string
  old_path?: string | null
  additions?: number | null
  deletions?: number | null
  binary?: boolean
}

export interface GitCommitDetailResult {
  ok?: boolean
  code?: string
  error?: string
  schema_version?: string
  commit?: GitCommitSummary
  files?: GitCommitFile[]
  files_truncated?: boolean
  body_truncated?: boolean
  diff_base?: string | null
}

/** Validated, complete commit detail held by the model. */
export interface GitCommitDetail {
  ok: true
  schema_version: string
  commit: GitCommitSummary
  files: GitCommitFile[]
  filesTruncated: boolean
  bodyTruncated: boolean
  diffBase: string | null
}

export interface GitCommitFileResult {
  ok?: boolean
  code?: string
  error?: string
  schema_version?: string
  oid?: string
  path?: string
  old_path?: string | null
  status?: string
  diff_base?: string | null
  patch?: string
  truncated?: boolean
  binary?: boolean
}

/** Validated file diff held by the model. */
export interface GitCommitFileView {
  oid: string
  path: string
  oldPath: string | null
  status: string
  diffBase: string | null
  patch: string
  truncated: boolean
  binary: boolean
}

/** One independently addressable read. Loading never blocks the write state. */
export interface GitResource<T> {
  phase: LoadPhase
  data: T | null
  error: string
}

export interface GitFileSelection { oid: string; path: string; parentIndex: number }

export interface GitSnapshot {
  libraries: ComponentLibrary[]
  target: string
  loadError: string
  message: string
  /** Latest commit/init action, kept after it terminates so its output stays visible. */
  task: TaskSnapshot | null
  busy: boolean
  running: boolean
  canCancel: boolean
  tab: GitTab
  status: GitResource<GitStatusResult>
  history: GitResource<GitHistoryData>
  diff: GitResource<string>
  detail: GitResource<GitCommitDetail>
  fileDiff: GitResource<GitCommitFileView>
  file: GitFileSelection | null
  parentIndex: number
  range: GitRange
  branchRef: string
  find: string
  selectedOid: string | null
  scrollTop: number
}

export const HISTORY_PAGE_SIZE = 50
export const HISTORY_MAX_COMMITS = 500
export const DETAIL_CACHE_LIMIT = 5

// ---------------------------------------------------------------------------
// Payload guards (strict on success shapes, permissive on error envelopes)
// ---------------------------------------------------------------------------

const isRecord = (value: unknown): value is Record<string, unknown> =>
  !!value && typeof value === 'object' && !Array.isArray(value)

function optionalString(data: Record<string, unknown>, key: string): boolean {
  return data[key] === undefined || typeof data[key] === 'string'
}
function optionalBoolean(data: Record<string, unknown>, key: string): boolean {
  return data[key] === undefined || typeof data[key] === 'boolean'
}

export function isStatusResult(value: unknown): value is GitStatusResult {
  if (!isRecord(value)) return false
  for (const key of ['root', 'branch', 'last_commit', 'changes', 'ahead', 'error']) {
    if (!optionalString(value, key)) return false
  }
  for (const key of ['ok', 'initialized']) {
    if (!optionalBoolean(value, key)) return false
  }
  return value.files === undefined || Array.isArray(value.files) && value.files.every((file) =>
    isRecord(file) && typeof file.status === 'string' && typeof file.path === 'string')
}

function isRef(value: unknown): value is GitRef {
  return isRecord(value)
    && typeof value.name === 'string'
    && (value.full_name === undefined || typeof value.full_name === 'string')
    && (value.kind === 'local' || value.kind === 'remote' || value.kind === 'tag')
    && typeof value.target_oid === 'string'
    && (value.is_head === undefined || typeof value.is_head === 'boolean')
}

function isCommit(value: unknown): value is GitCommitSummary {
  return isRecord(value)
    && typeof value.oid === 'string'
    && Array.isArray(value.parents) && value.parents.every((parent) => typeof parent === 'string')
    && typeof value.subject === 'string'
    && (value.body === undefined || typeof value.body === 'string')
    && typeof value.author_name === 'string'
    && typeof value.author_time === 'string'
    && typeof value.committer_time === 'string'
}

function isPage(value: unknown): value is GitHistoryPage {
  return isRecord(value)
    && typeof value.snapshot_id === 'string'
    && (value.next_cursor === null || typeof value.next_cursor === 'string')
    && typeof value.has_more === 'boolean'
}

function isHistory(value: unknown): value is GitHistoryResult {
  if (!isRecord(value)) return false
  if (value.ok === false) return true
  return value.ok === true
    && value.schema_version === 'graph_v1'
    && typeof value.repository_identity === 'string'
    && (value.head_oid === undefined || value.head_oid === null || typeof value.head_oid === 'string')
    && (value.head_ref === undefined || value.head_ref === null || typeof value.head_ref === 'string')
    && optionalBoolean(value, 'detached')
    && optionalString(value, 'state')
    && Array.isArray(value.refs) && value.refs.every(isRef)
    && optionalBoolean(value, 'refs_truncated')
    && Array.isArray(value.commits) && value.commits.every(isCommit)
    && isPage(value.page)
    && (value.total === undefined || value.total === null || typeof value.total === 'number')
    && optionalBoolean(value, 'truncated')
}

function isCommitFile(value: unknown): value is GitCommitFile {
  return isRecord(value)
    && typeof value.path === 'string'
    && optionalString(value, 'status')
    && (value.old_path === undefined || value.old_path === null || typeof value.old_path === 'string')
    && (value.additions === undefined || value.additions === null || typeof value.additions === 'number')
    && (value.deletions === undefined || value.deletions === null || typeof value.deletions === 'number')
    && (value.binary === undefined || typeof value.binary === 'boolean')
}

function isDetail(value: unknown): value is GitCommitDetailResult {
  if (!isRecord(value)) return false
  if (value.ok === false) return true
  return value.ok === true
    && isCommit(value.commit)
    && Array.isArray(value.files) && value.files.every(isCommitFile)
    && optionalBoolean(value, 'files_truncated')
    && optionalBoolean(value, 'body_truncated')
    && (value.diff_base === undefined || value.diff_base === null || typeof value.diff_base === 'string')
}

function isFilePatch(value: unknown): value is GitCommitFileResult {
  if (!isRecord(value)) return false
  if (value.ok === false) return true
  return value.ok === true
    && typeof value.oid === 'string'
    && typeof value.path === 'string'
    && (value.diff_base === undefined || value.diff_base === null || typeof value.diff_base === 'string')
    && typeof value.patch === 'string'
    && typeof value.truncated === 'boolean'
    && typeof value.binary === 'boolean'
}

interface DiffEnvelope { ok?: boolean; output?: string; error?: string }
function isDiff(value: unknown): value is DiffEnvelope {
  return isRecord(value)
    && optionalBoolean(value, 'ok')
    && optionalString(value, 'output')
    && optionalString(value, 'error')
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

const terminal = (task: TaskSnapshot) => ['succeeded', 'failed', 'cancelled'].includes(task.state)
const errorText = (error: unknown) => error instanceof Error ? error.message : String(error)
const emptyResource = <T>(): GitResource<T> => ({ phase: 'idle', data: null, error: '' })
const staleOrIdle = <T>(resource: GitResource<T>): GitResource<T> => ({
  ...resource, phase: resource.data ? 'stale' : 'idle', error: '',
})

type GitChannel = 'status' | 'history' | 'diff' | 'detail' | 'file'

interface ChannelRequest {
  channel: GitChannel
  generation: number
  target: string
  query: string
  handle: TaskHandle<unknown> | null
  off: (() => void) | null
  cancelRequested: boolean
  apply: (result: unknown) => boolean
  onSuccess?: () => void
}

interface ChannelOptions {
  query?: string
  apply: (result: unknown) => boolean
  onSuccess?: () => void
}

/**
 * Observable Git panel model.
 *
 * Writes (commit/init) and reads (status/history/diff/detail/file) have
 * separate lifecycles: a read can never hide or replace the write state, and a
 * write result only invalidates resources instead of replaying reads blind.
 * Every result is bound to the generation, target and query that requested it,
 * so late replies for an abandoned repository or filter are dropped.
 */
export function createGitModel({ createStore, librariesApi, tasks }: {
  createStore: ModelApi['createStore']
  librariesApi: LibrariesContract
  tasks: TasksContract
}) {
  const store = createStore<GitSnapshot>({
    libraries: [], target: '', loadError: '', message: '', task: null,
    busy: false, running: false, canCancel: false,
    tab: 'changes',
    status: emptyResource<GitStatusResult>(),
    history: emptyResource<GitHistoryData>(),
    diff: emptyResource<string>(),
    detail: emptyResource<GitCommitDetail>(),
    fileDiff: emptyResource<GitCommitFileView>(),
    file: null, parentIndex: 0,
    range: 'all', branchRef: '', find: '', selectedOid: null, scrollTop: 0,
  })
  let disposed = false
  let generation = 0
  let starting: Promise<void> | null = null
  let actionBusy = false
  let actionHandle: TaskHandle<unknown> | null = null
  let actionOff: (() => void) | null = null
  let submittedMessage: string | null = null
  const channels = new Map<GitChannel, ChannelRequest>()
  const detailCache = new Map<string, GitCommitDetail>()

  const snapshot = () => store.getSnapshot()
  const patch = (values: Partial<GitSnapshot>) => store.update((previous) => ({ ...previous, ...values }))

  function touch() {
    if (disposed) return
    let canCancel = false
    for (const request of channels.values()) {
      const task = request.handle?.getSnapshot()
      if (task && !terminal(task) && task.cancelable) {
        canCancel = true
        break
      }
    }
    patch({ running: actionBusy || channels.size > 0, canCancel })
  }

  function queryFor(channel: GitChannel): string {
    const current = snapshot()
    switch (channel) {
      case 'history': {
        const branch = current.branchRef || current.status.data?.branch || ''
        return `${current.range}|${current.range === 'branch' ? branch : current.branchRef}`
      }
      case 'detail':
        return `${current.selectedOid ?? ''}|${current.parentIndex}`
      case 'file':
        return current.file
          ? `${current.file.oid}|${current.file.path}|${current.file.parentIndex}`
          : ''
      default:
        return current.target
    }
  }

  function markLoading(channel: GitChannel) {
    const current = snapshot()
    switch (channel) {
      case 'status': patch({ status: { ...current.status, phase: 'loading', error: '' } }); break
      case 'history': patch({ history: { ...current.history, phase: 'loading', error: '' } }); break
      case 'diff': patch({ diff: { ...current.diff, phase: 'loading', error: '' } }); break
      case 'detail': patch({ detail: { ...current.detail, phase: 'loading', error: '' } }); break
      case 'file': patch({ fileDiff: { ...current.fileDiff, phase: 'loading', error: '' } }); break
    }
  }

  function markError(channel: GitChannel, message: string) {
    const current = snapshot()
    switch (channel) {
      case 'status': patch({ status: { ...current.status, phase: 'error', error: message } }); break
      case 'history': patch({ history: { ...current.history, phase: 'error', error: message } }); break
      case 'diff': patch({ diff: { ...current.diff, phase: 'error', error: message } }); break
      case 'detail': patch({ detail: { ...current.detail, phase: 'error', error: message } }); break
      case 'file': patch({ fileDiff: { ...current.fileDiff, phase: 'error', error: message } }); break
    }
  }

  async function cancelRequest(request: ChannelRequest) {
    const task = request.handle?.getSnapshot()
    if (!request.handle || !task || terminal(task) || !task.cancelable || request.cancelRequested) return
    request.cancelRequested = true
    try {
      await request.handle.cancel()
    } catch {
      // The terminal subscription still owns cleanup; a failed cancel is a hint, not state.
    }
  }

  function dropChannel(channel: GitChannel, cancel: boolean) {
    const request = channels.get(channel)
    if (!request) return
    channels.delete(channel)
    request.off?.()
    request.off = null
    if (cancel) void cancelRequest(request)
  }

  function finishRequest(request: ChannelRequest, result: unknown) {
    request.off?.()
    request.off = null
    channels.delete(request.channel)
    const current = snapshot()
    const applies = request.generation === generation
      && request.target === current.target
      && request.query === queryFor(request.channel)
    if (applies && request.apply(result)) request.onSuccess?.()
    touch()
  }

  function failRequest(request: ChannelRequest, message: string) {
    request.off?.()
    request.off = null
    channels.delete(request.channel)
    const current = snapshot()
    if (request.generation === generation && request.target === current.target
      && request.query === queryFor(request.channel)) {
      markError(request.channel, message)
    }
    touch()
  }

  function startChannel(channel: GitChannel, action: string, args: Record<string, unknown>, options: ChannelOptions) {
    const current = snapshot()
    if (disposed || !current.target) return
    dropChannel(channel, true)
    const request: ChannelRequest = {
      channel, generation, target: current.target, query: options.query ?? queryFor(channel),
      handle: null, off: null, cancelRequested: false,
      apply: options.apply, onSuccess: options.onSuccess,
    }
    channels.set(channel, request)
    markLoading(channel)
    touch()
    void (async () => {
      let handle: TaskHandle<unknown>
      try {
        handle = await tasks.run(action, args) as TaskHandle<unknown>
      } catch (error) {
        if (disposed || channels.get(channel) !== request) return
        failRequest(request, errorText(error))
        return
      }
      if (disposed) return
      if (channels.get(channel) !== request) {
        // Superseded while the host was creating the task (target/filter change):
        // release the backend read instead of leaving it running unobserved.
        try { await handle.cancel() } catch { /* best effort */ }
        return
      }
      request.handle = handle
      touch()
      const publish = () => {
        if (disposed || channels.get(channel) !== request) return
        const task = handle.getSnapshot()
        if (task.state === 'succeeded') finishRequest(request, task.result)
        else if (terminal(task)) {
          failRequest(request, task.error
            ?? (task.state === 'cancelled' ? translate('已取消', 'Cancelled') : translate('任务失败', 'Task failed')))
        } else {
          touch()
        }
      }
      const off = handle.subscribe(publish)
      if (channels.get(channel) !== request) {
        off()
        return
      }
      request.off = off
      publish()
      if (channels.get(channel) !== request) {
        off()
        if (request.off === off) request.off = null
      }
    })()
  }

  // -- reads ---------------------------------------------------------------

  function applyStatus(result: unknown): boolean {
    if (!isStatusResult(result) || result.ok === false) {
      markError('status', isStatusResult(result) && result.error ? result.error : 'Invalid Git status result')
      return false
    }
    patch({ status: { phase: 'ready', data: result, error: '' } })
    return true
  }

  function loadStatus(onSuccess?: () => void) {
    const current = snapshot()
    if (!current.target || channels.has('status')) return
    startChannel('status', 'status', { target_library: current.target }, { apply: applyStatus, onSuccess })
  }

  function loadActiveView(force = false) {
    const current = snapshot()
    if (!current.target) return
    if (current.tab === 'history'
      && (force || current.history.phase === 'idle' || current.history.phase === 'stale' || !current.history.data)) {
      loadHistory()
    } else if (current.tab === 'diff'
      && (force || current.diff.phase === 'idle' || current.diff.phase === 'stale' || current.diff.data === null)) {
      loadDiff()
    }
  }

  function loadHistory(append = false) {
    const current = snapshot()
    if (!current.target || channels.has('history')) return
    if (append) {
      if (channels.has('history')) return
      if (current.history.phase === 'stale' || current.history.phase === 'error') return
      const data = current.history.data
      if (!data || data.commits.length >= HISTORY_MAX_COMMITS) return
      if (!data.page?.has_more || !data.page.next_cursor) return
    }
    const range = current.range
    const branch = current.branchRef || current.status.data?.branch || ''
    const query = `${range}|${range === 'branch' ? branch : current.branchRef}`
    const args: Record<string, unknown> = {
      target_library: current.target,
      format: 'graph_v1',
      range,
      limit: HISTORY_PAGE_SIZE,
    }
    if (range === 'branch' && branch) args.branch_ref = branch
    const cursor = append ? current.history.data?.page?.next_cursor : null
    if (cursor) args.cursor = cursor
    startChannel('history', 'history', args, {
      query,
      apply: (result) => applyHistory(result, append),
    })
  }

  function applyHistory(result: unknown, append: boolean): boolean {
    if (!isHistory(result)) {
      markError('history', 'Invalid Git history result')
      return false
    }
    if (result.ok === false) {
      const message = result.error || translate('Git 历史读取失败', 'Failed to read Git history')
      if (append && (result.code === 'stale_cursor' || /stale[_-]?cursor/i.test(message))) {
        // The snapshot moved under the cursor. Keep the already loaded page and
        // mark it stale instead of silently mixing two snapshots; refresh is the
        // only way to continue paging.
        const current = snapshot()
        patch({
          history: {
            ...current.history,
            phase: 'stale',
            error: translate('历史快照已变化，请刷新后继续加载', 'The history snapshot changed; refresh before loading more'),
          },
        })
        return false
      }
      markError('history', message)
      return false
    }
    const commits = result.commits ?? []
    const previous = append ? snapshot().history.data : null
    let merged = commits
    if (previous) {
      const seen = new Set(previous.commits.map((commit) => commit.oid))
      merged = [...previous.commits, ...commits.filter((commit) => !seen.has(commit.oid))]
      if (merged.length > HISTORY_MAX_COMMITS) merged = merged.slice(0, HISTORY_MAX_COMMITS)
    }
    patch({
      history: {
        phase: merged.length ? 'ready' : 'empty',
        error: '',
        data: {
          repositoryIdentity: result.repository_identity ?? '',
          headOid: result.head_oid ?? '',
          headRef: result.head_ref ?? '',
          detached: result.detached === true,
          state: result.state ?? 'ready',
          refs: result.refs ?? [],
          commits: merged,
          page: result.page ?? null,
          total: typeof result.total === 'number' ? result.total : null,
          truncated: result.truncated === true,
          refsTruncated: result.refs_truncated === true,
        },
      },
    })
    return true
  }

  function loadDiff() {
    const current = snapshot()
    if (!current.target || channels.has('diff')) return
    startChannel('diff', 'diff', { target_library: current.target }, {
      query: current.target,
      apply: (result) => {
        if (!isDiff(result)) {
          markError('diff', 'Invalid Git diff result')
          return false
        }
        if (result.ok === false) {
          markError('diff', result.error || translate('Git 差异读取失败', 'Failed to read Git diff'))
          return false
        }
        const output = result.output ?? ''
        patch({ diff: { phase: output.trim() ? 'ready' : 'empty', data: output, error: '' } })
        return true
      },
    })
  }

  function cacheDetail(key: string, detail: GitCommitDetail) {
    detailCache.delete(key)
    detailCache.set(key, detail)
    while (detailCache.size > DETAIL_CACHE_LIMIT) {
      const oldest = detailCache.keys().next().value
      if (oldest === undefined) break
      detailCache.delete(oldest)
    }
  }

  function loadDetail(oid: string, parentIndex = 0) {
    if (!oid) return
    const key = `${oid}|${parentIndex}`
    const cached = detailCache.get(key)
    dropChannel('detail', true)
    if (cached) {
      patch({ detail: { phase: 'ready', data: cached, error: '' } })
      touch()
      return
    }
    const current = snapshot()
    if (!current.target) return
    const known = current.history.data?.commits.find((entry) => entry.oid === oid)
    const parents = known?.parents
      ?? (current.detail.data?.commit.oid === oid ? current.detail.data.commit.parents : [])
    const parent = parents[parentIndex]
    const args: Record<string, unknown> = { target_library: current.target, oid }
    // Root commits have no parent: the backend must compare against the empty
    // tree, so neither parent nor parent_index may be sent.
    if (parent) {
      args.parent = parent
      args.parent_index = parentIndex
    }
    startChannel('detail', 'commit_detail', args, {
      query: key,
      apply: (result) => {
        if (!isDetail(result)) {
          markError('detail', 'Invalid Git commit detail result')
          return false
        }
        if (result.ok === false) {
          markError('detail', result.error || translate('Git 提交详情读取失败', 'Failed to read Git commit details'))
          return false
        }
        const commit = result.commit
        if (!commit || commit.oid !== oid) {
          markError('detail', translate('Git 提交详情与所选提交不匹配', 'Git commit details do not match the selected commit'))
          return false
        }
        const detail: GitCommitDetail = {
          ok: true,
          schema_version: result.schema_version ?? 'commit_detail_v1',
          commit,
          files: result.files ?? [],
          filesTruncated: result.files_truncated === true,
          bodyTruncated: result.body_truncated === true,
          diffBase: result.diff_base ?? null,
        }
        cacheDetail(key, detail)
        patch({ detail: { phase: 'ready', data: detail, error: '' } })
        return true
      },
    })
  }

  function loadFile(oid: string, path: string, parentIndex: number) {
    const current = snapshot()
    if (!current.target || !oid) return
    const parent = current.detail.data?.commit.parents[parentIndex]
    const args: Record<string, unknown> = { target_library: current.target, oid, path }
    if (parent) {
      args.parent = parent
      args.parent_index = parentIndex
    }
    startChannel('file', 'commit_file', args, {
      query: `${oid}|${path}|${parentIndex}`,
      apply: (result) => {
        if (!isFilePatch(result)) {
          markError('file', 'Invalid Git file diff result')
          return false
        }
        if (result.ok === false) {
          markError('file', result.error || translate('Git 文件差异读取失败', 'Failed to read Git file diff'))
          return false
        }
        if (result.oid !== oid || result.path !== path) {
          markError('file', translate('Git 文件差异与所选文件不匹配', 'Git file diff does not match the selected file'))
          return false
        }
        const patchText = result.patch ?? ''
        const view: GitCommitFileView = {
          oid,
          path,
          oldPath: result.old_path ?? null,
          status: result.status ?? '',
          diffBase: result.diff_base ?? null,
          patch: patchText,
          truncated: result.truncated === true,
          binary: result.binary === true,
        }
        patch({
          fileDiff: {
            phase: view.binary || !view.patch.trim() ? 'empty' : 'ready',
            data: view,
            error: '',
          },
        })
        return true
      },
    })
  }

  // -- writes --------------------------------------------------------------

  function invalidateAfterWrite() {
    for (const channel of [...channels.keys()]) dropChannel(channel, true)
    detailCache.clear()
    const current = snapshot()
    patch({
      status: staleOrIdle(current.status),
      history: staleOrIdle(current.history),
      diff: staleOrIdle(current.diff),
      detail: emptyResource<GitCommitDetail>(),
      fileDiff: emptyResource<GitCommitFileView>(),
      file: null,
      selectedOid: null,
      parentIndex: 0,
    })
  }

  function finishAction(task: TaskSnapshot, generationAtStart: number, targetAtStart: string) {
    const message = submittedMessage
    actionOff?.()
    actionOff = null
    actionHandle = null
    actionBusy = false
    submittedMessage = null
    patch({ busy: false })
    touch()
    if (task.state !== 'succeeded' || (task.action !== 'commit' && task.action !== 'init')) {
      if (snapshot().target && snapshot().status.phase === 'idle') loadStatus(() => loadActiveView())
      return
    }
    const current = snapshot()
    if (generationAtStart !== generation || targetAtStart !== current.target) {
      // The write belonged to another repository. Never let it rewrite this one;
      // just make sure the newly selected target gets its own status read.
      if (current.target && current.status.phase === 'idle') loadStatus(() => loadActiveView())
      return
    }
    if (task.action === 'commit' && message !== null && snapshot().message === message) {
      patch({ message: '' })
    }
    invalidateAfterWrite()
    loadStatus(() => loadActiveView(true))
  }

  function runAction(action: 'commit' | 'init', extra: Record<string, unknown> = {}) {
    const current = snapshot()
    if (disposed || !current.target || actionBusy) return
    actionBusy = true
    submittedMessage = action === 'commit' ? current.message : null
    const generationAtStart = generation
    const targetAtStart = current.target
    patch({
      busy: true,
      task: { id: '', action, state: 'queued', cancelable: false },
    })
    touch()
    void tasks.run(action, { target_library: targetAtStart, ...extra }).then((handle) => {
      if (disposed || !actionBusy) return
      actionHandle = handle
      const publish = () => {
        if (disposed || actionHandle !== handle) return
        const task = handle.getSnapshot()
        patch({ task })
        if (terminal(task)) finishAction(task, generationAtStart, targetAtStart)
      }
      const off = handle.subscribe(publish)
      if (actionHandle !== handle) {
        off()
        return
      }
      actionOff = off
      publish()
      if (actionHandle !== handle) {
        off()
        if (actionOff === off) actionOff = null
      }
    }).catch((error) => {
      if (disposed || !actionBusy) return
      actionBusy = false
      submittedMessage = null
      patch({
        busy: false,
        task: { id: '', action, state: 'failed', cancelable: false, error: errorText(error) },
      })
      touch()
    })
  }

  // -- public surface ------------------------------------------------------

  function setTarget(target: string) {
    if (disposed || target === snapshot().target) return
    generation++
    for (const channel of [...channels.keys()]) dropChannel(channel, true)
    detailCache.clear()
    patch({
      target,
      task: null,
      busy: actionBusy,
      status: emptyResource<GitStatusResult>(),
      history: emptyResource<GitHistoryData>(),
      diff: emptyResource<string>(),
      detail: emptyResource<GitCommitDetail>(),
      fileDiff: emptyResource<GitCommitFileView>(),
      file: null,
      selectedOid: null, range: 'all', branchRef: '', find: '', scrollTop: 0, parentIndex: 0,
    })
    if (target) loadStatus(() => loadActiveView())
    else touch()
  }

  function start() {
    if (disposed) return Promise.resolve()
    if (!starting) {
      starting = Promise.resolve().then(() => librariesApi.componentLibraries()).then((items) => {
        if (disposed) return
        const all = items.filter((item) => !!(item.symbolPath || item.footprintPath))
        const writable = all.filter((item) => item.locked !== true && item.isProtected !== true)
        const libraries = (writable.length ? writable : all).slice(0, 100)
        patch({ libraries, loadError: '' })
        if (!snapshot().target && libraries[0]) setTarget(componentBase(libraries[0]))
      }).catch((error) => {
        starting = null
        if (!disposed) patch({ loadError: errorText(error) })
      })
    }
    return starting
  }

  function selectTab(tab: GitTab) {
    if (disposed) return
    patch({ tab })
    if (!snapshot().target) return
    if (tab === 'history') loadHistory()
    else if (tab === 'diff') loadDiff()
    else if (snapshot().status.phase === 'idle' || snapshot().status.phase === 'stale') loadStatus()
  }

  function setHistoryRange(range: GitRange, branchRef = '') {
    if (disposed) return
    const current = snapshot()
    if (range === current.range && branchRef === current.branchRef) return
    dropChannel('history', true)
    patch({
      range,
      branchRef: range === 'branch' ? branchRef : '',
      history: emptyResource<GitHistoryData>(),
      detail: emptyResource<GitCommitDetail>(),
      file: null,
      fileDiff: emptyResource<GitCommitFileView>(),
      selectedOid: null,
      scrollTop: 0,
    })
    dropChannel('detail', true)
    dropChannel('file', true)
    if (current.tab === 'history') loadHistory()
  }

  function setFind(find: string) {
    if (!disposed) patch({ find })
  }

  function setScrollTop(scrollTop: number) {
    if (disposed) return
    const value = Math.max(0, Math.round(scrollTop))
    if (value !== snapshot().scrollTop) patch({ scrollTop: value })
  }

  function selectCommit(oid: string) {
    if (disposed) return
    const current = snapshot()
    if (current.selectedOid === oid && current.detail.data?.commit.oid === oid) return
    patch({ selectedOid: oid, file: null, fileDiff: emptyResource<GitCommitFileView>(), parentIndex: 0 })
    dropChannel('file', true)
    if (oid) loadDetail(oid, 0)
  }

  function openFile(path: string, parentIndex?: number) {
    if (disposed) return
    const current = snapshot()
    const oid = current.selectedOid
    if (!oid) return
    const index = typeof parentIndex === 'number' ? parentIndex : current.parentIndex
    if (current.file?.oid === oid && current.file.path === path && current.file.parentIndex === index
      && current.fileDiff.data && current.fileDiff.phase !== 'error') return
    patch({
      file: { oid, path, parentIndex: index },
      fileDiff: { phase: 'idle', data: null, error: '' },
    })
    loadFile(oid, path, index)
  }

  function closeFile() {
    if (disposed) return
    dropChannel('file', true)
    patch({ file: null, fileDiff: emptyResource<GitCommitFileView>() })
    touch()
  }

  function setParentIndex(parentIndex: number) {
    if (disposed) return
    const current = snapshot()
    const index = Math.max(0, Math.round(parentIndex))
    patch({
      parentIndex: index,
      file: current.file ? { ...current.file, parentIndex: index } : null,
    })
    if (current.file) loadFile(current.file.oid, current.file.path, index)
    if (current.selectedOid) loadDetail(current.selectedOid, index)
  }

  function refresh() {
    if (disposed || !snapshot().target) return
    loadStatus(() => loadActiveView(true))
  }

  function refreshHistory() {
    if (disposed || !snapshot().target) return
    loadHistory()
  }

  function refreshDiff() {
    if (disposed || !snapshot().target) return
    loadDiff()
  }

  function loadMore() {
    if (disposed) return
    loadHistory(true)
  }

  function retryDetail() {
    const current = snapshot()
    if (current.selectedOid) loadDetail(current.selectedOid, current.parentIndex)
  }

  function retryFile() {
    const file = snapshot().file
    if (file) loadFile(file.oid, file.path, file.parentIndex)
  }

  async function cancel() {
    const entries = [...channels.entries()]
    if (!entries.length) {
      if (snapshot().running) touch()
      return
    }
    for (const [channel, request] of entries) {
      channels.delete(channel)
      request.off?.()
      request.off = null
      await cancelRequest(request)
    }
    const current = snapshot()
    const relaxed = <T>(resource: GitResource<T>): GitResource<T> =>
      ({ ...resource, phase: resource.data ? 'ready' : 'idle', error: '' })
    patch({
      status: relaxed(current.status),
      history: relaxed(current.history),
      diff: relaxed(current.diff),
      detail: relaxed(current.detail),
      fileDiff: relaxed(current.fileDiff),
      running: actionBusy,
      canCancel: false,
    })
    touch()
  }

  function dispose() {
    if (disposed) return
    disposed = true
    for (const request of channels.values()) {
      request.off?.()
      request.off = null
    }
    channels.clear()
    actionOff?.()
    actionOff = null
    actionHandle = null
    actionBusy = false
    store.dispose()
  }

  return {
    getSnapshot: () => store.getSnapshot(),
    subscribe: (listener: () => void) => store.subscribe(listener),
    start,
    setTarget,
    setMessage: (message: string) => { if (!disposed) patch({ message }) },
    runStatus: () => { loadStatus() },
    runCommit: () => runAction('commit', { message: snapshot().message }),
    runInit: () => runAction('init'),
    selectTab,
    setHistoryRange,
    setFind,
    setScrollTop,
    selectCommit,
    openFile,
    closeFile,
    setParentIndex,
    refresh,
    refreshHistory,
    refreshDiff,
    loadMore,
    retryDetail,
    retryFile,
    cancel,
    dispose,
  }
}

export type GitModel = ReturnType<typeof createGitModel>

/** Shared directory of a component library, used as the Git working tree. */
export function componentBase(library: Pick<ComponentLibrary, 'symbolPath' | 'footprintPath'>): string {
  const symbol = (library.symbolPath || '').replace(/[\\/]+$/, '')
  const footprint = (library.footprintPath || '').replace(/[\\/]+$/, '')
  if (!symbol) return footprint
  if (!footprint) return symbol
  const separator = symbol.includes('\\') ? '\\' : '/'
  const a = symbol.split(/[\\/]+/), b = footprint.split(/[\\/]+/)
  let matched = 0
  while (matched < a.length && matched < b.length && a[matched].toLowerCase() === b[matched].toLowerCase()) matched++
  const base = a.slice(0, matched).join(separator)
  return matched <= 1 || /^[A-Za-z]:$/.test(base) ? symbol : base
}
