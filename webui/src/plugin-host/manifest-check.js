// Pure manifest/consistency checks used before a plugin bundle is activated.
// Kept separate from the loader so version and contract errors are testable
// without a DOM or dynamic import.

import { satisfiesRange } from './contracts.js'

export function checkManifestCompatibility({ descriptor, sdkVersion, identity }) {
  const sdkRange = descriptor?.engines?.sdk
  if (sdkRange && !satisfiesRange(sdkVersion, sdkRange)) {
    return {
      ok: false,
      code: 'manifest/unsupported-sdk',
      message: `${identity.id} requires SDK ${sdkRange}, host SDK is ${sdkVersion}`,
      detail: { plugin: identity.id, range: sdkRange, sdk: sdkVersion },
    }
  }
  return { ok: true }
}

/**
 * Inspect manifest contract requirements against the live registry. Missing
 * providers mean the consumer must wait (not fail); present-but-incompatible
 * providers are hard errors for this plugin only.
 */
export function inspectContractRequirements({ descriptor, contracts, identity }) {
  const missing = []
  const incompatible = []
  const perPluginOnly = ['kiforge.tasks', 'kiforge.imports']
  for (const requirement of descriptor?.frontend?.requires ?? []) {
    if (perPluginOnly.includes(requirement.contract)) {
      missing.push({ ...requirement, perPluginOnly: true })
      continue
    }
    const provider = contracts.resolve(requirement.contract)
    if (!provider) {
      missing.push(requirement)
    } else if (!satisfiesRange(provider.version, requirement.range)) {
      incompatible.push({ ...requirement, provider })
    }
  }
  return { missing, incompatible }
}

/**
 * Order plugin records so a contract provider activates before its consumers,
 * regardless of the host API's array order. Missing providers are left for the
 * activation check to diagnose; a cycle is reported and the remaining records
 * keep their original order.
 */
export function orderPluginsByContracts(records, report = () => {}) {
  const providerByContract = new Map()
  for (const record of records) {
    for (const provided of record.descriptor?.frontend?.provides ?? []) {
      providerByContract.set(provided.contract, record)
    }
  }

  const ordered = []
  const visited = new Set()
  const visiting = new Set()

  function visit(record) {
    if (visited.has(record)) return
    if (visiting.has(record)) {
      report({
        code: 'contract/dependency-cycle',
        severity: 'error',
        message: `contract dependency cycle at ${record.id}`,
        detail: { plugin: record.id },
      })
      return
    }
    visiting.add(record)
    for (const requirement of record.descriptor?.frontend?.requires ?? []) {
      const provider = providerByContract.get(requirement.contract)
      if (provider && provider !== record) visit(provider)
    }
    visiting.delete(record)
    visited.add(record)
    ordered.push(record)
  }

  for (const record of records) visit(record)
  return ordered
}
