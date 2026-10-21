import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
const read = (name) => JSON.parse(readFileSync(new URL(`../src/locales/${name}.json`, import.meta.url), 'utf8'))

function flatten(value, prefix = '', out = new Map()) {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    for (const [key, child] of Object.entries(value)) flatten(child, prefix ? `${prefix}.${key}` : key, out)
  } else {
    out.set(prefix, value)
  }
  return out
}

const zh = flatten(read('zh'))
const en = flatten(read('en'))

test('zh/en locale key sets stay in sync', () => {
  const missingInEn = [...zh.keys()].filter((key) => !en.has(key))
  const missingInZh = [...en.keys()].filter((key) => !zh.has(key))
  assert.deepEqual(missingInEn, [], 'keys present in zh but missing in en')
  assert.deepEqual(missingInZh, [], 'keys present in en but missing in zh')
})

test('locale values are non-empty strings with identical interpolation placeholders', () => {
  const placeholders = (text) => [...String(text).matchAll(/{{\s*([^}]+?)\s*}}/g)].map((match) => match[1]).sort()
  for (const [key, zhValue] of zh) {
    const enValue = en.get(key)
    assert.equal(typeof zhValue, 'string', `${key} must be a string in zh`)
    assert.equal(typeof enValue, 'string', `${key} must be a string in en`)
    assert.ok(zhValue.trim().length > 0, `${key} must not be empty in zh`)
    assert.ok(enValue.trim().length > 0, `${key} must not be empty in en`)
    assert.deepEqual(placeholders(enValue), placeholders(zhValue), `${key} placeholders differ between zh and en`)
  }
})

test('locales do not leave common development markers in user-facing text', () => {
  for (const [name, table] of [['zh', zh], ['en', en]]) {
    for (const [key, value] of table) {
      assert.doesNotMatch(String(value), /\bTODO\b|\bFIXME\b|\uFFFD/, `${name}:${key} looks unfinished`)
    }
  }
})
