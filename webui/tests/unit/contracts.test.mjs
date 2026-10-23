import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  compareVersions,
  createContractRegistry,
  satisfiesRange,
  ContractError,
} from '../../src/plugin-host/contracts.js'

function registryWithLog() {
  const reports = []
  const registry = createContractRegistry({ report: (item) => reports.push(item) })
  return { registry, reports }
}

test('version ranges cover caret, tilde and comparators', () => {
  assert.equal(compareVersions('1.0.0', '1.0.0-rc.1'), 1)
  assert.equal(satisfiesRange('1.2.3', '^1.0.0'), true)
  assert.equal(satisfiesRange('2.0.0', '^1.0.0'), false)
  assert.equal(satisfiesRange('1.2.9', '~1.2.0'), true)
  assert.equal(satisfiesRange('1.3.0', '~1.2.0'), false)
  assert.equal(satisfiesRange('1.0.0-rc.2', '>=1.0.0-rc.1 <2.0.0'), true)
  assert.equal(satisfiesRange('1.0.0', '>=1.0.0-rc.1 <2.0.0'), true)
})

test('provider lifetime and compatible consumer', () => {
  const { registry } = registryWithLog()
  const impl = { query: () => 42 }
  const dispose = registry.provide({ id: 'a' }, 'demo.supplier', '1.0.0', impl)
  assert.equal(registry.require({ id: 'b' }, 'demo.supplier', '^1.0.0'), impl)
  dispose()
  assert.equal(registry.get('demo.supplier'), undefined)
})

test('duplicate provider is diagnostics-scoped and keeps the first provider', () => {
  const { registry, reports } = registryWithLog()
  const first = { id: 'first' }
  const second = { id: 'second' }
  registry.provide(first, 'demo.supplier', '1.0.0', { owner: 'first' })
  assert.throws(
    () => registry.provide(second, 'demo.supplier', '1.0.0', { owner: 'second' }),
    (error) => error instanceof ContractError && error.code === 'contract/duplicate-provider',
  )
  assert.deepEqual(registry.require({ id: 'consumer' }, 'demo.supplier', '^1.0.0'), { owner: 'first' })
  assert.equal(reports.length, 1)
  assert.equal(reports[0].code, 'contract/duplicate-provider')
  assert.equal(reports[0].detail.existing, 'first')
  assert.equal(reports[0].detail.attempted, 'second')
})

test('same plugin may replace its own provider on restart', () => {
  const { registry } = registryWithLog()
  const identity = { id: 'a' }
  registry.provide(identity, 'demo.supplier', '1.0.0', { generation: 1 })
  registry.provide(identity, 'demo.supplier', '1.0.0', { generation: 2 })
  assert.deepEqual(registry.require({ id: 'b' }, 'demo.supplier', '^1.0.0'), { generation: 2 })
})

test('incompatible consumer fails without affecting provider or other consumers', () => {
  const { registry, reports } = registryWithLog()
  registry.provide({ id: 'provider' }, 'demo.supplier', '1.0.0', { owner: 'provider' })
  assert.throws(
    () => registry.require({ id: 'consumer' }, 'demo.supplier', '^2.0.0'),
    (error) => error instanceof ContractError && error.code === 'contract/version-incompatible',
  )
  assert.deepEqual(registry.require({ id: 'other' }, 'demo.supplier', '^1.0.0'), { owner: 'provider' })
  assert.equal(reports[0].code, 'contract/version-incompatible')
  assert.equal(reports[0].detail.version, '1.0.0')
})

test('missing provider is reported and throws', () => {
  const { registry, reports } = registryWithLog()
  assert.throws(
    () => registry.require({ id: 'consumer' }, 'missing.contract'),
    (error) => error instanceof ContractError && error.code === 'contract/missing-provider',
  )
  assert.equal(reports[0].code, 'contract/missing-provider')
})
