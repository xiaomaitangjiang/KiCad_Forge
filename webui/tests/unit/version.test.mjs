import test from 'node:test'
import assert from 'node:assert/strict'
import { compareVersions, normalizeVersion } from '../../src/lib/version.js'

test('normalizeVersion accepts v-prefixed and optional suffixes', () => {
  assert.deepEqual(normalizeVersion('v1.2.3'), [1, 2, 3])
  assert.deepEqual(normalizeVersion('1.2.3-rc.1'), [1, 2, 3])
  assert.deepEqual(normalizeVersion('1.2.3+build.7'), [1, 2, 3])
  assert.equal(normalizeVersion('1.2'), null)
  assert.equal(normalizeVersion('latest'), null)
  assert.equal(normalizeVersion(undefined), null)
})

test('compareVersions orders numeric parts and ignores tag prefix', () => {
  assert.equal(compareVersions('v1.2.3', '1.2.3'), 0)
  assert.equal(compareVersions('1.10.0', '1.9.9'), 1)
  assert.equal(compareVersions('0.1.0', 'v0.1.1'), -1)
  assert.equal(compareVersions('invalid', '1.0.0'), 0)
})
