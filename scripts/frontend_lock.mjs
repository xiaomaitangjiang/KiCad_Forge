#!/usr/bin/env node
// Lock and verify the frontend framework dependencies (Cordis, SlotCore and
// their runtime dependencies). Run with --generate after npm ci, --verify in CI.
//
//   node scripts/frontend_lock.mjs --generate
//   node scripts/frontend_lock.mjs --verify

import { createHash } from 'node:crypto'
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(here, '..')
const packageJsonPath = path.join(repoRoot, 'webui', 'package.json')
const packageLockPath = path.join(repoRoot, 'webui', 'package-lock.json')
const lockPath = path.join(repoRoot, 'dependencies', 'frontend.lock.json')
const licenseDir = path.join(repoRoot, 'resources', 'licenses')

const PACKAGES = [
  { name: '@deepseek-ai/cordis', file: 'cordis' },
  { name: '@deepseek-ai/dsh-client-ui-slots', file: 'dsh-client-ui-slots' },
  { name: '@deepseek-ai/cosmokit', file: 'cosmokit' },
  { name: '@standard-schema/spec', file: 'standard-schema-spec' },
  { name: 'react-resizable-panels', file: 'react-resizable-panels', licenseFile: 'LICENSE.md' },
]

const mode = process.argv.includes('--generate') ? 'generate' : 'verify'
if (!process.argv.includes('--generate') && !process.argv.includes('--verify')) {
  console.error('usage: node scripts/frontend_lock.mjs --generate|--verify')
  process.exit(2)
}

function sha256(file) {
  return createHash('sha256').update(readFileSync(file)).digest('hex')
}

function readJson(file) {
  return JSON.parse(readFileSync(file, 'utf8'))
}

const pkg = readJson(packageJsonPath)
const packageLock = readJson(packageLockPath)
const entryPoint = (name) => `node_modules/${name}`

function packageInfo(name) {
  const key = entryPoint(name)
  const entry = packageLock.packages?.[key]
  if (!entry) throw new Error(`package-lock is missing ${key}`)
  return {
    version: entry.version,
    resolved: entry.resolved,
    integrity: entry.integrity,
    license: entry.license,
  }
}

function licenseSource(name, file = 'LICENSE') {
  return path.join(repoRoot, 'webui', 'node_modules', ...name.split('/'), file)
}

function expectedLock() {
  const packages = {}
  for (const item of PACKAGES) {
    const info = packageInfo(item.name)
    const source = licenseSource(item.name, item.licenseFile)
    if (!existsSync(source)) throw new Error(`missing license file: ${source}`)
    const licenseFile = path.join('resources', 'licenses', `${item.file}-LICENSE`)
    packages[item.name] = {
      ...info,
      license_file: licenseFile.split(path.sep).join('/'),
      license_sha256: sha256(source),
    }
  }
  return {
    source: 'webui/package-lock.json',
    packages,
  }
}

if (mode === 'generate') {
  const lock = expectedLock()
  mkdirSync(licenseDir, { recursive: true })
  for (const item of PACKAGES) {
    copyFileSync(licenseSource(item.name, item.licenseFile), path.join(licenseDir, `${item.file}-LICENSE`))
  }
  lock.generated_at = new Date().toISOString()
  writeFileSync(lockPath, JSON.stringify(lock, null, 2) + '\n', 'utf8')
  console.log(`frontend lock generated: ${path.relative(repoRoot, lockPath)}`)
} else {
  if (!existsSync(lockPath)) throw new Error('dependencies/frontend.lock.json is missing')
  const lock = readJson(lockPath)
  const expected = expectedLock()
  for (const item of PACKAGES) {
    const actual = lock.packages?.[item.name]
    const want = expected.packages[item.name]
    if (!actual) throw new Error(`lock is missing ${item.name}`)
    for (const field of ['version', 'resolved', 'integrity', 'license']) {
      if (actual[field] !== want[field]) {
        throw new Error(`${item.name}.${field}: lock=${actual[field]} package-lock=${want[field]}`)
      }
    }
    const licensePath = path.join(repoRoot, actual.license_file)
    if (!existsSync(licensePath)) throw new Error(`license file missing: ${licensePath}`)
    const hash = sha256(licensePath)
    if (hash !== actual.license_sha256) {
      throw new Error(`${item.name} license hash mismatch: ${hash} != ${actual.license_sha256}`)
    }
  }
  // Exact versions are intentionally pinned in package.json for direct deps.
  for (const item of PACKAGES.filter(
    (entry) =>
      entry.name === '@deepseek-ai/cordis' ||
      entry.name === '@deepseek-ai/dsh-client-ui-slots' ||
      entry.name === 'react-resizable-panels',
  )) {
    const declared = pkg.dependencies?.[item.name]
    if (declared !== lock.packages[item.name].version) {
      throw new Error(`${item.name}: package.json=${declared} lock=${lock.packages[item.name].version}`)
    }
  }
  console.log('frontend lock verified')
}
