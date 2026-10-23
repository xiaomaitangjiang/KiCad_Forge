import test from 'node:test'
import assert from 'node:assert/strict'
import { detectLanguage, translate } from '../../../plugins/git_version/frontend/src/i18n-text.ts'

function withDocumentLanguage(lang, run) {
  const previousDocument = globalThis.document
  const previousWindow = globalThis.window
  globalThis.document = { documentElement: { getAttribute: () => lang } }
  globalThis.window = previousWindow
  try { return run() } finally {
    if (previousDocument === undefined) delete globalThis.document
    else globalThis.document = previousDocument
  }
}

test('Git plugin translations follow the document language', () => {
  withDocumentLanguage('zh-CN', () => {
    assert.equal(detectLanguage(), 'zh')
    assert.equal(translate('提交', 'Commit'), '提交')
    assert.equal(translate('{n} 项更改', '{n} changed', { n: 2 }), '2 项更改')
  })
  withDocumentLanguage('en', () => {
    assert.equal(detectLanguage(), 'en')
    assert.equal(translate('提交', 'Commit'), 'Commit')
    assert.equal(translate('{n} 项更改', '{n} changed', { n: 2 }), '2 changed')
  })
})

test('Git plugin translation placeholders are left intact when missing', () => {
  withDocumentLanguage('en', () => {
    assert.equal(translate('{oid}', '{oid}'), '{oid}')
    assert.equal(translate('x {n}', 'x {n}', {}), 'x {n}')
  })
})
