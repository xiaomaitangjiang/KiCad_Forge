import test from 'node:test'
import assert from 'node:assert/strict'
import { describeEditRefresh, selectableCommitIds, EDIT_REFRESH_SOURCES } from '../src/lib/editRefresh.js'

const published = (data) => ({ status: 'fulfilled', value: { status: 'published', data } })
const failed = (message) => ({ status: 'fulfilled', value: { status: 'failed', error: message } })
const superseded = () => ({ status: 'fulfilled', value: { status: 'superseded' } })

test('edit refresh reports success only when every source published', () => {
  const report = describeEditRefresh([
    published([]), published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'published')
  assert.deepEqual(report.failedSources, [])
  assert.equal(report.error, '')
  assert.equal(report.details.length, EDIT_REFRESH_SOURCES.length)
})

test('edit refresh keeps failed sources and messages for the retry notice', () => {
  const report = describeEditRefresh([
    published([{ id: 's1', library_id: 'lib-a' }]),
    failed('HTTP 503'),
    published({}),
    failed('history unavailable'),
  ])
  assert.equal(report.status, 'failed')
  assert.deepEqual(report.failedSources, ['libraries', 'history'])
  assert.match(report.error, /HTTP 503/)
  assert.match(report.error, /history unavailable/)
})

test('edit refresh treats superseded reads as not failed', () => {
  const report = describeEditRefresh([
    superseded(), published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'superseded')
  assert.deepEqual(report.failedSources, [])
  assert.deepEqual(report.supersededSources, ['symbols'])
})

test('edit refresh treats a rejected read as a failed source', () => {
  const report = describeEditRefresh([
    { status: 'rejected', reason: Object.assign(new Error('network down'), { kind: 'network' }) },
    published([]), published({}), published({}),
  ])
  assert.equal(report.status, 'failed')
  assert.deepEqual(report.failedSources, ['symbols'])
  assert.match(report.error, /network down/)
})

test('selectableCommitIds keeps only UUIDs published in the target library', () => {
  const rows = [
    { id: 'a', library_id: 'lib-a' },
    { id: 'b', library_id: 'lib-b' },
    { id: 'c', library_id: 'lib-a' },
  ]
  assert.deepEqual(selectableCommitIds(rows, ['a', 'b', 'missing', 'a'], 'lib-a'), {
    selected: ['a'], missing: ['b', 'missing'],
  })
  assert.deepEqual(selectableCommitIds(rows, ['a', 'b'], ''), {
    selected: ['a', 'b'], missing: [],
  })
})

test('selectableCommitIds does not trust malformed rows or ids', () => {
  const rows = [null, { id: '' }, { id: 7 }, { id: 'ok', library_id: 'lib-a' }]
  assert.deepEqual(selectableCommitIds(rows, ['ok', '', null, 'ok'], 'lib-a'), {
    selected: ['ok'], missing: [],
  })
  assert.deepEqual(selectableCommitIds(null, ['a']), { selected: [], missing: ['a'] })
})
