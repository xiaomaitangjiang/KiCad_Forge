// 真实 WebView2 编辑命令验收：文本六命令、对象复制/粘贴/冲突改名、剪切移动、撤销/重做。
// KFORGE_BUILD_ROOT=<empty-data package> node webui/tests/e2e/run-editing-e2e.mjs
import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, runAppFixture } from '../support/plugin-e2e-harness.mjs'
import { seedEditingFixture } from '../support/editing-fixtures.mjs'

const MOD = { ctrl: 2, shift: 8 }
const sleep = (ms) => new Promise(resolve => setTimeout(resolve, ms))

async function key(cdp, { key: value, code, vk, modifiers = 0 }) {
  const base = { modifiers, key: value, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk }
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base })
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base })
}

async function waitForCards(cdp, waitFor, expected) {
  await waitFor(() => cdp.eval(`document.querySelectorAll('.symbol-card').length === ${expected}`), 30000,
    `${expected} symbol cards`)
}

async function selectAllSymbols(cdp, waitFor, expected) {
  await cdp.eval(`[...document.querySelectorAll('.sidebar-navigation .nav-item')].find((node) => node.textContent.includes('全部元件'))?.click()`)
  await waitForCards(cdp, waitFor, expected)
}

async function clickCard(cdp, name, modifiers = 0) {
  const point = await cdp.eval(`(() => {
    const card = [...document.querySelectorAll('.symbol-card')].find(n => n.querySelector('.symbol-name')?.textContent.trim() === ${JSON.stringify(name)})
    if (!card) return null
    card.scrollIntoView({ block: 'center' })
    const r = card.getBoundingClientRect()
    return { x: r.x + Math.min(40, r.width / 2), y: r.y + r.height / 2 }
  })()`)
  check(point, `symbol card missing: ${name}`)
  await cdp.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: point.x, y: point.y, button: 'left', buttons: 1, clickCount: 1, modifiers })
  await cdp.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: point.x, y: point.y, button: 'left', buttons: 0, clickCount: 1, modifiers })
  await sleep(80)
}

async function selectionState(cdp) {
  return cdp.eval(`(() => {
    const selected = [...document.querySelectorAll('.symbol-card.selected')]
    return { count: selected.length, names: selected.map(n => n.querySelector('.symbol-name')?.textContent.trim()) }
  })()`)
}

async function readClipboard(cdp) {
  return cdp.eval('navigator.clipboard.readText()')
}

async function setClipboard(cdp, text) {
  await cdp.eval(`navigator.clipboard.writeText(${JSON.stringify(text)})`)
}

async function dialogState(cdp) {
  return cdp.eval(`(() => {
    const modal = document.querySelector('.paste-target-modal[role=dialog]')
    if (!modal) return null
    return {
      title: modal.querySelector('h3')?.textContent?.trim() || '',
      summary: modal.querySelector('.paste-target-summary')?.textContent?.trim() || '',
      targets: [...modal.querySelectorAll('select option')].map(o => ({ id: o.value, name: o.textContent.trim() })),
      selectedTarget: modal.querySelector('select')?.value || '',
      conflicts: [...modal.querySelectorAll('.paste-conflict-row input')].map(i => i.value),
      error: modal.querySelector('.paste-target-error')?.textContent?.trim() || '',
      confirmDisabled: modal.querySelector('.btn-primary')?.disabled === true,
      confirmLabel: modal.querySelector('.btn-primary')?.textContent?.trim() || '',
    }
  })()`)
}

async function chooseTarget(cdp, libraryId) {
  await cdp.eval(`(() => {
    const select = document.querySelector('.paste-target-modal select')
    if (!select) return false
    const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set
    setter.call(select, ${JSON.stringify(libraryId)})
    select.dispatchEvent(new Event('change', { bubbles: true }))
    return true
  })()`)
  await sleep(80)
}

async function clickDialog(cdp, selector) {
  await cdp.eval(`document.querySelector('.paste-target-modal ${selector}')?.click()`)
}

async function apiSymbols(json, origin) {
  return json(`${origin}/api/symbols`)
}

async function findSymbol(json, origin, name, libraryId) {
  const list = await apiSymbols(json, origin)
  return list.find(s => s.name === name && (!libraryId || s.library_id === libraryId)) || null
}

function fileContains(file, name) {
  return readFileSync(file, 'utf8').includes(`(symbol "${name}"`)
}

async function openEditMenu(cdp, waitFor) {
  await cdp.eval(`[...document.querySelectorAll('.topbar-trigger')].find(n => n.textContent.trim() === '编辑')?.click()`)
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".topbar-dropdown [role=menuitem]"))'), 3000,
    'edit menu opens')
}

async function editMenuItems(cdp) {
  return cdp.eval(`[...document.querySelectorAll('.topbar-dropdown [role=menuitem]')].map(b => ({
    label: b.querySelector('span:not(.topbar-shortcut)')?.textContent.trim() || b.textContent.trim(),
    disabled: b.disabled,
    reason: b.title || '',
  }))`)
}

async function clickMenuItem(cdp, label) {
  await cdp.eval(`[...document.querySelectorAll('.topbar-dropdown [role=menuitem]')].find(b => b.textContent.includes(${JSON.stringify(label)}))?.click()`)
}

await runAppFixture('editing-e2e', async ({ root, launch, stop, json, waitFor, results }) => {
  // 1) 首次启动建库，停止后直接种入「一个工作空间 + 两个可写符号库」。
  let session = await launch()
  await waitFor(() => session.cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'first workbench boot')
  await stop(session)

  const { fileA, fileB } = seedEditingFixture(root)

  // 2) 正式会话。
  session = await launch()
  const { cdp, origin } = session
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".workbench"))'), 30000, 'workbench boot')
  await waitForCards(cdp, waitFor, 4)
  const startSymbols = await apiSymbols(json, origin)
  check(startSymbols.length === 4, 'seeded symbols missing: ' + JSON.stringify(startSymbols))
  check(fileContains(fileA, 'Alpha') && fileContains(fileB, 'Gamma'), 'seeded source files missing')

  // 3) 文本命令：真实焦点 + 选区 + 系统剪贴板 + React 受控值。
  await cdp.eval('document.querySelector(".toolbar-search")?.focus()')
  await cdp.send('Input.insertText', { text: 'text' })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "text"'), 3000, 'text input value')
  await key(cdp, { key: 'a', code: 'KeyA', vk: 65, modifiers: MOD.ctrl })
  const selectedText = await cdp.eval(`(() => { const i = document.querySelector('.toolbar-search'); return [i.selectionStart, i.selectionEnd, i.value] })()`)
  check(selectedText[0] === 0 && selectedText[1] === 4, 'Ctrl+A must select the focused field: ' + JSON.stringify(selectedText))
  await key(cdp, { key: 'c', code: 'KeyC', vk: 67, modifiers: MOD.ctrl })
  await waitFor(async () => (await readClipboard(cdp)) === 'text', 3000, 'text copied to system clipboard')
  await setClipboard(cdp, 'XYZ')
  await key(cdp, { key: 'a', code: 'KeyA', vk: 65, modifiers: MOD.ctrl })
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "XYZ"'), 3000, 'text paste replaces selection')
  await key(cdp, { key: 'z', code: 'KeyZ', vk: 90, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "text"'), 3000, 'text undo restores React value')
  await key(cdp, { key: 'y', code: 'KeyY', vk: 89, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "XYZ"'), 3000, 'text redo')
  await key(cdp, { key: 'a', code: 'KeyA', vk: 65, modifiers: MOD.ctrl })
  await key(cdp, { key: 'x', code: 'KeyX', vk: 88, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === ""'), 3000, 'text cut deletes after clipboard write')
  check((await readClipboard(cdp)) === 'XYZ', 'text cut must keep the selection in clipboard')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === "XYZ"'), 3000, 'text paste restores')
  // 清空搜索，避免过滤对象列表。
  await cdp.eval(`(() => { const i = document.querySelector('.toolbar-search'); i.focus(); i.select() })()`)
  await key(cdp, { key: 'Backspace', code: 'Backspace', vk: 8 })
  await waitFor(() => cdp.eval('document.querySelector(".toolbar-search")?.value === ""'), 3000, 'search cleared')
  await waitForCards(cdp, waitFor, 4)
  await cdp.eval('document.querySelector(".toolbar-search")?.blur()')

  // 4) 多选、对象全选与内建右键（ED-020..024）：集合按 UUID，右键不缩减已有多选。
  let selection
  await clickCard(cdp, 'Alpha')
  await clickCard(cdp, 'Beta', MOD.ctrl)
  selection = await selectionState(cdp)
  check(selection.count === 2 && selection.names.includes('Alpha') && selection.names.includes('Beta'),
    'Ctrl-click must add to the selection: ' + JSON.stringify(selection))
  check(await cdp.eval('document.body.innerText.includes("已选择 2 / 当前结果 4")'), 'status bar must show selected/total')
  check(await cdp.eval('document.querySelector("[data-testid=inspector-multi]")?.innerText.includes("已选择 2 个符号") === true'),
    'inspector must summarize the multi-selection')
  await cdp.eval('document.querySelector("[data-symbol-id=e2e-sym-alpha]")?.focus()')
  await key(cdp, { key: 'a', code: 'KeyA', vk: 65, modifiers: MOD.ctrl })
  selection = await selectionState(cdp)
  check(selection.count === 4, 'object Ctrl+A must select all loaded results: ' + JSON.stringify(selection))
  check(await cdp.eval('document.body.innerText.includes("已选择 4 / 当前结果 4")'), 'status bar must show all selected')
  await cdp.eval(`document.querySelector('[data-symbol-id=e2e-sym-beta]')?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 120, clientY: 220 }))`)
  await waitFor(() => cdp.eval('document.querySelectorAll(".symbol-context-menu [role=menuitem]").length >= 6'), 3000, 'built-in context menu')
  check(await cdp.eval('document.querySelectorAll(".symbol-context-menu [role=menuitem]").length >= 6'), 'context menu must expose the six edit commands')
  selection = await selectionState(cdp)
  check(selection.count === 4, 'right-click on a selected card must preserve the multi-selection')
  await key(cdp, { key: 'Escape', code: 'Escape', vk: 27 })
  await waitFor(() => cdp.eval('!document.querySelector(".symbol-context-menu")'), 3000, 'context menu closes')

  // 5) 对象复制 → 粘贴到另一个符号库（不冲突），检查新 UUID、文件与历史。
  await clickCard(cdp, 'Alpha')
  selection = await selectionState(cdp)
  check(selection.count === 1 && selection.names[0] === 'Alpha', 'single click selection: ' + JSON.stringify(selection))
  // 菜单与快捷键必须共用同一可用状态（ED-003）：先用菜单复制，再用快捷键复制。
  await openEditMenu(cdp, waitFor)
  const menuItems = await editMenuItems(cdp)
  check(menuItems.length === 6, 'edit menu must expose six commands: ' + JSON.stringify(menuItems))
  const copyItem = menuItems.find(item => item.label === '复制')
  check(copyItem && copyItem.disabled === false, 'object copy must be enabled for the selected symbol: ' + JSON.stringify(menuItems))
  await clickMenuItem(cdp, '复制')
  await waitFor(async () => (await readClipboard(cdp)).startsWith('KFORGE-SYMBOLS-V1'), 5000, 'menu copy writes envelope')
  const alphaBefore = await findSymbol(json, origin, 'Alpha', 'e2e-lib-a')
  check(alphaBefore, 'Alpha missing before copy')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".paste-target-modal"))'), 5000, 'paste dialog opens')
  let dialog = await dialogState(cdp)
  check(dialog.targets.some(t => t.id === 'e2e-lib-a') && dialog.targets.some(t => t.id === 'e2e-lib-b'),
    'paste targets must be symbol libraries, got ' + JSON.stringify(dialog.targets))
  check(dialog.confirmLabel === '粘贴', 'paste dialog localized label: ' + dialog.confirmLabel)
  await chooseTarget(cdp, 'e2e-lib-b')
  await clickDialog(cdp, '.btn-primary')
  await waitFor(() => cdp.eval('!document.querySelector(".paste-target-modal")'), 8000, 'paste commit closes dialog')
  // R6：提交后定位目标库（LibB），列表只显示该库的 3 个符号。
  await waitForCards(cdp, waitFor, 3)
  check(await cdp.eval(`document.querySelector('.sidebar-library-row.active')?.textContent.includes('LibB') === true`),
    'paste refresh must select the committed target library')
  const alphaAfter = await findSymbol(json, origin, 'Alpha', 'e2e-lib-b')
  check(alphaAfter, 'pasted Alpha must exist in LibB')
  check(alphaAfter.id !== alphaBefore.id, 'paste must allocate a new UUID')
  check(fileContains(fileB, 'Alpha'), 'pasted symbol must be appended to LibB source file')
  const historyAfterPaste = await json(`${origin}/api/editing/history`)
  check(historyAfterPaste.can_undo === true && historyAfterPaste.count >= 1, 'paste must create history: ' + JSON.stringify(historyAfterPaste))

  // 6) 撤销 → 重做：同一 UUID/同一位置。
  await selectAllSymbols(cdp, waitFor, 5)
  await clickCard(cdp, 'Alpha')
  await key(cdp, { key: 'z', code: 'KeyZ', vk: 90, modifiers: MOD.ctrl })
  await waitForCards(cdp, waitFor, 4)
  await waitFor(async () => (await findSymbol(json, origin, 'Alpha', 'e2e-lib-b')) === null, 8000,
    'undo removes the pasted symbol')
  await clickCard(cdp, 'Alpha')
  await key(cdp, { key: 'y', code: 'KeyY', vk: 89, modifiers: MOD.ctrl })
  await waitForCards(cdp, waitFor, 5)
  const alphaRedone = await waitFor(async () => {
    const found = await findSymbol(json, origin, 'Alpha', 'e2e-lib-b')
    return found?.id === alphaAfter.id ? found : null
  }, 8000, 'redo restores the same pasted UUID')

  // 7) 同名冲突：不自动覆盖，必须显式改名。
  await clickCard(cdp, 'Alpha')
  await key(cdp, { key: 'c', code: 'KeyC', vk: 67, modifiers: MOD.ctrl })
  await waitFor(async () => (await readClipboard(cdp)).startsWith('KFORGE-SYMBOLS-V1'), 5000, 'second copy')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".paste-target-modal"))'), 5000, 'conflict paste dialog')
  await chooseTarget(cdp, 'e2e-lib-b')
  await clickDialog(cdp, '.btn-primary')
  await waitFor(() => cdp.eval('document.querySelector(".paste-conflict-row input") !== null'), 5000, 'conflict rename input')
  dialog = await dialogState(cdp)
  check(dialog.conflicts.length === 1 && dialog.confirmDisabled === true,
    'unresolved conflict must block commit: ' + JSON.stringify(dialog))
  await cdp.eval(`(() => {
    const input = document.querySelector('.paste-conflict-row input')
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set
    setter.call(input, 'AlphaCopy')
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })()`)
  await sleep(80)
  await clickDialog(cdp, '.btn-primary')
  await waitFor(() => cdp.eval('!document.querySelector(".paste-target-modal")'), 8000, 'renamed paste commit')
  await waitForCards(cdp, waitFor, 4)
  check(await cdp.eval(`document.querySelector('.sidebar-library-row.active')?.textContent.includes('LibB') === true`),
    'renamed paste refresh must select LibB')
  await selectAllSymbols(cdp, waitFor, 6)
  await waitFor(async () => Boolean(await findSymbol(json, origin, 'AlphaCopy', 'e2e-lib-b')), 8000,
    'renamed paste creates AlphaCopy')

  // 8) 剪切 → 跨库移动：源 UUID 不变、源从 LibA 移除、撤销恢复。
  await clickCard(cdp, 'Beta')
  const betaBefore = await findSymbol(json, origin, 'Beta', 'e2e-lib-a')
  check(betaBefore, 'Beta missing before cut')
  await key(cdp, { key: 'x', code: 'KeyX', vk: 88, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('document.querySelectorAll(".symbol-card.pending-cut").length === 1'), 5000, 'cut marker')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('Boolean(document.querySelector(".paste-target-modal"))'), 5000, 'move dialog')
  await chooseTarget(cdp, 'e2e-lib-b')
  await clickDialog(cdp, '.btn-primary')
  await waitFor(() => cdp.eval('!document.querySelector(".paste-target-modal")'), 10000, 'move commit')
  await waitForCards(cdp, waitFor, 5)
  check(await cdp.eval(`document.querySelector('.sidebar-library-row.active')?.textContent.includes('LibB') === true`),
    'move refresh must select LibB')
  await selectAllSymbols(cdp, waitFor, 6)
  const betaMoved = await waitFor(async () => {
    const [source, target] = await Promise.all([
      findSymbol(json, origin, 'Beta', 'e2e-lib-a'),
      findSymbol(json, origin, 'Beta', 'e2e-lib-b'),
    ])
    return source === null && target?.id === betaBefore.id ? target : null
  }, 10000, 'move removes Beta from LibA and keeps its UUID in LibB')
  check(!fileContains(fileA, 'Beta') && fileContains(fileB, 'Beta'), 'move must rewrite both source files')
  await clickCard(cdp, 'Beta')
  await key(cdp, { key: 'z', code: 'KeyZ', vk: 90, modifiers: MOD.ctrl })
  await waitForCards(cdp, waitFor, 6)
  await waitFor(async () => {
    const [source, target] = await Promise.all([
      findSymbol(json, origin, 'Beta', 'e2e-lib-a'),
      findSymbol(json, origin, 'Beta', 'e2e-lib-b'),
    ])
    return source?.id === betaBefore.id && target === null
  }, 8000, 'undo move restores source library/UUID')

  // 9) 请求形状与失败可发现：无前缀纯文本不能当符号剪贴板。
  await setClipboard(cdp, 'plain text')
  await clickCard(cdp, 'Gamma')
  await key(cdp, { key: 'v', code: 'KeyV', vk: 86, modifiers: MOD.ctrl })
  await waitFor(() => cdp.eval('!document.querySelector(".paste-target-modal")'), 3000, 'plain text must not open paste dialog')
  const toast = await cdp.eval('document.querySelector("#toast")?.textContent || document.body.innerText')
  check(toast.includes('剪贴板') || toast.includes('符号'), 'format failure should be visible in toast: ' + toast.slice(0, 300))

  writeFileSync(path.join(results, 'editing-e2e-summary.json'), JSON.stringify({
    start: startSymbols.length,
    copied: alphaBefore.id,
    pasted: alphaAfter.id,
    redone: alphaRedone.id,
    moved: betaMoved.id,
    history: historyAfterPaste,
  }, null, 2) + '\n')
  await stop(session)
})
