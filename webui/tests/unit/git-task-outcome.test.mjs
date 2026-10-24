import test from 'node:test'
import assert from 'node:assert/strict'
import { describeTaskOutcome } from '../../../plugins/git_version/frontend/src/task-outcome.ts'

const uncertainTask = {
  id: 'task-1',
  action: 'commit',
  state: 'failed',
  cancelable: false,
  error: 'git commit timed out',
  uncertain: true,
}

test('Git uncertain task keeps its original state/error/output and offers only read-only checks', () => {
  const outcome = describeTaskOutcome(uncertainTask, 'zh')

  assert.equal(outcome.uncertain, true)
  assert.equal(outcome.state, 'failed')
  assert.equal(outcome.error, 'git commit timed out')
  assert.equal(outcome.output, '')
  assert.equal(outcome.resultText, 'git commit timed out')
  assert.equal(outcome.title, '结果待核对')
  assert.equal(outcome.stateLabel, '状态：failed')
  assert.match(outcome.detail, /写入结果不确定/)
  assert.match(outcome.detail, /failed/)
  assert.match(outcome.detail, /不要重复提交或初始化仓库/)
  assert.match(outcome.readOnlyNote, /不会提交或初始化/)
  assert.deepEqual(outcome.verifyActions.map((action) => action.id), ['status', 'history'])
  assert.deepEqual(outcome.verifyActions.map((action) => action.label), ['刷新状态', '刷新历史'])
  assert.ok(outcome.verifyActions.every((action) => action.id !== 'commit' && action.id !== 'init'))
})

test('Git uncertain outcome is absent for false, missing, or non-boolean flags', () => {
  for (const uncertain of [false, undefined, 'true', 1]) {
    const outcome = describeTaskOutcome({ ...uncertainTask, uncertain }, 'zh')
    assert.equal(outcome.uncertain, false)
    assert.equal(outcome.title, '')
    assert.equal(outcome.detail, '')
    assert.equal(outcome.stateLabel, '')
    assert.equal(outcome.readOnlyNote, '')
    assert.deepEqual(outcome.verifyActions, [])
    assert.equal(outcome.state, 'failed')
    assert.equal(outcome.error, 'git commit timed out')
    assert.equal(outcome.resultText, 'git commit timed out')
  }
})

test('Git uncertain task is bilingual while preserving the original payload', () => {
  const zh = describeTaskOutcome(uncertainTask, 'zh')
  const en = describeTaskOutcome(uncertainTask, 'en')

  assert.equal(en.uncertain, true)
  assert.equal(en.title, 'Result needs review')
  assert.equal(en.stateLabel, 'State: failed')
  assert.match(en.detail, /Write outcome is uncertain/)
  assert.match(en.detail, /do not commit or initialize again/)
  assert.equal(en.readOnlyNote, 'Read-only check; it does not commit or initialize')
  assert.deepEqual(en.verifyActions.map((action) => action.label), ['Refresh status', 'Refresh history'])
  assert.equal(en.state, zh.state)
  assert.equal(en.error, zh.error)
  assert.equal(en.output, zh.output)
  assert.equal(en.resultText, zh.resultText)
})

test('Git uncertain task preserves both a diagnostic error and partial output', () => {
  const outcome = describeTaskOutcome({
    state: 'succeeded',
    error: 'connection lost after commit',
    result: { output: '[main abc123] commit subject' },
    uncertain: true,
  }, 'zh')

  assert.equal(outcome.state, 'succeeded')
  assert.equal(outcome.error, 'connection lost after commit')
  assert.equal(outcome.output, '[main abc123] commit subject')
  assert.equal(outcome.resultText, 'connection lost after commit\n[main abc123] commit subject')
})

test('Git uncertain task keeps long error and output verbatim without folding them into detail', () => {
  const error = 'E'.repeat(2048)
  const output = '输出'.repeat(2048)
  const outcome = describeTaskOutcome({
    state: 'failed', error, result: { output }, uncertain: true,
  }, 'zh')

  assert.equal(outcome.error, error)
  assert.equal(outcome.output, output)
  assert.equal(outcome.error.length, 2048)
  assert.equal(outcome.output.length, 4096)
  assert.equal(outcome.resultText, `${error}\n${output}`)
  assert.ok(!outcome.detail.includes(output), 'long output must stay in the result line, not the banner detail')
})

test('Git ordinary tasks keep the historical result text rules and malformed input is safe', () => {
  assert.equal(describeTaskOutcome({
    state: 'failed', result: { output: 'should stay hidden' },
  }, 'zh').resultText, '')
  assert.equal(describeTaskOutcome({
    state: 'succeeded', result: { output: 'done' },
  }, 'zh').resultText, 'done')
  assert.equal(describeTaskOutcome(null, 'zh').resultText, '')
  assert.equal(describeTaskOutcome(null, 'zh').uncertain, false)
  assert.deepEqual(describeTaskOutcome(null, 'zh').verifyActions, [])

  const malformed = describeTaskOutcome({
    state: 7,
    error: new Error('not a wire string'),
    result: { output: 42 },
    uncertain: true,
  }, 'zh')
  assert.equal(malformed.uncertain, true)
  assert.equal(malformed.state, '')
  assert.equal(malformed.error, '')
  assert.equal(malformed.output, '')
  assert.equal(malformed.resultText, '')
  assert.equal(malformed.stateLabel, '状态：未知')
  assert.equal(malformed.title, '结果待核对')
})
