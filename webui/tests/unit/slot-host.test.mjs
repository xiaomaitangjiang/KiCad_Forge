import { test } from 'node:test'
import assert from 'node:assert/strict'
import React from 'react'
import { renderToStaticMarkup } from 'react-dom/server'
import { createSlotHost, PluginContributionIcon, SlotOutlet } from '../../src/plugin-host/slot-host.js'

function createHost() {
  const host = createSlotHost({
    viewSlot: 'app.view',
    auxSlot: 'app.aux',
    additionalSlots: [
      { name: 'sidebar.section', kind: 'list' },
      { name: 'sidebar.panel', kind: 'list' },
      { name: 'toolbar.action', kind: 'list' },
      { name: 'card.accessory', kind: 'list' },
      { name: 'inspector.tab', kind: 'keyed' },
      { name: 'contextmenu.item', kind: 'list' },
      { name: 'settings.section', kind: 'list' },
    ],
  })
  host.registerShell()
  return host
}

test('host declares all seven public slots plus views', () => {
  const host = createHost()
  assert.equal(host.count('sidebar.section'), 0)
  assert.equal(host.count('sidebar.panel'), 0)
  assert.equal(host.count('toolbar.action'), 0)
  assert.equal(host.count('inspector.tab'), 0)
  assert.equal(host.count('settings.section'), 0)
  assert.equal(host.count('app.view'), 0)
})

test('a view registers into app.view and disposes without touching slots', () => {
  const host = createHost()
  const dispose = host.registerRootView(
    { id: 'demo', title: 'Demo View', slots: [{ name: 'demo.tabs', kind: 'keyed' }], render: () => null },
    { id: 'plugin.one', version: '1.0.0' },
  )
  const views = host.entries('app.view')
  assert.equal(views.length, 1)
  assert.equal(views[0].options.label, 'Demo View')
  assert.equal(host.count('demo.tabs'), 0)

  const tabDispose = host.register(
    { name: 'demo.tabs', kind: 'keyed' },
    { id: 'tab', key: 'tab', title: 'Tab', render: () => null },
  )
  assert.equal(host.count('demo.tabs'), 1)
  tabDispose()
  assert.equal(host.count('demo.tabs'), 0)

  dispose()
  assert.equal(host.count('app.view'), 0)
})

test('keyed inspector tabs keep distinct keys and replace own entries on restart', () => {
  const host = createHost()
  const dispose = host.register(
    { name: 'inspector.tab', kind: 'keyed' },
    { id: 'one', key: 'one', title: 'One', render: () => null },
  )
  host.register(
    { name: 'inspector.tab', kind: 'keyed' },
    { id: 'two', key: 'two', title: 'Two', render: () => null },
  )
  assert.equal(host.count('inspector.tab'), 2)
  dispose()
  assert.equal(host.count('inspector.tab'), 1)
  assert.equal(host.entries('inspector.tab')[0].options.key, 'two')
})

test('community contributions can declare nested child slots recursively', () => {
  const host = createHost()
  const disposeParent = host.register(
    { name: 'card.accessory', kind: 'list' },
    {
      id: 'parent',
      children: { 'community.parent.body': { kind: 'list' } },
      render: () => null,
    },
  )
  assert.equal(host.count('community.parent.body'), 0)
  host.register(
    { name: 'community.parent.body', kind: 'list' },
    {
      id: 'child',
      children: { 'community.parent.leaf': { kind: 'list' } },
      render: () => null,
    },
  )
  host.register(
    { name: 'community.parent.leaf', kind: 'list' },
    { id: 'leaf', render: () => null },
  )
  assert.equal(host.count('community.parent.body'), 1)
  assert.equal(host.count('community.parent.leaf'), 1)
  disposeParent()
  assert.equal(host.count('community.parent.body'), 0)
  assert.equal(host.count('community.parent.leaf'), 0)
})

test('nested contributions render their own child slots and recover after the parent returns', () => {
  const host = createHost()
  const body = { name: 'community.parent.body', kind: 'list' }
  const leaf = { name: 'community.parent.leaf', kind: 'list' }
  const declarations = []
  let disposeChild
  let disposeLeaf
  const offBody = host.onDeclared(body, (available) => {
    declarations.push(['body', available])
    disposeChild?.()
    disposeChild = available ? host.register(body, {
      id: 'child',
      children: { [leaf.name]: { kind: 'list' } },
      render: ({ value, renderSlot }) => React.createElement(
        'article', null, renderSlot(leaf, { value: `${value}/child` }),
      ),
    }) : undefined
  })
  const offLeaf = host.onDeclared(leaf, (available) => {
    declarations.push(['leaf', available])
    disposeLeaf?.()
    disposeLeaf = available ? host.register(leaf, {
      id: 'leaf',
      render: ({ value }) => React.createElement('span', null, value),
    }) : undefined
  })
  const registerParent = (value) => host.register(
    { name: 'card.accessory', kind: 'list' },
    {
      id: 'parent',
      children: { [body.name]: { kind: 'list' } },
      render: ({ renderSlot }) => React.createElement(
        'section', null, renderSlot(body, { value }),
      ),
    },
  )
  const markup = () => renderToStaticMarkup(React.createElement(SlotOutlet, {
    host, slot: 'card.accessory',
  }))

  let disposeParent = registerParent('first')
  assert.equal(markup(), '<section><article><span>first/child</span></article></section>')
  assert.deepEqual(host.diagnostics, [])

  disposeParent()
  assert.equal(markup(), '')
  assert.equal(host.count(body.name), 0)
  assert.equal(host.count(leaf.name), 0)

  disposeParent = registerParent('second')
  assert.equal(markup(), '<section><article><span>second/child</span></article></section>')
  assert.equal(host.count(body.name), 1)
  assert.equal(host.count(leaf.name), 1)
  for (const name of ['body', 'leaf']) {
    assert.deepEqual(
      declarations.filter(([slot]) => slot === name).map(([, available]) => available),
      [false, true, false, true],
    )
  }
  assert.deepEqual(host.diagnostics, [])

  disposeParent()
  offBody()
  offLeaf()
})

test('a contribution without an explicit icon uses its plugin manifest icon URL', () => {
  const host = createHost()
  const panel = { name: 'sidebar.panel', kind: 'list' }
  host.setPluginIcon('plugin.a', '/api/plugins/plugin.a/icon/icon.svg')
  host.register(panel, { id: 'panel', title: 'Panel', render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  const [entry] = host.entriesWithIcons(panel)
  assert.equal(entry.icon.type, 'img')
  assert.equal(entry.icon.props.src, '/api/plugins/plugin.a/icon/icon.svg')
  assert.equal(entry.icon.props.className, 'kf-plugin-icon')
  const html = renderToStaticMarkup(React.createElement(PluginContributionIcon, {
    host, slot: panel, entry,
  }))
  assert.match(html, /<img class="kf-plugin-icon" src="\/api\/plugins\/plugin\.a\/icon\/icon\.svg"/)
})

test('an explicit contribution icon wins over the manifest icon and icons do not leak', () => {
  const host = createHost()
  const panel = { name: 'sidebar.panel', kind: 'list' }
  const explicit = React.createElement('span', { 'data-icon': 'explicit' })
  host.setPluginIcon('plugin.a', '/api/plugins/plugin.a/icon/icon.svg')
  host.register(panel, { id: 'explicit', title: 'Explicit', icon: explicit, render: () => null },
    { id: 'plugin.a', version: '1.0.0' })
  host.register(panel, { id: 'plain', title: 'Plain', render: () => null },
    { id: 'plugin.b', version: '1.0.0' })
  const entries = host.entriesWithIcons(panel)
  assert.equal(entries.find((entry) => entry.options.label === 'Explicit').icon, explicit)
  assert.equal(entries.find((entry) => entry.options.label === 'Plain').icon, undefined)
  assert.equal(host.pluginIconUrl(entries[0]), '/api/plugins/plugin.a/icon/icon.svg')
  assert.equal(host.pluginIconUrl(entries[1]), null)
})

test('a contribution cannot render an existing slot outside its own children', () => {
  for (const children of [undefined, { 'community.allowed': { kind: 'list' } }]) {
    const host = createHost()
    host.register({ name: 'toolbar.action', kind: 'list' }, {
      id: 'unrelated', render: () => React.createElement('span', null, 'must not render'),
    })
    host.register({ name: 'card.accessory', kind: 'list' }, {
      id: 'contribution', children,
      render: ({ renderSlot }) => React.createElement('section', null, renderSlot('toolbar.action')),
    })
    const markup = renderToStaticMarkup(React.createElement(SlotOutlet, {
      host, slot: 'card.accessory',
    }))
    assert.equal(markup, '<section></section>')
    assert.equal(host.diagnostics.length, 1)
    assert.match(host.diagnostics[0].message, /Undeclared child slot render: toolbar.action/)
    assert.equal(host.count('toolbar.action'), 1)
  }
})

test('three-level child slots actually render through each owner scope', () => {
  const host = createHost()
  const body = { name: 'community.render.body', kind: 'list' }
  const leaf = { name: 'community.render.leaf', kind: 'list' }
  host.register(
    { name: 'card.accessory', kind: 'list' },
    {
      id: 'parent',
      children: { [body.name]: { kind: 'list' } },
      render: ({ renderSlot }) => React.createElement('div', { id: 'parent' }, renderSlot(body, {})),
    },
  )
  host.register(body, {
    id: 'child',
    children: { [leaf.name]: { kind: 'list' } },
    render: ({ renderSlot }) => React.createElement('div', { id: 'child' }, renderSlot(leaf, {})),
  })
  host.register(leaf, {
    id: 'leaf',
    render: () => React.createElement('span', { id: 'leaf' }, 'leaf-content'),
  })
  const parentEntry = host.entries('card.accessory')[0]
  const html = renderToStaticMarkup(host.renderEntry(parentEntry, {}, null))
  assert.match(html, /id="parent"/)
  assert.match(html, /id="child"/)
  assert.match(html, /id="leaf"/)
  assert.match(html, /leaf-content/)
})
