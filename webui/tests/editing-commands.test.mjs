import test from 'node:test'
import assert from 'node:assert/strict'

import {
  captureEditContext,
  editCommandState,
  focusEditContext,
  hasSelection,
  isEditSnapshotUnchanged,
  supportsTextSelection,
} from '../src/lib/editingContext.js'
import {
  COMMANDS,
  commandState,
  createCommandService,
  isSnapshotCurrent,
  matchShortcut,
  normalizeSnapshot,
  shortcutLabel,
} from '../src/editing/command-service.js'
import { MAX_CLIPBOARD_BYTES, MAX_CLIPBOARD_ITEMS, checkCapacity } from '../src/editing/limits.js'

function fakeInput(overrides = {}) {
  const element = {
    nodeType: 1,
    tagName: 'INPUT',
    type: 'text',
    isConnected: true,
    disabled: false,
    readOnly: false,
    isContentEditable: false,
    value: 'hello world',
    selectionStart: 0,
    selectionEnd: 5,
    focused: false,
    restored: null,
    getClientRects: () => [{ width: 10, height: 10 }],
    closest: () => null,
    getAttribute: () => null,
    focus() {
      this.focused = true
    },
    setSelectionRange(start, end) {
      this.selectionStart = start
      this.selectionEnd = end
      this.restored = [start, end]
    },
    ...overrides,
  }
  return element
}

function textSnapshot(overrides = {}) {
  return {
    owner: 'text',
    scopeKey: 'scope-1',
    epoch: 3,
    text: {
      key: 'input-a',
      version: 1,
      valid: true,
      readOnly: false,
      selectable: true,
      hasSelection: true,
      value: 'hello world',
      selectionStart: 0,
      selectionEnd: 5,
      ...overrides,
    },
    objects: null,
  }
}

function objectSnapshot(overrides = {}) {
  return {
    owner: 'objects',
    scopeKey: 'scope-1',
    epoch: 4,
    text: null,
    objects: {
      domain: 'symbols',
      count: 2,
      total: 10,
      supported: true,
      writable: true,
      busy: false,
      pendingCutCount: 0,
      clipboard: { kind: 'kforge-symbols', version: 1, bytes: 2048, items: 2 },
      history: { undoLabel: '粘贴 2 个符号', redoLabel: '', undoable: true, redoable: false },
      ...overrides,
    },
  }
}

function keyEvent(overrides = {}) {
  return {
    key: 'c',
    ctrlKey: true,
    metaKey: false,
    shiftKey: false,
    altKey: false,
    isComposing: false,
    keyCode: 0,
    defaultPrevented: false,
    prevented: false,
    preventDefault() {
      this.prevented = true
      this.defaultPrevented = true
    },
    ...overrides,
  }
}

test('text selection capability is judged by real API support', () => {
  assert.equal(supportsTextSelection(fakeInput()), true)
  const numberInput = fakeInput({ type: 'number', selectionStart: null, selectionEnd: null })
  assert.equal(supportsTextSelection(numberInput), false)
  const readOnly = fakeInput({ readOnly: true })
  assert.equal(supportsTextSelection(readOnly), true)
})

test('captureEditContext marks non-selectable inputs instead of faking an empty selection', () => {
  const context = captureEditContext(fakeInput({ type: 'number', selectionStart: null, selectionEnd: null }), 'scope-1')
  assert.ok(context)
  assert.equal(context.selectable, false)
  assert.equal(context.selectionStart, 0)
  assert.equal(context.selectionEnd, 0)
  assert.equal(hasSelection(context), false)

  const text = captureEditContext(fakeInput(), 'scope-1')
  assert.equal(text.selectable, true)
  assert.equal(hasSelection(text), true)
})

test('focusEditContext restores the captured range through the real setSelectionRange', () => {
  const element = fakeInput()
  const context = captureEditContext(element, 'scope-1')
  context.selectionStart = 2
  context.selectionEnd = 8
  const restored = focusEditContext(context)
  assert.equal(restored, true)
  assert.equal(element.focused, true)
  assert.deepEqual(element.restored, [2, 8])
})

test('focusEditContext does not throw when the control has no setSelectionRange', () => {
  const element = fakeInput({ setSelectionRange: undefined })
  const context = {
    target: element,
    scopeKey: 'scope-1',
    kind: 'input',
    readOnly: false,
    selectable: false,
    value: 'hello',
    selectionStart: 0,
    selectionEnd: 0,
    range: null,
  }
  assert.equal(focusEditContext(context), true)
  assert.equal(element.focused, true)
})

test('editCommandState keeps readOnly, empty selection and unsupported selection apart', () => {
  const editable = editCommandState(captureEditContext(fakeInput(), 'scope-1'), 'scope-1')
  for (const id of COMMANDS) assert.equal(editable[id].disabled, false, id + ' should be enabled')

  const noSelection = editCommandState(
    captureEditContext(fakeInput({ selectionStart: 0, selectionEnd: 0 }), 'scope-1'),
    'scope-1',
  )
  assert.deepEqual(noSelection.copy, { disabled: true, reason: 'noSelection' })
  assert.deepEqual(noSelection.cut, { disabled: true, reason: 'noSelection' })
  assert.equal(noSelection.paste.disabled, false)

  const readOnly = editCommandState(captureEditContext(fakeInput({ readOnly: true }), 'scope-1'), 'scope-1')
  assert.equal(readOnly.copy.disabled, false)
  assert.deepEqual(readOnly.cut, { disabled: true, reason: 'readOnly' })
  assert.deepEqual(readOnly.paste, { disabled: true, reason: 'readOnly' })
  assert.deepEqual(readOnly.undo, { disabled: true, reason: 'readOnly' })
  assert.equal(readOnly.selectAll.disabled, false)

  const unsupportedInput = fakeInput({ type: 'number', selectionStart: null, selectionEnd: null })
  const unsupported = editCommandState(captureEditContext(unsupportedInput, 'scope-1'), 'scope-1')
  assert.deepEqual(unsupported.copy, { disabled: true, reason: 'noSelectionSupport' })
  assert.deepEqual(unsupported.cut, { disabled: true, reason: 'noSelectionSupport' })
  assert.deepEqual(unsupported.selectAll, { disabled: true, reason: 'noSelectionSupport' })
  assert.equal(unsupported.paste.disabled, false)

  const wrongScope = editCommandState(captureEditContext(fakeInput(), 'scope-1'), 'scope-2')
  assert.deepEqual(wrongScope.copy, { disabled: true, reason: 'noTarget' })
})

test('commandState for object lists reflects selection, clipboard, history and busy state', () => {
  const base = objectSnapshot()
  for (const id of ['copy', 'cut', 'paste', 'selectAll', 'undo']) {
    const state = commandState(base, id)
    assert.equal(state.disabled, false, id + ' should be enabled: ' + JSON.stringify(state))
  }
  assert.deepEqual(commandState(base, 'redo'), { disabled: true, reason: 'historyEmpty' })

  assert.equal(commandState(objectSnapshot({ supported: false }), 'copy').reason, 'objectUnsupported')
  assert.equal(commandState(objectSnapshot({ count: 0 }), 'copy').reason, 'noObjectSelection')
  assert.equal(commandState(objectSnapshot({ count: 0 }), 'selectAll').disabled, false)
  assert.equal(commandState(objectSnapshot({ total: 0 }), 'selectAll').reason, 'noResults')
  assert.equal(commandState(objectSnapshot({ clipboard: null }), 'paste').reason, 'clipboardEmpty')
  assert.equal(objectSnapshot({ writable: false }) && commandState(objectSnapshot({ writable: false }), 'cut').reason, 'readOnly')
  assert.equal(commandState(objectSnapshot({ writable: false }), 'copy').disabled, false)
  assert.equal(commandState(objectSnapshot({ history: { undoable: false, redoable: false } }), 'undo').reason, 'historyEmpty')
  assert.equal(commandState(objectSnapshot({ busy: true }), 'paste').reason, 'busy')
  assert.equal(commandState(objectSnapshot({ busy: true }), 'copy').disabled, false)

  assert.equal(commandState({ owner: null }, 'copy').reason, 'noTarget')
  assert.equal(commandState(textSnapshot({ valid: false }), 'copy').reason, 'noTarget')
})

test('matchShortcut follows platform modifiers and IME rules', () => {
  assert.equal(matchShortcut(keyEvent(), 'other'), 'copy')
  assert.equal(matchShortcut(keyEvent({ key: 'x' }), 'other'), 'cut')
  assert.equal(matchShortcut(keyEvent({ key: 'v' }), 'other'), 'paste')
  assert.equal(matchShortcut(keyEvent({ key: 'a' }), 'other'), 'selectAll')
  assert.equal(matchShortcut(keyEvent({ key: 'z' }), 'other'), 'undo')
  assert.equal(matchShortcut(keyEvent({ key: 'z', shiftKey: true }), 'other'), 'redo')
  assert.equal(matchShortcut(keyEvent({ key: 'y' }), 'other'), 'redo')
  assert.equal(matchShortcut(keyEvent({ key: 'y' }), 'mac'), null)
  assert.equal(matchShortcut(keyEvent({ key: 'z', shiftKey: true, ctrlKey: false, metaKey: true }), 'mac'), 'redo')

  assert.equal(matchShortcut(keyEvent({ ctrlKey: false, metaKey: true }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ ctrlKey: false, metaKey: true }), 'mac'), 'copy')
  assert.equal(matchShortcut(keyEvent({ ctrlKey: false }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ altKey: true }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ isComposing: true }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ keyCode: 229 }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ key: 'Process' }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ defaultPrevented: true }), 'other'), null)
  assert.equal(matchShortcut(keyEvent({ key: 'c', shiftKey: true }), 'other'), null)
  assert.equal(matchShortcut(null, 'other'), null)

  assert.equal(shortcutLabel('copy', 'mac'), 'Cmd+C')
  assert.equal(shortcutLabel('redo', 'mac'), 'Cmd+Shift+Z')
  assert.equal(shortcutLabel('copy', 'other'), 'Ctrl+C')
  assert.equal(shortcutLabel('redo', 'other'), 'Ctrl+Y')
})

test('isSnapshotCurrent rejects a moved target, version bump or scope change', () => {
  const frozen = normalizeSnapshot(textSnapshot())
  assert.equal(isSnapshotCurrent(frozen, textSnapshot()), true)
  assert.equal(isSnapshotCurrent(frozen, textSnapshot({ version: 2 })), false)
  assert.equal(isSnapshotCurrent(frozen, textSnapshot({ key: 'input-b' })), false)
  assert.equal(isSnapshotCurrent(frozen, { ...textSnapshot(), epoch: 9 }), false)
  assert.equal(isSnapshotCurrent(frozen, { ...textSnapshot(), scopeKey: 'scope-2' }), false)

  const objects = normalizeSnapshot(objectSnapshot())
  assert.equal(isSnapshotCurrent(objects, objectSnapshot()), true)
  assert.equal(isSnapshotCurrent(objects, objectSnapshot({ domain: 'footprints' })), false)
})

test('createCommandService refuses disabled and stale commands without calling handlers', async () => {
  let calls = 0
  const snapshots = [{ owner: null }]
  const service = createCommandService({
    getSnapshot: () => snapshots[snapshots.length - 1],
    handlers: {
      copy: async () => {
        calls += 1
        return { status: 'done', text: 'hello' }
      },
    },
    platform: 'other',
  })

  const disabled = await service.execute('copy')
  assert.deepEqual(disabled, { status: 'disabled', commandId: 'copy', reason: 'noTarget' })
  assert.equal(calls, 0)

  const frozen = normalizeSnapshot(textSnapshot())
  snapshots.push(textSnapshot({ version: 5 }))
  const stale = await service.execute('copy', frozen)
  assert.deepEqual(stale, { status: 'stale', commandId: 'copy', reason: 'staleTarget' })
  assert.equal(calls, 0)

  snapshots.push(textSnapshot())
  const done = await service.execute('copy', frozen)
  assert.equal(done.status, 'done')
  assert.equal(done.text, 'hello')
  assert.equal(calls, 1)
})

test('createCommandService reports handler failures as failed results', async () => {
  const failure = new Error('clipboard denied')
  const snapshots = [textSnapshot()]
  const service = createCommandService({
    getSnapshot: () => snapshots[0],
    handlers: { copy: async () => { throw failure } },
    platform: 'other',
  })
  const errors = []
  const serviceWithSpy = createCommandService({
    getSnapshot: () => snapshots[0],
    handlers: { copy: async () => { throw failure } },
    platform: 'other',
    onError: (error) => errors.push(error),
  })
  const result = await service.execute('copy')
  assert.equal(result.status, 'failed')
  assert.equal(result.error, failure)
  const spied = await serviceWithSpy.execute('copy')
  assert.equal(spied.status, 'failed')
  assert.deepEqual(errors, [failure])
})


test('object write commands share one gate across cut/paste/undo/redo', async () => {
  let release
  const pending = new Promise((resolve) => { release = resolve })
  let cutCalls = 0
  const service = createCommandService({
    getSnapshot: () => objectSnapshot({
      history: { undoLabel: '粘贴', redoLabel: '重做', undoable: true, redoable: true },
    }),
    handlers: {
      undo: () => pending,
      cut: async () => { cutCalls += 1; return { status: 'done' } },
    },
    platform: 'other',
  })

  const undo = service.execute('undo')
  assert.equal(service.getState('cut').reason, 'busy')
  assert.equal(service.getState('paste').reason, 'busy')
  assert.equal(service.getState('redo').reason, 'busy')
  assert.equal(service.getState('copy').disabled, false, 'read-only commands stay available')

  const blocked = await service.execute('cut')
  assert.equal(blocked.status, 'disabled')
  assert.equal(blocked.reason, 'busy')
  assert.equal(cutCalls, 0, 'a second write must not reach the handler')

  release({ status: 'done', operation: 'undo' })
  const done = await undo
  assert.equal(done.status, 'done')
  assert.equal(service.getState('cut').disabled, false)
})

test('unknown undo/redo result freezes writes until the caller reloads state', async () => {
  const network = Object.assign(new Error('Network error'), { kind: 'network' })
  const service = createCommandService({
    getSnapshot: () => objectSnapshot({
      history: { undoLabel: '粘贴', redoLabel: '重做', undoable: true, redoable: true },
    }),
    handlers: { undo: async () => { throw network } },
    platform: 'other',
    onError: () => {},
  })
  const result = await service.execute('undo')
  assert.equal(result.status, 'failed')
  assert.equal(service.isWriteUncertain(), true)
  assert.equal(service.getState('redo').reason, 'uncertain')
  assert.equal(service.getState('cut').reason, 'uncertain')
  assert.equal(service.getState('copy').disabled, false)
  service.clearWriteUncertain()
  assert.equal(service.getState('redo').disabled, false)

  const rejected = Object.assign(new Error('conflict'), { kind: 'http', status: 409 })
  const deterministic = createCommandService({
    getSnapshot: () => objectSnapshot({
      history: { undoLabel: '粘贴', redoLabel: '重做', undoable: true, redoable: true },
    }),
    handlers: { undo: async () => { throw rejected } },
    platform: 'other',
    onError: () => {},
  })
  await deterministic.execute('undo')
  assert.equal(deterministic.isWriteUncertain(), false, 'a deterministic 4xx must not freeze writes')
  assert.equal(deterministic.getState('redo').disabled, false)
})

test('handleKeydown dispatches once, prevents native behaviour only when enabled', () => {
  const snapshots = [textSnapshot()]
  const received = []
  const service = createCommandService({
    getSnapshot: () => snapshots[0],
    handlers: { copy: async () => { received.push('copy'); return { status: 'done' } } },
    platform: 'other',
  })

  const event = keyEvent({ key: 'a' })
  service.handleKeydown(event)
  assert.equal(event.prevented, true)
  assert.equal(service.handleKeydown(event), null, 'same event must not dispatch twice')

  const blocked = keyEvent({ key: 'c' })
  snapshots[0] = textSnapshot({ hasSelection: false })
  assert.equal(service.handleKeydown(blocked), null)
  assert.equal(blocked.prevented, false, 'disabled command must leave native copy alone')

  const unrelated = keyEvent({ key: 'q' })
  assert.equal(service.handleKeydown(unrelated), null)
})

test('isEditSnapshotUnchanged detects content and selection drift', () => {
  const element = fakeInput()
  const context = captureEditContext(element, 'scope-1')
  assert.equal(isEditSnapshotUnchanged(context, captureEditContext(element, 'scope-1')), true)

  element.selectionStart = 1
  element.selectionEnd = 6
  const moved = captureEditContext(element, 'scope-1')
  assert.equal(isEditSnapshotUnchanged(context, moved), false)
  element.selectionStart = 0
  element.selectionEnd = 5

  element.value = 'hello WORLD'
  const edited = captureEditContext(element, 'scope-1')
  assert.equal(isEditSnapshotUnchanged(context, edited), false)
  element.value = 'hello world'

  const otherTarget = captureEditContext(fakeInput(), 'scope-1')
  assert.equal(isEditSnapshotUnchanged(context, otherTarget), false)
})

test('capacity constants guard clipboard and history budgets', () => {
  assert.equal(MAX_CLIPBOARD_ITEMS, 200)
  assert.equal(MAX_CLIPBOARD_BYTES, 4 * 1024 * 1024)
  assert.equal(checkCapacity(1, 1024), null)
  assert.equal(checkCapacity(201, 1024), 'items')
  assert.equal(checkCapacity(1, MAX_CLIPBOARD_BYTES + 1), 'bytes')
})
