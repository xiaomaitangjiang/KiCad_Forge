import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFile } from 'node:fs/promises'
import ts from 'typescript'

const dataModule = (source) => `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`
const fileUrl = (path) => new URL(path, import.meta.url).href

// Transpile the real component with controlled hook inputs. Inspect its React
// elements so the test exercises the owner props actually sent to PluginEntries.
async function renderSymbolTable(workspaceId) {
  const symbol = { id: 'symbol-1', name: 'Part', footprint_status: 'available', model_status: 'available' }
  const menu = { x: 12, y: 24, symbol }
  const imports = {
    react: dataModule(`export const useState = () => [${JSON.stringify(menu)}, () => {}]
export const useEffect = () => {}
export const useRef = (value) => ({ current: value })`),
    'react-i18next': dataModule('export const useTranslation = () => ({ t: (key) => key })'),
    '../lib/footprintStatus': fileUrl('../../src/lib/footprintStatus.js'),
    '../lib/typeMetadata': fileUrl('../../src/lib/typeMetadata.js'),
    './PluginSlot': dataModule(`
      export function PluginEntries() {}
      export function PluginSlot() {}
      export const usePluginEntries = () => [{}]
    `),
    '../editing/runtime': dataModule(`
      export const editingRuntime = {
        freeze: () => ({ owner: 'objects', objects: { history: {} } }),
        getState: () => ({ disabled: true }),
        execute: async () => ({ status: 'done' }),
        platform: () => 'other',
      }
    `),
    '../editing/reasons': dataModule("export const reasonLabel = () => ''"),
    '../editing/selection-model': fileUrl('../../src/editing/selection-model.js'),
    '../editing/command-service': dataModule("export const shortcutLabel = () => 'Ctrl+C'"),
    '../workbench/overlay': dataModule('export const useOverlayLayer = () => false'),
  }
  let source = await readFile(new URL('../../src/components/SymbolTable.jsx', import.meta.url), 'utf8')
  for (const [name, url] of Object.entries(imports)) {
    source = source.replaceAll(`from '${name}'`, `from '${url}'`)
  }
  const { outputText } = ts.transpileModule(source, {
    compilerOptions: { jsx: ts.JsxEmit.ReactJSX, target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ESNext },
    fileName: 'SymbolTable.jsx',
  })
  const output = outputText.replace('"react/jsx-runtime"', JSON.stringify(fileUrl('../../node_modules/react/jsx-runtime.js')))
  const { default: SymbolTable } = await import(dataModule(output))
  return SymbolTable({
    symbols: [symbol], selection: { ids: [], activeId: null, anchorId: null }, onSelect() {},
    typeMap: new Map(), workspaceId,
  })
}

function elements(node) {
  if (!node || typeof node !== 'object') return []
  if (Array.isArray(node)) return node.flatMap(elements)
  return [node, ...elements(node.props?.children)]
}

for (const workspaceId of ['workspace-1', '']) {
  test(`symbol context menu supplies the complete public owner contract (${workspaceId || 'no workspace'})`, async () => {
    const tree = await renderSymbolTable(workspaceId)
    const menu = elements(tree).find((element) => element.props?.slot === 'contextmenu.item')
    assert.ok(menu, 'the open context menu must render plugin entries')
    assert.equal(menu.props.ownerProps.workspaceId, workspaceId || null)
    assert.equal(menu.props.ownerProps.targetKind, 'symbol')
    assert.equal(menu.props.ownerProps.symbolId, 'symbol-1')
    assert.equal(menu.props.ownerProps.target.id, 'symbol-1')
  })
}
