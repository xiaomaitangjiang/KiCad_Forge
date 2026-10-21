import * as React from 'react'
import { definePlugin, sidebarPanel } from '@kiforge/plugin-sdk'
import type { TaskSnapshot } from '@kiforge/plugin-sdk'
import { createGitModel, componentBase, HISTORY_MAX_COMMITS } from './model'
import type {
  GitCommitFile, GitFileSelection, GitModel, GitResource, GitSnapshot, GitStatusResult,
} from './model'
import { GRAPH_RAIL_WIDTH, groupRefs, laneCenter, layoutGraph, localBranches, matchCommits } from './graph'
import { useT } from './i18n'
import type { GraphRow } from './graph'
import './style.css'

const LANE_COLORS = [0, 1, 2, 3, 4, 5].map((lane) => `var(--gf-lane-${lane})`)
const laneColor = (lane: number) => LANE_COLORS[Math.abs(lane) % LANE_COLORS.length]

const shortOid = (oid: string) => oid.slice(0, 7)

function shortDate(value: string): string {
  if (!value) return ''
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return `${date.getMonth() + 1}/${date.getDate()} ${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`
}

function statusToken(status?: string): string {
  const raw = (status || '').trim()
  if (raw === '??' || raw === 'U' || raw === '?') return 'untracked'
  return raw.replace(/[^A-Za-z]/g, '') || 'unknown'
}
const statusGlyph = (status?: string) => (status || '?').trim().slice(0, 2) || '?'

function taskText(task: TaskSnapshot | null): string {
  if (!task) return ''
  if (task.error) return task.error
  if (task.state !== 'succeeded' || !task.result || typeof task.result !== 'object') return ''
  const output = (task.result as Record<string, unknown>).output
  return typeof output === 'string' ? output : ''
}

const useGitSnapshot = (model: GitModel) =>
  React.useSyncExternalStore(model.subscribe, model.getSnapshot, model.getSnapshot)

function ErrorBar({ message, onRetry, testid }: {
  message: string
  onRetry?: () => void
  testid?: string
}) {
  const t = useT()
  return (
    <div className="gf-git-error" data-testid={testid} role="alert">
      <span className="gf-git-error-text">{message}</span>
      {onRetry && <button type="button" className="gf-git-retry" onClick={onRetry}>{t('重试', 'Retry')}</button>}
    </div>
  )
}

function RefBadge({ ref }: { ref: { name: string; kind: string; is_head: boolean } }) {
  const label = ref.is_head ? (ref.name === 'HEAD' ? 'HEAD' : `HEAD → ${ref.name}`) : ref.name
  return (
    <span
      className={`gf-git-ref kind-${ref.kind}${ref.is_head ? ' head' : ''}`}
      title={label}
    >{label}</span>
  )
}

function GraphCell({ row, laneCount, railWidth }: {
  row: GraphRow
  laneCount: number
  railWidth: number
}) {
  const x = (lane: number) => laneCenter(lane, laneCount)
  const nodeX = x(row.lane)
  const nodeColor = laneColor(row.lane)
  const middle = 29
  const bottom = 58
  return (
    <div className="gf-git-graph-viewport" aria-hidden="true">
      <div className="gf-git-graph-track" style={{ width: railWidth }}>
        <svg className="gf-git-graph-cell" width={railWidth} height="58"
          viewBox={`0 0 ${railWidth} 58`} focusable="false">
          {row.passThrough.map((edge) => (
            <line key={`pass-${edge.lane}`} x1={x(edge.lane)} y1={0} x2={x(edge.lane)} y2={bottom}
              stroke={laneColor(edge.lane)} className="gf-git-edge" />
          ))}
          <line x1={nodeX} y1={0} x2={nodeX} y2={middle} stroke={nodeColor} className="gf-git-edge" />
          {row.parents.map((parent) => {
            const parentX = x(parent.lane)
            const path = parent.lane === row.lane
              ? `M ${nodeX} ${middle} L ${parentX} ${bottom}`
              : `M ${nodeX} ${middle} C ${nodeX} ${middle + 12}, ${parentX} ${bottom - 12}, ${parentX} ${bottom}`
            return (
              <path key={`parent-${parent.lane}-${parent.oid}`} d={path}
                stroke={laneColor(parent.lane)}
                className={`gf-git-edge${parent.continuation ? ' continuation' : ''}`}
                strokeDasharray={parent.continuation ? '3 3' : undefined} />
            )
          })}
          {row.parents.filter((parent) => parent.continuation).map((parent) => (
            <circle key={`continuation-${parent.lane}`} cx={x(parent.lane)} cy={bottom - 4} r={2}
              fill={laneColor(parent.lane)} className="gf-git-continuation-mark" />
          ))}
          <circle cx={nodeX} cy={middle} r={row.merge ? 4.5 : 5}
            fill={row.merge ? 'var(--panel)' : nodeColor} stroke={nodeColor} strokeWidth={2}
            className={`gf-git-node${row.merge ? ' merge' : ''}`} />
        </svg>
      </div>
    </div>
  )
}

function RepoHeader({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const repo: GitStatusResult | null = snapshot.status.data
  const files = repo?.files ?? []
  return (
    <header className="gf-git-repo">
      <label className="gf-git-repo-select">
        <span className="gf-git-visually-hidden">{t('元件库 / 仓库', 'Library / repository')}</span>
        <select
          data-testid="git-target"
          aria-label={t('元件库 / 仓库', 'Library / repository')}
          value={snapshot.target}
          onChange={(event) => model.setTarget(event.target.value)}
        >
          {!snapshot.libraries.length && <option value="">{t('（暂无元件库）', '(no libraries)')}</option>}
          {snapshot.libraries.map((library) => (
            <option key={library.id} value={componentBase(library)}>
              {library.name || library.id}
            </option>
          ))}
        </select>
      </label>
      <div className="gf-git-repo-meta">
        {repo?.initialized && (
          <span className="gf-git-branch" data-testid="git-branch" title={t('当前分支', 'Current branch')}>⑂ {repo.branch || 'main'}</span>
        )}
        {repo?.initialized && (
          <span className={`gf-git-badge ${files.length ? 'dirty' : 'clean'}`} data-testid="git-workspace-state">
            {files.length ? t('{n} 项更改', '{n} changed', { n: files.length }) : t('工作区干净', 'Clean working tree')}
          </span>
        )}
        {repo && repo.initialized === false && (
          <span className="gf-git-badge muted" data-testid="git-workspace-state">{t('未初始化', 'Not initialized')}</span>
        )}
        {repo?.ahead && <span className="gf-git-ahead" title={t('领先上游提交', 'Ahead of upstream')}>{repo.ahead}</span>}
        {snapshot.status.phase === 'loading' && <span className="gf-git-muted">{t('读取中…', 'Loading…')}</span>}
      </div>
      {snapshot.status.phase === 'error' && (
        <ErrorBar message={snapshot.status.error || t('读取仓库状态失败', 'Failed to read repository status')} onRetry={model.runStatus} testid="git-status-error" />
      )}
    </header>
  )
}

function ChangesPane({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const repo = snapshot.status.data
  const files = repo?.files ?? []
  return (
    <div className="gf-git-changes">
      {repo?.initialized && (
        <div className="gf-git-section-title">
          <span>{t('工作区更改', 'Working tree changes')} <span className="gf-git-count">{files.length}</span></span>
          <button type="button" className="gf-git-link-button" onClick={model.runStatus} disabled={snapshot.busy}>
            {t('刷新', 'Refresh')}
          </button>
        </div>
      )}
      {repo?.initialized === false && (
        <div className="gf-git-empty" data-testid="git-uninitialized">{t('未初始化仓库', 'Repository not initialized')}</div>
      )}
      {repo?.initialized && files.length === 0 && (
        <div className="gf-git-empty" data-testid="git-clean">{t('没有更改', 'No changes')}</div>
      )}
      {files.length > 0 && (
        <ul className="gf-git-file-list" data-testid="git-changes">
          {files.map((file) => (
            <li key={`${file.status}:${file.path}`} className="gf-git-file">
              <span className={`gf-git-file-status status-${statusToken(file.status)}`}>
                {statusGlyph(file.status)}
              </span>
              <span className="gf-git-file-path" title={file.path}>{file.path}</span>
            </li>
          ))}
        </ul>
      )}
      {!repo && snapshot.status.phase === 'loading' && <div className="gf-git-empty">{t('正在读取仓库…', 'Reading repository…')}</div>}
      {!repo && snapshot.status.phase === 'idle' && (
        <div className="gf-git-empty">{snapshot.target ? t('正在读取仓库…', 'Reading repository…') : t('请选择元件库', 'Select a library')}</div>
      )}
    </div>
  )
}

function FilePatch({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const resource = snapshot.fileDiff
  if (resource.phase === 'error') {
    return (
      <div className="gf-git-file-patch">
        <ErrorBar message={resource.error || t('读取文件差异失败', 'Failed to read file diff')} onRetry={model.retryFile} />
      </div>
    )
  }
  if (!resource.data) {
    return <div className="gf-git-file-patch"><div className="gf-git-empty">{t('正在读取文件差异…', 'Reading file diff…')}</div></div>
  }
  const data = resource.data
  return (
    <div className="gf-git-file-patch" data-testid="git-file-patch">
      {data.diffBase && <div className="gf-git-muted">{t('比较基准 {oid}', 'Compared with {oid}', { oid: shortOid(data.diffBase) })}</div>}
      {data.oldPath && <div className="gf-git-muted">{t('重命名自 {path}', 'Renamed from {path}', { path: data.oldPath })}</div>}
      {data.binary
        ? <div className="gf-git-empty">{t('二进制文件，无法显示文本差异', 'Binary file; no text diff available')}</div>
        : data.patch.trim()
          ? <pre className="gf-git-patch">{data.patch}</pre>
          : <div className="gf-git-empty">{t('没有文本差异', 'No text diff')}</div>}
      {data.truncated && <div className="gf-git-muted">{t('差异过大，内容已截断', 'Diff is too large and was truncated')}</div>}
    </div>
  )
}

function CommitDetail({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const oid = snapshot.selectedOid
  if (!oid) return null
  const detail = snapshot.detail
  if (detail.phase === 'error') {
    return (
      <section className="gf-git-detail" data-testid="git-commit-detail">
        <ErrorBar message={detail.error || t('读取提交详情失败', 'Failed to read commit details')} onRetry={model.retryDetail} />
      </section>
    )
  }
  if (!detail.data) {
    return (
      <section className="gf-git-detail" data-testid="git-commit-detail">
        <div className="gf-git-empty">{t('正在读取提交详情…', 'Reading commit details…')}</div>
      </section>
    )
  }
  const commit = detail.data.commit
  const files = detail.data.files ?? []
  const file: GitFileSelection | null = snapshot.file
  const comparison = detail.data.diffBase
    ? t('比较基准 {oid}', 'Compared with {oid}', { oid: shortOid(detail.data.diffBase) })
    : commit.parents.length > 1
      ? t('与父级 {n} 比较', 'Compared with parent {n}', { n: snapshot.parentIndex + 1 })
      : commit.parents.length === 1 ? t('与父提交比较', 'Compared with parent') : t('根提交', 'Root commit')
  return (
    <section className="gf-git-detail" data-testid="git-commit-detail" aria-label={t('提交详情', 'Commit details')}>
      <div className="gf-git-detail-heading">
        <strong>{t('提交详情', 'Commit details')}</strong>
        <span className="gf-git-oid" data-testid="git-commit-oid" title={commit.oid}>{commit.oid}</span>
      </div>
      <div className="gf-git-detail-subject">{commit.subject}</div>
      <dl className="gf-git-detail-meta">
        <dt>{t('作者', 'Author')}</dt><dd>{commit.author_name}</dd>
        <dt>{t('时间', 'Time')}</dt><dd title={commit.author_time}>{shortDate(commit.author_time)}</dd>
        <dt>{t('父级', 'Parents')}</dt>
        <dd>
          {commit.parents.length === 0 && t('根提交', 'Root commit')}
          {commit.parents.map((parent, index) => (
            <button type="button" key={parent} className="gf-git-parent" title={parent}
              onClick={() => model.selectCommit(parent)}>
              {commit.parents.length > 1 ? `${index + 1}·${shortOid(parent)}` : shortOid(parent)}
            </button>
          ))}
        </dd>
      </dl>
      {commit.body && commit.body.trim() && commit.body.trim() !== commit.subject && (
        <details className="gf-git-detail-body">
          <summary>{t('完整消息', 'Full message')}</summary>
          <pre className="gf-git-message-body">{commit.body}</pre>
        </details>
      )}
      {detail.data.bodyTruncated && <div className="gf-git-muted">{t('提交消息过长，已截断', 'Commit message is too long and was truncated')}</div>}
      {commit.parents.length > 1 && (
        <div className="gf-git-parent-picker">
          <span className="gf-git-muted">{t('比较父级', 'Compare parent')}</span>
          {commit.parents.map((parent, index) => (
            <button type="button" key={parent}
              className={index === snapshot.parentIndex ? 'selected' : ''}
              aria-pressed={index === snapshot.parentIndex}
              title={parent}
              onClick={() => model.setParentIndex(index)}>
              {t('父级', 'Parent')} {index + 1}
            </button>
          ))}
        </div>
      )}
      <div className="gf-git-detail-files">
        <div className="gf-git-section-title">
          <span>{t('文件', 'Files')} <span className="gf-git-count">{files.length}</span></span>
          <span className="gf-git-muted">{comparison}</span>
        </div>
        {files.length === 0
          ? <div className="gf-git-empty">{t('该提交没有文件变更', 'This commit has no file changes')}</div>
          : (
            <ul className="gf-git-detail-file-list">
              {files.map((entry: GitCommitFile) => {
                const open = file?.oid === commit.oid && file.path === entry.path
                return (
                  <li key={`${entry.status ?? ''}:${entry.path}`} className={`gf-git-detail-file${open ? ' open' : ''}`}>
                    <button type="button" className="gf-git-detail-file-row" aria-expanded={open}
                      onClick={() => { if (open) model.closeFile(); else model.openFile(entry.path) }}>
                      <span className={`gf-git-file-status status-${statusToken(entry.status)}`}>
                        {statusGlyph(entry.status)}
                      </span>
                      <span className="gf-git-file-path" title={entry.old_path ? `${entry.old_path} → ${entry.path}` : entry.path}>
                        {entry.path}
                      </span>
                      {(entry.additions != null || entry.deletions != null) && (
                        <span className="gf-git-diffstat">
                          <span className="add">+{entry.additions ?? 0}</span>
                          <span className="del">−{entry.deletions ?? 0}</span>
                        </span>
                      )}
                      {entry.binary && <span className="gf-git-tag">{t('二进制', 'Binary')}</span>}
                    </button>
                    {open && <FilePatch model={model} snapshot={snapshot} />}
                  </li>
                )
              })}
            </ul>
          )}
        {detail.data.filesTruncated && (
          <div className="gf-git-muted">{t('文件列表过长，已截断；请缩小提交或刷新范围', 'File list is too long and was truncated; narrow the commit or refresh')}</div>
        )}
      </div>
    </section>
  )
}

function HistoryPane({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const history = snapshot.history
  const data = history.data
  const commits = data?.commits ?? []
  const layout = React.useMemo(() => layoutGraph(commits), [commits])
  const rows = layout.rows
  const laneOverflow = layout.laneOverflow
  const refs = React.useMemo(() => groupRefs(data?.refs ?? []), [data?.refs])
  const matches = React.useMemo(() => matchCommits(commits, snapshot.find), [commits, snapshot.find])
  const listRef = React.useRef<HTMLDivElement | null>(null)
  const pendingScroll = React.useRef(snapshot.scrollTop)
  const scrollTimer = React.useRef<number | null>(null)
  const mountedScroll = React.useRef(snapshot.scrollTop)
  const railWidth = layout.railWidth
  const maxGraphShift = Math.max(0, railWidth - GRAPH_RAIL_WIDTH)
  const [graphShift, setGraphShift] = React.useState(0)
  const graphScrollRef = React.useRef<HTMLDivElement | null>(null)

  React.useEffect(() => {
    setGraphShift((shift) => (maxGraphShift <= 0 ? 0 : Math.min(shift, maxGraphShift)))
  }, [maxGraphShift])

  React.useEffect(() => {
    const node = graphScrollRef.current
    if (node && Math.abs(node.scrollLeft - graphShift) > 0.5) node.scrollLeft = graphShift
  }, [graphShift])

  const onGraphScroll = (event: React.UIEvent<HTMLDivElement>) => {
    const next = Math.min(maxGraphShift, Math.max(0, event.currentTarget.scrollLeft))
    setGraphShift((shift) => (shift === next ? shift : next))
  }

  React.useLayoutEffect(() => {
    const node = listRef.current
    if (node) node.scrollTop = mountedScroll.current
  }, [])

  React.useEffect(() => () => {
    if (scrollTimer.current !== null) window.clearTimeout(scrollTimer.current)
    model.setScrollTop(pendingScroll.current)
  }, [model])

  const onScroll = (event: React.UIEvent<HTMLDivElement>) => {
    pendingScroll.current = event.currentTarget.scrollTop
    if (scrollTimer.current !== null) return
    scrollTimer.current = window.setTimeout(() => {
      scrollTimer.current = null
      model.setScrollTop(pendingScroll.current)
    }, 120)
  }

  const selectOffset = (offset: number) => {
    if (!rows.length) return
    const current = Math.max(0, rows.findIndex((row) => row.oid === snapshot.selectedOid))
    const next = Math.min(rows.length - 1, Math.max(0, current + offset))
    const oid = rows[next].oid
    model.selectCommit(oid)
    window.requestAnimationFrame(() => {
      listRef.current
        ?.querySelector(`[data-commit-oid="${oid}"]`)
        ?.scrollIntoView({ block: 'nearest' })
    })
  }

  const onKeyDown = (event: React.KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'ArrowDown') {
      event.preventDefault()
      selectOffset(1)
    } else if (event.key === 'ArrowUp') {
      event.preventDefault()
      selectOffset(-1)
    } else if (event.key === 'Home') {
      event.preventDefault()
      if (rows[0]) model.selectCommit(rows[0].oid)
    } else if (event.key === 'End') {
      event.preventDefault()
      if (rows[rows.length - 1]) model.selectCommit(rows[rows.length - 1].oid)
    } else if (event.key === 'Enter' && snapshot.selectedOid) {
      event.preventDefault()
      model.selectCommit(snapshot.selectedOid)
    }
  }

  const branches = localBranches(data?.refs ?? [])
  if (snapshot.branchRef && !branches.includes(snapshot.branchRef)) branches.push(snapshot.branchRef)
  const rangeValue = snapshot.range === 'all' ? 'all' : snapshot.branchRef ? `branch:${snapshot.branchRef}` : 'head'
  const caption = snapshot.find.trim() && commits.length > 0 && matches.size === 0
    ? t('没有匹配的已加载提交', 'No matching loaded commits')
    : commits.length
      ? t('{n} 条提交', '{n} commits', { n: commits.length })
        + (matches.size > 0 ? t(' · {n} 条匹配', ' · {n} matched', { n: matches.size }) : '')
      : data?.state === 'unborn' ? t('仓库还没有提交', 'No commits yet') : t('暂无提交', 'No commits to show')
  const stateCaption = history.phase === 'stale'
    ? t('快照已变化，需刷新', 'Snapshot changed; refresh')
    : history.phase === 'loading'
      ? t('刷新中…', 'Refreshing…')
      : data?.truncated ? t('结果已截断', 'Results truncated') : data?.page?.has_more ? t('可继续加载', 'More available') : t('已全部加载', 'All loaded')

  return (
    <div className="gf-git-history">
      <div className="gf-git-history-tools">
        <select
          data-testid="git-history-range"
          aria-label={t('历史范围', 'History range')}
          value={rangeValue}
          onChange={(event) => {
            const value = event.target.value
            if (value === 'all') model.setHistoryRange('all')
            else if (value === 'head') model.setHistoryRange('branch')
            else model.setHistoryRange('branch', value.slice('branch:'.length))
          }}
        >
          <option value="all">{t('全部引用', 'All refs')}</option>
          <option value="head">{t('当前分支', 'Current branch')}</option>
          {branches.map((name) => <option key={name} value={`branch:${name}`}>{name}</option>)}
        </select>
        <button type="button" data-testid="git-history-refresh" title={t('刷新只读历史', 'Refresh read-only history')}
          disabled={history.phase === 'loading'} onClick={model.refreshHistory}>
          {t('刷新', 'Refresh')}
        </button>
      </div>
      <input
        className="gf-git-history-find"
        data-testid="git-history-find"
        type="search"
        value={snapshot.find}
        placeholder={t('查找已加载提交：消息 / 作者 / SHA', 'Filter loaded commits: message / author / SHA')}
        aria-label={t('查找已加载提交', 'Filter loaded commits')}
        onChange={(event) => model.setFind(event.target.value)}
      />
      {(history.phase === 'error' || history.phase === 'stale') && (
        <ErrorBar message={history.error || t('读取历史失败', 'Failed to read history')} onRetry={model.refreshHistory} testid="git-history-error" />
      )}
      <div className="gf-git-history-caption">
        <span>{caption}</span>
        <span>{stateCaption}</span>
      </div>
      {(laneOverflow > 0 || data?.truncated || data?.refsTruncated || data?.detached) && (
        <div className="gf-git-history-note">
          {laneOverflow > 0 && (
            <span>{t('轨道超出 74px 视宽，可拖动「轨道」滚动条查看其余 {n} 条并行分支', 'The graph rail exceeds 74px; drag the "Rail" scrollbar to see {n} more parallel branches', { n: laneOverflow })}</span>
          )}
          {data?.detached && <span>{data.headRef ? t('分离 HEAD（{ref}）', 'Detached HEAD ({ref})', { ref: data.headRef }) : t('分离 HEAD', 'Detached HEAD')}</span>}
          {data?.refsTruncated && <span>{t('引用过多，仅显示部分 refs', 'Too many refs; only a subset is shown')}</span>}
          {data?.truncated && <span>{t('结果按预算截断，请缩小范围或刷新', 'Results were truncated by budget; narrow the range or refresh')}</span>}
        </div>
      )}
      {rows.length > 0 && maxGraphShift > 0 && (
        <div className="gf-git-graph-scroll-row">
          <span className="gf-git-graph-scroll-label">{t('轨道', 'Rail')}</span>
          <div
            ref={graphScrollRef}
            className="gf-git-graph-scroll"
            data-testid="git-history-graph-scroll"
            role="scrollbar"
            aria-label={t('历史轨道横向滚动', 'Scroll the history graph rail horizontally')}
            aria-controls="gf-git-history-list"
            aria-orientation="horizontal"
            aria-valuemin={0}
            aria-valuemax={Math.round(maxGraphShift)}
            aria-valuenow={Math.round(graphShift)}
            tabIndex={0}
            title={t('拖动查看完整分支轨道', 'Drag to see the full branch rail')}
            onScroll={onGraphScroll}
          >
            <div className="gf-git-graph-scroll-spacer"
              style={{ width: `calc(100% + ${maxGraphShift}px)` }} />
          </div>
        </div>
      )}
      {history.phase === 'loading' && !data && <div className="gf-git-empty">{t('正在读取历史…', 'Reading history…')}</div>}
      {history.phase === 'empty' && !commits.length && (
        <div className="gf-git-empty">
          {data?.state === 'unborn' ? t('仓库还没有提交；提交后会出现在这里。', 'No commits yet; new commits will appear here.') : t('没有可显示的提交', 'No commits to show')}
        </div>
      )}
      {rows.length > 0 && (
        <div
          className="gf-git-history-list"
          id="gf-git-history-list"
          ref={listRef}
          role="listbox"
          style={graphShift ? { '--gf-git-graph-shift': `${-graphShift}px` } as React.CSSProperties : undefined}
          aria-label={t('提交历史', 'Commit history')}
          tabIndex={0}
          aria-activedescendant={snapshot.selectedOid ? `gf-git-commit-${snapshot.selectedOid}` : undefined}
          onScroll={onScroll}
          onKeyDown={onKeyDown}
        >
          {rows.map((row, index) => {
            const commit = commits[index]
            const selected = snapshot.selectedOid === row.oid
            const dimmed = matches.size > 0 && !matches.has(row.oid)
            return (
              <div
                key={row.oid}
                id={`gf-git-commit-${row.oid}`}
                role="option"
                aria-selected={selected}
                data-commit-oid={row.oid}
                className={`gf-git-history-row${selected ? ' selected' : ''}${dimmed ? ' dimmed' : ''}`}
                title={`${commit.subject}\n${commit.oid}${row.continues ? `\n${t('父级未加载', 'Parent not loaded')}` : ''}`}
                onClick={() => model.selectCommit(row.oid)}
              >
                <GraphCell row={row} laneCount={layout.laneCount} railWidth={railWidth} />
                <span className="gf-git-commit-text">
                  <span className="gf-git-commit-subject">
                    {(refs.get(row.oid) ?? []).slice(0, 4).map((ref) => (
                      <RefBadge key={`${ref.kind}:${ref.name}`} ref={ref} />
                    ))}
                    {(refs.get(row.oid)?.length ?? 0) > 4 && (
                      <span className="gf-git-ref more">+{(refs.get(row.oid)?.length ?? 0) - 4}</span>
                    )}
                    <span className="gf-git-subject">{commit.subject}</span>
                    {row.merge && <span className="gf-git-merge-tag">{t('合并', 'Merge')}</span>}
                  </span>
                  <span className="gf-git-commit-meta">
                    <span className="gf-git-oid" title={commit.oid}>{shortOid(commit.oid)}</span>
                    <span className="gf-git-author">{commit.author_name}</span>
                    <span className="gf-git-date" title={commit.author_time}>{shortDate(commit.author_time)}</span>
                  </span>
                </span>
              </div>
            )
          })}
        </div>
      )}
      {data?.page?.has_more && commits.length < HISTORY_MAX_COMMITS && (
        <button type="button" className="gf-git-load-more" data-testid="git-history-more"
          disabled={history.phase === 'loading' || history.phase === 'stale'} onClick={model.loadMore}>
          {t('加载更多（每次 50 条）', 'Load more (50 at a time)')}
        </button>
      )}
      {commits.length >= HISTORY_MAX_COMMITS && (
        <div className="gf-git-muted gf-git-history-limit">{t('已加载 {n} 条上限，请刷新或缩小范围', 'Loaded the {n}-commit limit; refresh or narrow the range', { n: HISTORY_MAX_COMMITS })}</div>
      )}
      <CommitDetail model={model} snapshot={snapshot} />
    </div>
  )
}

function DiffPane({ model, snapshot }: { model: GitModel; snapshot: GitSnapshot }) {
  const t = useT()
  const diff = snapshot.diff
  return (
    <div className="gf-git-diff-pane">
      <div className="gf-git-history-tools">
        <span className="gf-git-muted">{t('工作区改动', 'Working tree changes')}</span>
        <button type="button" data-testid="git-diff-refresh" disabled={diff.phase === 'loading'}
          onClick={model.refreshDiff}>
          {t('刷新', 'Refresh')}
        </button>
      </div>
      {diff.phase === 'error' && (
        <ErrorBar message={diff.error || t('读取差异失败', 'Failed to read diff')} onRetry={model.refreshDiff} testid="git-diff-error" />
      )}
      {diff.phase === 'loading' && !diff.data && <div className="gf-git-empty">{t('正在读取差异…', 'Reading diff…')}</div>}
      {diff.phase === 'empty' && <div className="gf-git-empty">{t('没有未提交的差异', 'No uncommitted diff')}</div>}
      {diff.data && (
        <pre className="gf-git-diff-output" data-testid="git-diff-output">{diff.data}</pre>
      )}
    </div>
  )
}

/** The whole Git flow lives in the primary sidebar (WP-001/002/005). */
function GitSidebarPanel({ model }: { model: GitModel }) {
  const t = useT()
  const snapshot = useGitSnapshot(model)
  React.useEffect(() => { void model.start() }, [model])
  const files = snapshot.status.data?.files ?? []
  const artifact = snapshot.task !== null
    || snapshot.status.phase !== 'idle'
    || snapshot.history.phase !== 'idle'
    || snapshot.diff.phase !== 'idle'
    || snapshot.detail.phase !== 'idle'
    || snapshot.fileDiff.phase !== 'idle'
  const output = taskText(snapshot.task)

  return (
    <section className={`gf-git-panel gf-git-details gf-git-sidebar`}
      data-testid="git-panel" aria-busy={snapshot.running}>
      <RepoHeader model={model} snapshot={snapshot} />
      {snapshot.loadError && (
        <ErrorBar message={snapshot.loadError} onRetry={() => { void model.start() }} testid="git-library-error" />
      )}

      <div className="gf-git-tabs" role="tablist" aria-label={t('Git 视图', 'Git view')}>
        <button type="button" role="tab" id="gf-git-tab-changes"
          aria-selected={snapshot.tab === 'changes'} aria-controls="gf-git-pane"
          data-testid="git-tab-changes" disabled={!snapshot.target}
          className={`gf-git-tab${snapshot.tab === 'changes' ? ' selected' : ''}`}
          onClick={() => model.selectTab('changes')}>
          {t('更改', 'Changes')} <span className="gf-git-count">{snapshot.status.data ? files.length : '·'}</span>
        </button>
        <button type="button" role="tab" id="gf-git-tab-history"
          aria-selected={snapshot.tab === 'history'} aria-controls="gf-git-pane"
          data-testid="git-tab-history" disabled={!snapshot.target}
          className={`gf-git-tab${snapshot.tab === 'history' ? ' selected' : ''}`}
          onClick={() => model.selectTab('history')}>{t('历史', 'History')}</button>
        <button type="button" role="tab" id="gf-git-tab-diff"
          aria-selected={snapshot.tab === 'diff'} aria-controls="gf-git-pane"
          data-testid="git-tab-diff" disabled={!snapshot.target}
          className={`gf-git-tab${snapshot.tab === 'diff' ? ' selected' : ''}`}
          onClick={() => model.selectTab('diff')}>{t('差异', 'Diff')}</button>
      </div>

      <div className="gf-git-tab-pane" id="gf-git-pane" role="tabpanel"
        aria-labelledby={`gf-git-tab-${snapshot.tab}`} data-testid={`git-${snapshot.tab}-pane`}>
        {snapshot.tab === 'changes' && <ChangesPane model={model} snapshot={snapshot} />}
        {snapshot.tab === 'history' && <HistoryPane model={model} snapshot={snapshot} />}
        {snapshot.tab === 'diff' && <DiffPane model={model} snapshot={snapshot} />}
      </div>

      <div className="gf-git-commit">
        <textarea
          data-testid="git-message"
          rows={2}
          placeholder={t('消息（Ctrl+Enter 提交）', 'Message (Ctrl+Enter to commit)')}
          aria-label={t('提交消息', 'Commit message')}
          value={snapshot.message}
          onChange={(event) => model.setMessage(event.target.value)}
          onKeyDown={(event) => {
            if ((event.ctrlKey || event.metaKey) && event.key === 'Enter'
              && snapshot.message.trim() && !snapshot.busy && snapshot.target) {
              event.preventDefault()
              model.runCommit()
            }
          }}
        />
        <button type="button" className="gf-git-primary"
          disabled={snapshot.busy || snapshot.running || !snapshot.target || !snapshot.message.trim()}
          onClick={model.runCommit}>
          {t('提交', 'Commit')}
        </button>
      </div>

      <div className="gf-git-actions">
        <button type="button" disabled={snapshot.busy || snapshot.running || !snapshot.target} onClick={model.runStatus}>{t('状态', 'Status')}</button>
        <button type="button" disabled={snapshot.busy || snapshot.running || !snapshot.target} onClick={model.runInit}>{t('初始化仓库', 'Initialize repository')}</button>
        {snapshot.canCancel && (
          <button type="button" className="gf-git-cancel" onClick={() => void model.cancel()}>{t('取消任务', 'Cancel task')}</button>
        )}
      </div>
      {snapshot.running && <div className="gf-git-running" data-testid="git-running" role="status">{t('任务进行中…', 'Task running…')}</div>}
      {artifact && (
        <div className="gf-git-task-result" data-testid="git-task-result" hidden={!output}>{output}</div>
      )}
    </section>
  )
}

export default definePlugin({
  name: 'com.kicad_forge.git_version.frontend',
  apply(ctx) {
    const model = createGitModel({
      createStore: ctx.model.createStore,
      librariesApi: ctx.libraries,
      tasks: ctx.tasks,
    })
    ctx.on('dispose', model.dispose)
    ctx.slots.register(sidebarPanel, {
      id: 'git-panel',
      title: 'Git',
      // The manifest declares icon.svg; the host resolves it to an asset URL.
      // Do not inline a second copy of the icon here.
      render: () => <GitSidebarPanel model={model} />,
    })
  },
})
