import test, { before, after } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { createServer } from 'vite'
import i18n from 'i18next'
import { initReactI18next, I18nextProvider } from 'react-i18next'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const zh = JSON.parse(fs.readFileSync(path.join(root, 'src/locales/zh.json'), 'utf8'))
const en = JSON.parse(fs.readFileSync(path.join(root, 'src/locales/en.json'), 'utf8'))

// Node 里没有浏览器全局；SSR 渲染只用到 localStorage 的初始化和 i18n 的语言探测。
const store = new Map()
globalThis.localStorage = {
  getItem: (key) => (store.has(key) ? store.get(key) : null),
  setItem: (key, value) => store.set(key, String(value)),
  removeItem: (key) => store.delete(key),
  clear: () => store.clear(),
}
if (!globalThis.navigator) {
  Object.defineProperty(globalThis, 'navigator', { value: { language: 'zh-CN' }, configurable: true })
}
globalThis.window = globalThis
globalThis.document = {
  addEventListener() {}, removeEventListener() {},
  documentElement: { dataset: {}, style: {} },
  querySelector: () => null, querySelectorAll: () => [], getElementById: () => null,
}

await i18n.use(initReactI18next).init({
  resources: { zh: { translation: zh }, en: { translation: en } },
  lng: 'zh',
  fallbackLng: 'en',
  interpolation: { escapeValue: false },
  initImmediate: false,
})

// 只替换 useStatus：让真实 App 在 SSR 下渲染指定的统计快照，其余 hook 保持真实实现。
// 这样中心页/状态栏/侧栏三处口径都能在真实组件树上断言，而不是另写一份展示逻辑。
function statusMockPlugin() {
  return {
    name: 'kf-status-mock',
    enforce: 'pre',
    resolveId(source, importer) {
      const fromApp = importer && importer.replace(/\\/g, '/').endsWith('/src/App.jsx')
      const isUseData = /(^|\/)hooks\/useData$/.test(source.replace(/\\/g, '/'))
      if (fromApp && isUseData) return '\0kf-use-data-mock'
      return null
    },
    load(id) {
      if (id !== '\0kf-use-data-mock') return null
      return [
        "import { useStatus as useRealStatus } from '/src/hooks/useData.js?actual'",
        "export * from '/src/hooks/useData.js?actual'",
        'export function useStatus() {',
        '  // 始终调用真实 hook 以保持 hook 顺序一致；只有测试显式给了快照时才替换状态字段。',
        '  const real = useRealStatus()',
        '  const status = globalThis.__KF_STATUS_SNAPSHOT__',
        '  return status ? { ...real, status } : real',
        '}',
      ].join('\n')
    },
  }
}

let server
let App
let SidebarNavigation

before(async () => {
  server = await createServer({
    root,
    // 单进程只起一个 Vite 实例：middlewareMode 下 HMR 端口固定，起两个会互相抢端口。
    server: { middlewareMode: true, hmr: false },
    appType: 'custom',
    logLevel: 'error',
    plugins: [statusMockPlugin()],
  })
  ;({ default: App } = await server.ssrLoadModule('/src/App.jsx'))
  ;({ default: SidebarNavigation } = await server.ssrLoadModule('/src/components/sidebar/SidebarNavigation.jsx'))
})

after(async () => {
  await server?.close()
})

function render(element) {
  return renderToStaticMarkup(React.createElement(I18nextProvider, { i18n }, element))
}

function decode(text) {
  return text
    .replace(/&quot;/g, '"').replace(/&#x27;/g, "'")
    .replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&')
}

function byTestId(html, testId) {
  const match = html.match(new RegExp(`<([a-z]+)[^>]*data-testid="${testId}"[^>]*>([\\s\\S]*?)</\\1>`))
  assert.ok(match, `页面里应存在 ${testId}`)
  const opening = match[0].slice(0, match[0].indexOf('>') + 1)
  return { text: decode(match[2]), attribute: (name) => {
    const found = opening.match(new RegExp(` ${name}="([^"]*)"`))
    return found ? decode(found[1]) : undefined
  } }
}

function mockStatus(snapshot) {
  globalThis.__KF_STATUS_SNAPSHOT__ = snapshot
  return render(React.createElement(App))
}

function renderRealStatus() {
  delete globalThis.__KF_STATUS_SNAPSHOT__
  return render(React.createElement(App))
}

const t = i18n.t.bind(i18n)

test('App SSR：首次统计未知时中心页/状态栏/侧栏都显示占位而不是空或 0', () => {
  const html = renderRealStatus()
  const center = byTestId(html, 'center-library-counts')
  assert.equal(center.text, '… 元件 · … 封装')
  assert.equal(center.attribute('title'), t('app.loadingSymbols'))
  assert.equal(center.attribute('aria-label'), `${t('sidebar.allSymbols')}: … 元件 · … 封装`)

  const statusbar = byTestId(html, 'status-library-counts')
  assert.equal(statusbar.text, `${t('sidebar.allSymbols')}: … 元件 · … 封装`)
  assert.equal(statusbar.attribute('title'), t('app.loadingSymbols'))
  assert.equal(statusbar.attribute('role'), 'status')

  const badge = byTestId(html, 'sidebar-all-symbols-count')
  assert.equal(badge.text, '…')
  assert.equal(badge.attribute('title'), t('app.loadingSymbols'))
  assert.equal(badge.attribute('aria-label'), `${t('sidebar.allSymbols')}: …`)
})

test('App SSR：真实 0 显示 0，不会被当成未知或空', () => {
  const html = mockStatus({ known: true, stale: false, reason: null, error: null, symbols: 0, footprints: 0 })
  const center = byTestId(html, 'center-library-counts')
  assert.equal(center.text, '0 元件 · 0 封装')
  assert.equal(center.attribute('aria-label'), `${t('sidebar.allSymbols')}: 0 元件 · 0 封装`)
  assert.equal(byTestId(html, 'status-library-counts').text, `${t('sidebar.allSymbols')}: 0 元件 · 0 封装`)
  assert.equal(byTestId(html, 'sidebar-all-symbols-count').text, '0')
})

test('App SSR：后台忙时保留上一份有效计数，并用标题说明统计未刷新', () => {
  const html = mockStatus({ known: true, stale: true, reason: 'busy', error: null, symbols: 4, footprints: 2 })
  const center = byTestId(html, 'center-library-counts')
  assert.equal(center.text, '4 元件 · 2 封装')
  assert.equal(center.attribute('title'), t('lcsc.refreshReason.status'))
  assert.equal(byTestId(html, 'status-library-counts').text, `${t('sidebar.allSymbols')}: 4 元件 · 2 封装`)
  assert.equal(byTestId(html, 'status-library-counts').attribute('title'), t('lcsc.refreshReason.status'))
  const badge = byTestId(html, 'sidebar-all-symbols-count')
  assert.equal(badge.text, '4')
  assert.equal(badge.attribute('title'), t('lcsc.refreshReason.status'))
  assert.equal(badge.attribute('aria-label'), `${t('sidebar.allSymbols')}: 4`)
})

test('App SSR：统计读取失败时保留旧计数，不显示虚假 0 或空徽标', () => {
  const html = mockStatus({ known: true, stale: true, reason: 'error', error: '网络错误', symbols: 4, footprints: 2 })
  const center = byTestId(html, 'center-library-counts')
  assert.equal(center.text, '4 元件 · 2 封装')
  assert.equal(center.attribute('title'), t('status.loadFailed', { error: '网络错误' }))
  assert.equal(byTestId(html, 'status-library-counts').attribute('title'), t('status.loadFailed', { error: '网络错误' }))
  assert.equal(byTestId(html, 'sidebar-all-symbols-count').text, '4')
})

test('SidebarNavigation：徽标支持占位串以及 title/aria-label 说明', () => {
  const html = render(React.createElement(SidebarNavigation, {
    activeAll: true, activeIssues: false, allCount: '…',
    allCountTitle: t('app.loadingSymbols'), allCountLabel: `${t('sidebar.allSymbols')}: …`,
    issuesCount: 0, matchesCount: 0,
  }))
  const badge = byTestId(html, 'sidebar-all-symbols-count')
  assert.equal(badge.text, '…')
  assert.equal(badge.attribute('title'), t('app.loadingSymbols'))
  assert.equal(badge.attribute('aria-label'), `${t('sidebar.allSymbols')}: …`)
})
