// 文本命令适配器回归：Node 无 DOM，用最小假元素覆盖真实调用路径。
import { test, after } from 'node:test'
import assert from 'node:assert/strict'

const previousNavigator = Object.getOwnPropertyDescriptor(globalThis, 'navigator')
const previousDocument = globalThis.document
let clipboardText = ''

function fakeInput(value = 'text') {
  const host = {
    nodeType: 1,
    tagName: 'INPUT',
    type: 'text',
    className: 'toolbar-search',
    isConnected: true,
    isContentEditable: false,
    readOnly: false,
    disabled: false,
    value,
    selectionStart: value.length,
    selectionEnd: value.length,
    getClientRects: () => [{}],
    closest: () => null,
    getAttribute: () => null,
    focus() { document.activeElement = host },
    setSelectionRange(start, end) { host.selectionStart = start; host.selectionEnd = end },
    dispatchEvent() { return true },
    contains() { return false },
  }
  return host
}

Object.defineProperty(globalThis, 'navigator', {
  configurable: true,
  value: {
    clipboard: {
      writeText: async (text) => { clipboardText = text },
      readText: async () => clipboardText,
    },
  },
})
globalThis.document = {
  activeElement: null,
  querySelectorAll: () => [],
  execCommand: (command) => command === 'insertText' || command === 'delete' || command === 'selectAll',
}

const { selectAllText, copyText } = await import('../src/editing/text-adapter.js')

after(() => {
  if (previousNavigator) Object.defineProperty(globalThis, 'navigator', previousNavigator)
  else delete globalThis.navigator
  globalThis.document = previousDocument
})

test('selectAllText selects the whole field through setSelectionRange', async () => {
  const host = fakeInput('text')
  const result = await selectAllText(host, 'workspace:lib')
  assert.equal(result.status, 'done')
  assert.equal(host.selectionStart, 0)
  assert.equal(host.selectionEnd, 4)
})

test('copyText writes the current selection to the clipboard', async () => {
  clipboardText = ''
  const host = fakeInput('text')
  host.setSelectionRange(1, 3)
  const result = await copyText(host, 'workspace:lib')
  assert.equal(result.status, 'done')
  assert.equal(clipboardText, 'ex')
})

test('copyText without a selection reports noSelection and keeps the clipboard', async () => {
  clipboardText = 'keep'
  const host = fakeInput('text')
  host.setSelectionRange(2, 2)
  await assert.rejects(() => copyText(host, 'workspace:lib'), (error) => error.code === 'noSelection')
  assert.equal(clipboardText, 'keep')
})
