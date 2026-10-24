import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createContractRegistry } from '../../src/plugin-host/contracts.js'
import {
  checkManifestCompatibility,
  inspectContractRequirements,
  orderPluginsByContracts,
} from '../../src/plugin-host/manifest-check.js'

const SDK = '1.0.0'

function setup() {
  const reports = []
  const contracts = createContractRegistry({ report: (item) => reports.push(item) })
  contracts.provide({ id: 'provider' }, 'demo.supplier', '1.0.0', { ok: true })
  return { contracts, reports }
}

test('compatible manifest passes SDK and contract checks', () => {
  const { contracts } = setup()
  const descriptor = {
    engines: { sdk: '^1.0.0' },
    frontend: { requires: [{ contract: 'demo.supplier', range: '^1.0.0' }] },
  }
  const result = checkManifestCompatibility({
    descriptor,
    sdkVersion: SDK,
    identity: { id: 'consumer' },
  })
  assert.equal(result.ok, true)
  assert.deepEqual(
    inspectContractRequirements({ descriptor, contracts, identity: { id: 'consumer' } }),
    { missing: [], incompatible: [] },
  )
})

test('incompatible SDK is rejected for this plugin only', () => {
  const { contracts } = setup()
  const result = checkManifestCompatibility({
    descriptor: { engines: { sdk: '^2.0.0' } },
    sdkVersion: SDK,
    identity: { id: 'future-plugin' },
  })
  assert.equal(result.ok, false)
  assert.equal(result.code, 'manifest/unsupported-sdk')
  assert.deepEqual(contracts.require({ id: 'unrelated' }, 'demo.supplier', '^1.0.0'), { ok: true })
})

test('incompatible contract version is reported without affecting others', () => {
  const { contracts } = setup()
  const inspection = inspectContractRequirements({
    descriptor: { frontend: { requires: [{ contract: 'demo.supplier', range: '^2.0.0' }] } },
    contracts,
    identity: { id: 'consumer' },
  })
  assert.equal(inspection.missing.length, 0)
  assert.equal(inspection.incompatible.length, 1)
  assert.equal(inspection.incompatible[0].provider.version, '1.0.0')
  assert.deepEqual(contracts.require({ id: 'other' }, 'demo.supplier', '^1.0.0'), { ok: true })
})

test('missing required contract waits instead of failing', () => {
  const { contracts } = setup()
  const inspection = inspectContractRequirements({
    descriptor: { frontend: { requires: [{ contract: 'missing.contract', range: '*' }] } },
    contracts,
    identity: { id: 'consumer' },
  })
  assert.equal(inspection.missing.length, 1)
  assert.equal(inspection.incompatible.length, 0)
})

test('per-plugin tasks/imports cannot be declared as manifest contracts', () => {
  const { contracts } = setup()
  const inspection = inspectContractRequirements({
    descriptor: {
      frontend: { requires: [{ contract: 'kiforge.tasks', range: '^1.0.0' }] },
    },
    contracts,
    identity: { id: 'consumer' },
  })
  assert.equal(inspection.missing.length, 1)
  assert.equal(inspection.missing[0].perPluginOnly, true)
})

test('contract providers activate before consumers regardless of API order', () => {
  const reports = []
  const records = [
    { id: 'page', descriptor: { frontend: { requires: [{ contract: 'demo.supplier', range: '^1.0.0' }] } } },
    { id: 'supplier', descriptor: { frontend: { provides: [{ contract: 'demo.supplier', version: '1.0.0' }] } } },
    { id: 'tab', descriptor: { frontend: { requires: [{ contract: 'demo.supplier', range: '^1.0.0' }] } } },
  ]
  const ordered = orderPluginsByContracts(records, (item) => reports.push(item))
  assert.deepEqual(ordered.map((record) => record.id), ['supplier', 'page', 'tab'])
  assert.equal(reports.length, 0)
})

test('contract dependency cycle is reported and does not drop plugins', () => {
  const reports = []
  const records = [
    { id: 'a', descriptor: { frontend: { provides: [{ contract: 'a' }], requires: [{ contract: 'b' }] } } },
    { id: 'b', descriptor: { frontend: { provides: [{ contract: 'b' }], requires: [{ contract: 'a' }] } } },
  ]
  const ordered = orderPluginsByContracts(records, (item) => reports.push(item))
  assert.equal(ordered.length, 2)
  assert.equal(reports[0].code, 'contract/dependency-cycle')
})
