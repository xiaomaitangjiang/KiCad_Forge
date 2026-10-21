import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createSlotHost } from '../src/plugin-host/slot-host.js'

const panel = { name: 'sidebar.panel', kind: 'list' }
function createHost() {
  const host = createSlotHost({ additionalSlots: [panel] })
  host.registerShell()
  return host
}
const icon = (text) => ({ type: 'icon', text })

test('plugin contributions get an identity independent of local ids and sibling plugins', () => {
  const host = createHost()
  const a = host.register(panel, { id: 'workspace', title: 'A', icon: icon('a'), render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  const b = host.register(panel, { id: 'workspace', title: 'B', icon: icon('b'), render: () => null },
    { id: 'plugin.b', version: '1.0.0' })

  const entries = host.entriesWithIcons(panel)
  assert.equal(entries.length, 2)
  assert.equal(new Set(entries.map((entry) => entry.options.id)).size, 2)
  for (const entry of entries) {
    assert.notEqual(entry.options.id, 'workspace')
    assert.equal(entry.contribution.localId, 'workspace')
  }
  assert.deepEqual(entries.map((entry) => entry.contribution.pluginId), ['plugin.a', 'plugin.b'])
  assert.deepEqual(entries.map((entry) => entry.icon.text), ['a', 'b'])

  a()
  const remaining = host.entriesWithIcons(panel)
  assert.equal(remaining.length, 1)
  assert.equal(remaining[0].contribution.pluginId, 'plugin.b')
  assert.equal(remaining[0].icon.text, 'b')
  b()
})

test('duplicate registration failure leaves the first contribution and icon untouched', () => {
  const host = createHost()
  host.register(panel, { id: 'same', icon: icon('first'), render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  assert.throws(() => host.register(panel, { id: 'same', icon: icon('second'), render: () => null },
    { id: 'plugin.a', version: '1.0.0' }))
  const entries = host.entriesWithIcons(panel)
  assert.equal(entries.length, 1)
  assert.equal(entries[0].icon.text, 'first')
})

test('an old disposer cannot delete metadata of a newer contribution', () => {
  const host = createHost()
  const oldDispose = host.register(panel, { id: 'same', icon: icon('old'), render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  oldDispose()
  host.register(panel, { id: 'same', icon: icon('new'), render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  oldDispose()
  const entries = host.entriesWithIcons(panel)
  assert.equal(entries.length, 1)
  assert.equal(entries[0].icon.text, 'new')
})

test('same-order contributions sort by stable identity, not registration order', () => {
  const host = createHost()
  host.register(panel, { id: 'z', order: 5, render: () => null }, { id: 'plugin.z', version: '1.0.0' })
  host.register(panel, { id: 'a', order: 5, render: () => null }, { id: 'plugin.a', version: '1.0.0' })
  host.register(panel, { id: 'm', order: 1, render: () => null }, { id: 'plugin.m', version: '1.0.0' })
  assert.deepEqual(host.entriesWithIcons(panel).map((entry) => entry.contribution.localId), ['m', 'a', 'z'])
})

test('same-order sorting uses code units across accents, case, reverse registration and restoration', () => {
  const ids = ['z', 'é', 'a', 'Z', 'A', 'ä']
  const expected = ['A', 'Z', 'a', 'z', 'ä', 'é']
  for (const input of [ids, [...ids].reverse()]) {
    const host = createHost()
    const disposers = new Map()
    const register = (id) => host.register(panel, { id, order: 5, render: () => null },
      { id: 'plugin.sort', version: '1.0.0' })
    for (const id of input) disposers.set(id, register(id))
    const sorted = () => host.entriesWithIcons(panel).map((entry) => entry.contribution.localId)
    assert.deepEqual(sorted(), expected)
    const snapshot = host.entries(panel.name)
    assert.equal(host.entries(panel.name), snapshot)

    disposers.get('A')()
    disposers.get('é')()
    assert.deepEqual(sorted(), expected.filter((id) => !['A', 'é'].includes(id)))
    disposers.set('é', register('é'))
    disposers.set('A', register('A'))
    assert.deepEqual(sorted(), expected)
    assert.notEqual(host.entries(panel.name), snapshot)
    assert.equal(host.entries(panel.name), host.entries(panel.name))
  }
})

test('contributions without an id/key are rejected before touching the registry', () => {
  const host = createHost()
  assert.throws(() => host.register(panel, { render: () => null }, { id: 'plugin.a', version: '1.0.0' }),
    /id or key/i)
  assert.equal(host.count(panel.name), 0)
})
