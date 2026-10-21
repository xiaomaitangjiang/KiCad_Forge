// R4 regression: lost commit replies must retry the original request, never
// re-run preflight and never mint a second operation id.
import test from 'node:test'
import assert from 'node:assert/strict'
import { COMMIT_PHASES, createCommitSession, isUncertainCommitError } from '../src/editing/paste-commit.js'

function request(operationId = 'op-1') {
  return {
    target_library_id: 'lib-b',
    operation_id: operationId,
    dry_run: false,
    renames: { Alpha: 'Alpha2' },
    items: [{ name: 'Alpha', sexpr: '(symbol "Alpha")', dependency: false }],
  }
}

test('paste commit: success preflights once and commits the frozen request', async () => {
  const calls = []
  const session = createCommitSession({
    preflight: async (body) => { calls.push(['preflight', body]); return { target: { digest: 'D1' }, conflicts: [] } },
    commit: async (body, plan) => { calls.push(['commit', body, plan]); return { ok: true, created: [{ name: 'Alpha2' }] } },
  })
  const started = await session.submit(request())
  assert.equal(started.status, 'preflighted')
  const done = await session.commitPlan()
  assert.equal(done.status, 'committed')
  assert.equal(calls.length, 2)
  assert.equal(calls[1][1].operation_id, 'op-1')
  assert.equal(session.state().phase, COMMIT_PHASES.committed)
})

test('paste commit: a lost reply becomes uncertain and retries the same request', async () => {
  let commitCalls = 0
  let preflightCalls = 0
  let mode = 'lost'
  const session = createCommitSession({
    preflight: async () => { preflightCalls += 1; return { target: { digest: 'D1' }, conflicts: [] } },
    commit: async (body) => {
      commitCalls += 1
      if (mode === 'lost') {
        const error = new Error('Network error')
        error.kind = 'network'
        throw error
      }
      return { ok: true, replayed: true, created: [{ name: body.renames.Alpha }] }
    },
  })
  await session.submit(request())
  const lost = await session.commitPlan()
  assert.equal(lost.status, 'uncertain')
  assert.equal(session.state().canRetry, true)
  mode = 'ok'
  const retried = await session.retry()
  assert.equal(retried.status, 'committed')
  assert.equal(preflightCalls, 1)
  assert.equal(commitCalls, 2)
})

test('paste commit: a deterministic conflict returns to idle and does not expose retry', async () => {
  const session = createCommitSession({
    preflight: async () => ({ target: { digest: 'D1' }, conflicts: [] }),
    commit: async () => {
      const error = new Error('conflict')
      error.kind = 'http'
      error.status = 409
      throw error
    },
  })
  await session.submit(request())
  const failed = await session.commitPlan()
  assert.equal(failed.status, 'failed')
  assert.equal(session.state().phase, COMMIT_PHASES.idle)
  assert.equal(session.state().canRetry, false)
})

test('paste commit: retry refuses to run after success and submit is ignored while busy', async () => {
  let release
  const session = createCommitSession({
    preflight: () => new Promise((resolve) => { release = resolve }),
    commit: async () => ({ ok: true }),
  })
  const first = session.submit(request())
  const second = await session.submit(request('op-2'))
  assert.equal(second.status, 'ignored')
  release({ target: { digest: 'D1' } })
  await first
  await session.commitPlan()
  assert.equal((await session.retry()).status, 'ignored')
})

test('paste commit: request is frozen before preflight', async () => {
  const seen = []
  const session = createCommitSession({
    preflight: async (body) => { seen.push(body); return { target: { digest: 'D1' } } },
    commit: async () => ({ ok: true }),
  })
  const body = request()
  const pending = session.submit(body)
  body.renames.Alpha = 'ChangedLater'
  body.items[0].name = 'ChangedLater'
  await pending
  assert.equal(seen[0].renames.Alpha, 'Alpha2')
  assert.equal(seen[0].items[0].name, 'Alpha')
})

test('paste commit: uncertainty classification covers network and 5xx only', () => {
  assert.equal(isUncertainCommitError({ kind: 'network' }), true)
  assert.equal(isUncertainCommitError({ kind: 'http', status: 503 }), true)
  assert.equal(isUncertainCommitError({ kind: 'http', status: 409 }), false)
  assert.equal(isUncertainCommitError({ kind: 'http', status: 400 }), false)
})
