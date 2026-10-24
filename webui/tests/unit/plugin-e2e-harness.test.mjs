import assert from 'node:assert/strict'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import test from 'node:test'
import vm from 'node:vm'
import { copyRuntime, seedFixtureLanguage } from '../support/plugin-e2e-harness.mjs'

test('app fixture seeds its Chinese scenario language and preserves an explicit preference', async () => {
  for (const saved of [null, 'en', 'zh']) {
    const storage = new Map(saved ? [['kf-lang', saved]] : [])
    const document = { documentElement: { lang: saved === 'zh' ? 'zh-CN' : 'en' } }
    let reloads = 0
    const cdp = {
      async eval(source) {
        return vm.runInNewContext(source, { document, localStorage: { getItem: (key) => storage.get(key) ?? null, setItem: (key, value) => storage.set(key, value) } })
      },
      async send(method) {
        assert.equal(method, 'Page.reload')
        reloads++
        document.documentElement.lang = storage.get('kf-lang') === 'zh' ? 'zh-CN' : 'en'
      },
    }
    await seedFixtureLanguage(cdp)
    assert.equal(storage.get('kf-lang'), saved ?? 'zh')
    assert.equal(reloads, saved === null ? 1 : 0)
    await seedFixtureLanguage(cdp)
    assert.equal(reloads, saved === null ? 1 : 0, 'the same document must not reload twice')
  }
})

function fixture(t) {
  const root = mkdtempSync(path.join(os.tmpdir(), 'kf-e2e-copy-test-'))
  t.after(() => rmSync(root, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 }))
  const source = path.join(root, 'source')
  const target = path.join(root, 'target')
  mkdirSync(path.join(source, 'data'), { recursive: true })
  mkdirSync(target)
  const write = (relative, text = 'fixture') => {
    const destination = path.join(source, relative)
    mkdirSync(path.dirname(destination), { recursive: true })
    writeFileSync(destination, text)
  }
  write('KiCad_Forge.exe', 'not an executable; never started')
  return { root, source, target, write }
}

test('app fixture copies only runtime assets and creates its own empty data', (t) => {
  const { source, target, write } = fixture(t)
  for (const item of ['libstdc++-6.dll', 'webui/index.html', 'plugins/demo/manifest.json', 'plugins/demo/frontend/dist/index.js', 'config/types.json', 'licenses/MIT.txt']) write(item)
  for (const directory of ['data', 'node_modules', 'fetched', 'staging', '.git', '__pycache__']) write(`plugins/demo/${directory}/private.txt`, 'must not be copied')
  write('plugins/demo/frontend/node_modules/private.txt', 'must not be copied')
  write('unrelated.exe')
  write('meta.db', 'must not be copied')
  write('private/archive.txt', 'must not be copied')
  copyRuntime(source, target)
  assert.deepEqual(readdirSync(path.join(target, 'data')), [])
  assert.equal(readFileSync(path.join(target, 'KiCad_Forge.exe'), 'utf8'), 'not an executable; never started')
  assert.equal(readFileSync(path.join(target, 'plugins/demo/frontend/dist/index.js'), 'utf8'), 'fixture')
  assert.equal(existsSync(path.join(target, 'libstdc++-6.dll')), true)
  for (const directory of ['data', 'node_modules', 'fetched', 'staging', '.git', '__pycache__']) assert.equal(existsSync(path.join(target, 'plugins/demo', directory)), false)
  assert.equal(existsSync(path.join(target, 'plugins/demo/frontend/node_modules')), false)
  for (const item of ['unrelated.exe', 'meta.db', 'private']) assert.equal(existsSync(path.join(target, item)), false)
})

test('app fixture refuses a source containing user data before copying', (t) => {
  const { source, target, write } = fixture(t)
  write('data/meta.db', 'private database placeholder')
  assert.throws(() => copyRuntime(source, target), /refusing app source with non-empty data/)
  assert.deepEqual(readdirSync(target), [])
})

test('app fixture requires an explicit portable data directory', (t) => {
  const { source, target } = fixture(t)
  rmdirSync(path.join(source, 'data'))
  assert.throws(() => copyRuntime(source, target), /empty data directory/)
  assert.deepEqual(readdirSync(target), [])
})

test('excluded dependency junctions are ignored without following them', (t) => {
  const { root, source, target, write } = fixture(t)
  write('plugins/demo/frontend/dist/index.js')
  const outside = path.join(root, 'outside')
  mkdirSync(outside)
  writeFileSync(path.join(outside, 'private.txt'), 'never copy this')
  symlinkSync(outside, path.join(source, 'plugins/demo/frontend/node_modules'), process.platform === 'win32' ? 'junction' : 'dir')
  copyRuntime(source, target)
  assert.equal(existsSync(path.join(target, 'plugins/demo/frontend/node_modules')), false)
  assert.equal(readFileSync(path.join(outside, 'private.txt'), 'utf8'), 'never copy this')
})

test('unrecognized runtime links are rejected instead of traversed', (t) => {
  const { root, source, target, write } = fixture(t)
  write('plugins/demo/manifest.json')
  const outside = path.join(root, 'outside')
  mkdirSync(outside)
  writeFileSync(path.join(outside, 'private.txt'), 'never copy this')
  symlinkSync(outside, path.join(source, 'plugins/demo/escape'), process.platform === 'win32' ? 'junction' : 'dir')
  assert.throws(() => copyRuntime(source, target), /must not contain symlinks/)
  assert.equal(existsSync(path.join(target, 'plugins/demo/escape/private.txt')), false)
})
