import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import i18next from 'i18next'
import { api, setLoadingCallback } from '../../src/api.js'
import { committedRefreshOutcome, configFrom, emptySample, isMatchingReport, isMatchingSamplePreview, newMatchingEntry,
  parseMatchingRead, sameMatchingContent, sampleBody, validateMatchingDraft, matchingErrorMessage,
  matchingMessage, translateMatchingMessage } from '../../src/lib/typeMatching.js'
import { resolveMode, validateOutcome } from '../browser/run-lcsc-browser.mjs'

const rule = { id: 'r1', field: 'reference', pattern: '^R', priority: 2147483647,
  case_sensitive: false, enabled: true, type_id: 'custom:relay' }
const exception = { id: 'e1', field: 'model', key: '  AbC  ', enabled: true, type_id: 'custom:relay' }
const config = () => ({ schema_version: 1, revision: 2, rules: [{ ...rule }], exceptions: [{ ...exception }] })
const read = () => ({ ...config(), ok: true, has_snapshot: true, load_error: '', scope: 'global',
  engine: { name: 'PCRE2' }, limits: { max_rules: 500 } })
const counts = { total: 3, revision: 2, protected_count: 1, matched: 1, unmatched: 0, conflicts: 1,
  input_too_long: 0, evaluation_limited: 0, evaluation_errors: 0, would_update: 1, unchanged: 0,
  type_updated: 1, source_changed: 0, cas_skipped: 0, deleted: 0, rolled_back: 0, not_attempted: 0 }
const report = () => ({ ok: false, error: 'write stopped', summary: { ...counts }, details: [{
  name: 'R1', status: 'matched', reason: 'source matched', structured_evidence: [] }] })

test('匹配配置：顶层状态与能力不混入保存DTO，原文、类型ID和顺序保留', () => {
  const parsed = parseMatchingRead(read())
  assert.equal(parsed.healthy, true)
  assert.deepEqual(parsed.config, config())
  assert.equal(parsed.config.exceptions[0].key, '  AbC  ')
  assert.equal(parsed.config.rules[0].priority, 2147483647)
  assert.equal(parsed.engine.name, 'PCRE2')
  assert.equal('scope' in parsed.config, false)
})

test('匹配配置：坏版本/安全整数/缺字段/坏列表/重复跨表ID不作为空配置', () => {
  for (const patch of [{ schema_version: 2 }, { revision: -1 }, { revision: 9007199254740992 },
    { rules: null }, { rules: [null] }, { exceptions: null }, { rules: [{ ...rule, enabled: 'true' }] },
    { exceptions: [{ ...exception, id: 'r1' }] }, { rules: [{ ...rule, field: '3d' }] },
    { rules: [{ ...rule, priority: 2147483648 }] }, { rules: [{ ...rule, priority: -2147483649 }] },
    { rules: [{ ...rule, priority: 1.2 }] }, { rules: [{ ...rule, type_id: '' }] }]) {
    assert.throws(() => configFrom({ ...config(), ...patch }), undefined, JSON.stringify(patch))
  }
  const missing = config(); delete missing.rules[0].case_sensitive
  assert.throws(() => configFrom(missing))
})

test('匹配配置：UTF8字节限制、空正则、非空精确原文与int32两端', () => {
  assert.doesNotThrow(() => configFrom({ ...config(), rules: [{ ...rule, pattern: '', priority: -2147483648 }] }))
  assert.throws(() => configFrom({ ...config(), rules: [{ ...rule, pattern: '中'.repeat(171) }] }))
  assert.throws(() => configFrom({ ...config(), exceptions: [{ ...exception, key: '' }] }))
  assert.doesNotThrow(() => configFrom({ ...config(), exceptions: [{ ...exception, key: '  ' }] }))
  assert.throws(() => configFrom({ ...config(), exceptions: [{ ...exception, key: 'x'.repeat(4097) }] }))
  assert.throws(() => configFrom({ ...config(), rules: Array.from({ length: 501 }, (_, id) => ({ ...rule, id: String(id) })) }))
})

test('读取状态：首次坏加载没有伪造配置；lastvalid保留但不可写', () => {
  const noSnapshot = parseMatchingRead({ ok: false, has_snapshot: false, load_error: 'bad file' })
  assert.equal(noSnapshot.config, null)
  assert.equal(noSnapshot.healthy, false)
  assert.equal(noSnapshot.error, 'bad file')
  const lastValid = parseMatchingRead({ ...read(), ok: false, load_error: 'broken now' })
  assert.deepEqual(lastValid.config, config())
  assert.equal(lastValid.healthy, false)
  for (const invalid of [null, [], { ...read(), ok: 'true' }, { ...read(), has_snapshot: 'true' },
    { ...read(), load_error: 0 }, { ok: true, has_snapshot: false, load_error: '' }, { ...read(), load_error: 'bad' }]) {
    assert.throws(() => parseMatchingRead(invalid))
  }
})

test('草稿：比较忽略CAS版本，禁用/顺序/原文差异保留；缺失类型不自动替换', () => {
  assert.equal(sameMatchingContent(config(), { ...config(), revision: 9 }), true)
  const reordered = config()
  reordered.rules = [{ type_id: rule.type_id, enabled: true, ...rule }]
  assert.equal(sameMatchingContent(config(), reordered), true, 'JSON字段顺序不影响已保存内容比较')
  const changed = config(); changed.exceptions[0].key = 'AbC'
  assert.equal(sameMatchingContent(config(), changed), false)
  changed.exceptions[0].enabled = false
  assert.equal(configFrom(changed).exceptions[0].enabled, false)
  assert.match(validateMatchingDraft(config(), new Map()), /custom:relay/)
  assert.equal(validateMatchingDraft(config(), new Map([['custom:relay', {}]])), '')
  assert.equal(validateMatchingDraft({ ...config(), rules: [{ ...rule, priority: '' }] }).length > 0, true)
})

test('新增：标准UUID、无默认前缀推断、类型需明确选择', () => {
  const first = newMatchingEntry('rules'), second = newMatchingEntry('exceptions')
  assert.notEqual(first.id, second.id)
  assert.equal(first.id.length, 36)
  assert.equal(first.field, 'name')
  assert.equal(first.pattern, '')
  assert.equal(first.type_id, '')
  assert.equal(second.key, '')
  assert.equal('pattern' in second, false)
})

test('样本：缺失、显式空串、清空后空串与两个空格保留；sample:{}不切换数据库', () => {
  const sample = emptySample()
  assert.deepEqual({ sample: sampleBody(sample) }, { sample: {} })
  sample.reference.provided = true
  assert.deepEqual(sampleBody(sample), { reference: '' })
  sample.reference.text = 'R1'
  assert.deepEqual(sampleBody(sample), { reference: 'R1' })
  sample.reference.text = ''
  assert.deepEqual(sampleBody(sample), { reference: '' })
  sample.reference.text = '  '
  assert.deepEqual(sampleBody(sample), { reference: '  ' })
  sample.reference.provided = false
  assert.deepEqual(sampleBody(sample), {})
  sample.model = { provided: true, text: 'Part-42' }
  assert.deepEqual(sampleBody(sample), { model: 'Part-42' })
})

test('提交报告：保留partial实际commit数，坏shape/缺计数/超100明细不可信', () => {
  assert.equal(isMatchingReport(report()), true)
  assert.equal(report().summary.type_updated, 1)
  const missing = report(); delete missing.summary.source_changed
  for (const invalid of [null, { ok: true }, missing, { ...report(), summary: { ...counts, total: -1 } },
    { ...report(), details: [null] }, { ...report(), details: Array(101).fill(report().details[0]) }]) {
    assert.equal(isMatchingReport(invalid), false)
  }
  const sample = { ok: true, mode: 'sample', source: 'draft', revision: 2, sample: {}, status: 'unmatched',
    type_id: '', reason: 'no rule matched', structured_evidence: [], load_error: '', saved_healthy: true }
  assert.equal(isMatchingSamplePreview(sample), true)
  assert.equal(isMatchingSamplePreview({ ...sample, structured_evidence: [null] }), false)
  assert.equal(isMatchingSamplePreview({ ...sample, reason: {} }), false)
  assert.equal(isMatchingSamplePreview({ ...sample, sample: { reference: null } }), false)
  assert.equal(isMatchingReport({ ...report(), details: [{ ...report().details[0], structured_evidence: [null] }] }), false)
})

test('提交后刷新：allSettled全部published；失败、拒绝、null、坏shape、superseded不能报同步完成', () => {
  const settled = status => ({ status: 'fulfilled', value: { status } })
  const sources = ['元件', '统计', '类型']
  assert.equal(committedRefreshOutcome(Array(3).fill(settled('published')), sources).status, 'published')
  for (const invalid of [{ status: 'fulfilled', value: null }, { status: 'fulfilled', value: [] },
    { status: 'rejected', reason: new Error('offline') }, settled('failed')]) {
    const out = committedRefreshOutcome([settled('published'), invalid, settled('published')], sources)
    assert.equal(out.status, 'failed')
    assert.equal(out.details[1].source, '统计')
    assert.equal(!!out.details[1].error, true)
  }
  assert.equal(committedRefreshOutcome([settled('published'), settled('superseded'), settled('published')], sources).status, 'superseded')
})

test('实际API包装：非2xx结构化payload、HTTP空体、网络和JSON解析分类保持', async () => {
  const original = globalThis.fetch
  setLoadingCallback(null)
  try {
    globalThis.fetch = async () => ({ ok: false, status: 500, json: async () => report() })
    await assert.rejects(api.applyTypeMatching({ expected_revision: 2 }), error => {
      assert.equal(error.kind, 'http'); assert.equal(error.status, 500)
      assert.equal(error.detail, 'write stopped'); assert.deepEqual(error.payload, report())
      return true
    })
    globalThis.fetch = async () => ({ ok: false, status: 400, json: async () => { throw new Error('no json') } })
    await assert.rejects(api.typeMatching(), error => error.kind === 'http' && error.message === 'HTTP 400')
    globalThis.fetch = async () => { throw new Error('offline') }
    await assert.rejects(api.typeMatching(), error => error.kind === 'network')
    globalThis.fetch = async () => ({ ok: true, json: async () => { throw new Error('broken json') } })
    await assert.rejects(api.typeMatching(), error => error.kind === 'parse')
  } finally { globalThis.fetch = original }
})

test('实际API请求：save完整DTO、apply仅版本、sample键存在、reload只空对象', async () => {
  const original = globalThis.fetch, calls = []
  try {
    globalThis.fetch = async (url, init = {}) => {
      calls.push({ url, init, body: init.body ? JSON.parse(init.body) : undefined })
      return { ok: true, json: async () => ({ ok: true }) }
    }
    await api.saveTypeMatching({ config: config(), expected_revision: 2 })
    await api.applyTypeMatching({ expected_revision: 2 })
    await api.previewTypeMatching({ config: config(), sample: {} })
    await api.previewTypeMatching({ config: config() })
    await api.reloadTypeMatching()
    assert.deepEqual(calls[0].body, { config: config(), expected_revision: 2 })
    assert.deepEqual(calls[1].body, { expected_revision: 2 })
    assert.deepEqual(calls[2].body.sample, {})
    assert.equal('sample' in calls[3].body, false)
    assert.deepEqual(calls[4].body, {})
    assert.equal(calls[1].url, '/api/component-type-matching/apply')
  } finally { globalThis.fetch = original }
})

test('runner：type-matching独立显式模式、互斥、非0结果严格校验', () => {
  assert.equal(resolveMode(['--type-matching'], { KFORGE_LCSC_MODE: 'legacy' }), 'type-matching')
  for (const args of [['--type-matching', '--legacy'], ['--type-matching', '--types']]) {
    assert.throws(() => resolveMode(args, {}))
  }
  assert.throws(() => validateOutcome({ mode: 'type-matching', results: [] }, 'type-matching'))
  assert.throws(() => validateOutcome({ mode: 'types', results: [{ name: 'x', ok: true }] }, 'type-matching'))
  assert.throws(() => validateOutcome({ mode: 'type-matching', results: [null] }, 'type-matching'))
  assert.equal(validateOutcome({ mode: 'type-matching', results: [{ name: '真实组件', ok: true }] }, 'type-matching').length, 1)
})


test('匹配翻译：已有消息按当前语言渲染，后端诊断与用户原文保持原样', async () => {
  const resources = Object.fromEntries(['zh', 'en'].map(language => [language, { translation:
    JSON.parse(readFileSync(new URL(`../../src/locales/${language}.json`, import.meta.url), 'utf8')) }]))
  const local = i18next.createInstance()
  await local.init({ resources, lng: 'zh', fallbackLng: false, interpolation: { escapeValue: false } })
  const notice = matchingMessage('notices.appliedRefreshing', { count: 500 })
  const validation = validateMatchingDraft({ ...config(), rules: [{ ...rule, priority: '' }] }, undefined, true)
  assert.equal(translateMatchingMessage(notice, local.t), '写入已结束，实际更新 500 个元件，正在刷新界面。')
  assert.match(translateMatchingMessage(validation, local.t), /int32/)
  await local.changeLanguage('en')
  assert.equal(translateMatchingMessage(notice, local.t), 'Writing finished: 500 components updated. Refreshing views…')
  assert.equal(translateMatchingMessage(validation, local.t), 'Invalid regex pattern or priority (priority must be an int32 integer)')
  assert.equal(translateMatchingMessage('后端原文:  ^AbC  ', local.t), '后端原文:  ^AbC  ')
  assert.equal(matchingErrorMessage(new Error('backend reason')), 'backend reason')
  const missing = validateMatchingDraft(config(), new Map(), true)
  assert.equal(translateMatchingMessage(missing, local.t), 'Target type does not exist: custom:relay')
  assert.equal(configFrom(config()).exceptions[0].key, '  AbC  ')
})

test('匹配翻译：中英完整键一致，首次坏加载与刷新兜底消息可翻译', () => {
  const locales = ['zh', 'en'].map(language => JSON.parse(readFileSync(new URL(`../../src/locales/${language}.json`, import.meta.url), 'utf8')))
  const keys = (value, prefix = '') => Object.entries(value).flatMap(([key, child]) =>
    typeof child === 'object' ? keys(child, `${prefix}${key}.`) : [`${prefix}${key}`]).sort()
  assert.ok(locales[0].matching && locales[1].matching, '两种语言都必须提供匹配界面文案')
  assert.deepEqual(keys(locales[0].matching), keys(locales[1].matching))
  assert.deepEqual(keys(locales[0].classifying), keys(locales[1].classifying))
  const noSnapshot = parseMatchingRead({ ok: false, has_snapshot: false, load_error: '' })
  assert.equal(noSnapshot.errorMessage.key, 'matching.errors.noConfig')
  const outcome = committedRefreshOutcome([{ status: 'fulfilled', value: null }], ['symbols'])
  assert.equal(outcome.details[0].error, '读取未能发布', '旧字符串契约保留')
  assert.equal(outcome.details[0].errorMessage.key, 'matching.errors.unpublished')
})
