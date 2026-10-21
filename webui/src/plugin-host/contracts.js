// Host-side contract registry: versioned service metadata and duplicate
// provider diagnostics, layered on top of Cordis service injection.
//
// Cordis still owns activation order through `inject`; this registry only
// validates that a consumer's required version is actually available and that
// two plugins cannot silently provide the same contract.

function parseVersion(value) {
  const match = /^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/.exec(String(value).trim())
  if (!match) return null
  return {
    major: Number(match[1]),
    minor: Number(match[2]),
    patch: Number(match[3]),
    prerelease: match[4] ? match[4].split('.') : [],
  }
}

function comparePrerelease(a, b) {
  if (!a.length && !b.length) return 0
  if (!a.length) return 1
  if (!b.length) return -1
  for (let index = 0; index < Math.max(a.length, b.length); index += 1) {
    if (a[index] === undefined) return -1
    if (b[index] === undefined) return 1
    const an = /^\d+$/.test(a[index]) ? Number(a[index]) : a[index]
    const bn = /^\d+$/.test(b[index]) ? Number(b[index]) : b[index]
    if (typeof an === 'number' && typeof bn === 'number') {
      if (an !== bn) return an < bn ? -1 : 1
    } else if (an !== bn) {
      return String(an) < String(bn) ? -1 : 1
    }
  }
  return 0
}

export function compareVersions(left, right) {
  const a = parseVersion(left)
  const b = parseVersion(right)
  if (!a || !b) throw new Error(`Invalid version: ${left} / ${right}`)
  for (const key of ['major', 'minor', 'patch']) {
    if (a[key] !== b[key]) return a[key] < b[key] ? -1 : 1
  }
  return comparePrerelease(a.prerelease, b.prerelease)
}

function upperBoundForCaret(version) {
  const parsed = parseVersion(version)
  if (!parsed) return null
  if (parsed.major > 0) return `${parsed.major + 1}.0.0-0`
  if (parsed.minor > 0) return `0.${parsed.minor + 1}.0-0`
  return `0.0.${parsed.patch + 1}-0`
}

function upperBoundForTilde(version) {
  const parsed = parseVersion(version)
  if (!parsed) return null
  return `${parsed.major}.${parsed.minor + 1}.0-0`
}

export function satisfiesRange(version, range) {
  const text = String(range ?? '*').trim()
  if (!text || text === '*' || text === 'latest') return true
  for (const comparator of text.split(/\s+/).filter(Boolean)) {
    if (comparator.startsWith('^')) {
      const lower = comparator.slice(1)
      const upper = upperBoundForCaret(lower)
      if (compareVersions(version, lower) < 0) return false
      if (upper && compareVersions(version, upper) >= 0) return false
      continue
    }
    if (comparator.startsWith('~')) {
      const lower = comparator.slice(1)
      const upper = upperBoundForTilde(lower)
      if (compareVersions(version, lower) < 0) return false
      if (upper && compareVersions(version, upper) >= 0) return false
      continue
    }
    const match = /^(>=|<=|>|<|=)?(.+)$/.exec(comparator)
    if (!match) return false
    const operator = match[1] ?? '='
    const target = match[2]
    const order = compareVersions(version, target)
    if (operator === '=' && order !== 0) return false
    if (operator === '>=' && order < 0) return false
    if (operator === '<=' && order > 0) return false
    if (operator === '>' && order <= 0) return false
    if (operator === '<' && order >= 0) return false
  }
  return true
}

export class ContractError extends Error {
  constructor(code, message, detail) {
    super(message)
    this.name = 'ContractError'
    this.code = code
    this.detail = detail
  }
}

export function createContractRegistry({ report = () => {}, onChange = () => {} } = {}) {
  const providers = new Map()
  const listeners = new Set()

  function emit(change) {
    onChange(change)
    for (const listener of [...listeners]) listener(change)
  }

  function provide(identity, contract, version, implementation) {
    if (typeof contract !== 'string' || !contract) {
      throw new ContractError('contract/invalid', 'Contract name must be a non-empty string')
    }
    if (!parseVersion(version)) {
      throw new ContractError('contract/invalid-version', `Invalid contract version: ${version}`)
    }
    const existing = providers.get(contract)
    if (existing && existing.pluginId !== identity.id) {
      const reportItem = {
        code: 'contract/duplicate-provider',
        severity: 'error',
        message: `${identity.id} cannot provide ${contract}: already provided by ${existing.pluginId}`,
        detail: { contract, existing: existing.pluginId, attempted: identity.id },
      }
      report(reportItem)
      throw new ContractError(reportItem.code, reportItem.message, reportItem.detail)
    }
    if (existing) existing.dispose()
    let live = true
    const record = {
      contract,
      version,
      pluginId: identity.id,
      implementation,
      dispose() {
        if (!live) return
        live = false
        if (providers.get(contract) === record) {
          providers.delete(contract)
          emit({ type: 'dispose', contract, pluginId: identity.id, version })
        }
      },
    }
    providers.set(contract, record)
    emit({ type: 'provide', contract, pluginId: identity.id, version })
    return () => record.dispose()
  }

  function get(contract) {
    return providers.get(contract)?.implementation
  }

  function resolve(contract) {
    const record = providers.get(contract)
    return record
      ? { version: record.version, pluginId: record.pluginId, implementation: record.implementation }
      : null
  }

  function subscribe(listener) {
    listeners.add(listener)
    return () => listeners.delete(listener)
  }

  function requireContract(identity, contract, range = '*') {
    const record = providers.get(contract)
    if (!record) {
      const message = `${identity.id} requires ${contract}, but no provider is active`
      report({
        code: 'contract/missing-provider',
        severity: 'error',
        message,
        detail: { contract, range, plugin: identity.id },
      })
      throw new ContractError('contract/missing-provider', message, { contract })
    }
    if (!satisfiesRange(record.version, range)) {
      const message = `${identity.id} requires ${contract}@${range}, but ${record.pluginId} provides ${record.version}`
      report({
        code: 'contract/version-incompatible',
        severity: 'error',
        message,
        detail: {
          contract,
          range,
          requiredBy: identity.id,
          provider: record.pluginId,
          version: record.version,
        },
      })
      throw new ContractError('contract/version-incompatible', message, {
        contract,
        range,
        version: record.version,
      })
    }
    return record.implementation
  }

  return {
    provide,
    get,
    resolve,
    subscribe,
    require: requireContract,
    list: () =>
      [...providers.values()].map(({ contract, version, pluginId }) => ({
        contract,
        version,
        pluginId,
      })),
  }
}
