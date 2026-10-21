// Thin React binding over the published SlotCore registry.
//
// SlotCore owns registration, declarations, ownership and disposal. This file
// only projects its snapshots into React and renders entry components. It is
// intentionally small: no Cordis, no store, no factory support in phase 1.

import React from 'react'
import { SlotCore } from '@deepseek-ai/dsh-client-ui-slots'

function EntryContent({ renderEntry }) {
  return renderEntry()
}

class ContributionIconBoundary extends React.Component {
  constructor(props) {
    super(props)
    this.state = { error: null }
  }

  static getDerivedStateFromError(error) {
    return { error }
  }

  componentDidCatch(error) {
    const meta = this.props.entry?.contribution ?? this.props.host?.contributionFor?.(this.props.entry)
    this.props.host?.diagnostics.push({
      code: 'plugin/icon-error',
      slot: this.props.slot,
      plugin: meta?.pluginId ?? null,
      localId: meta?.localId ?? null,
      message: error?.message || String(error),
    })
  }

  render() {
    if (this.state.error) return this.props.fallback ?? null
    return this.props.children
  }
}

class EntryBoundary extends React.Component {
  constructor(props) {
    super(props)
    this.state = { error: null }
  }

  static getDerivedStateFromError(error) {
    return { error }
  }

  componentDidCatch(error) {
    this.props.host.reportEntryError(this.props.slot, this.props.entry, error)
  }

  render() {
    if (this.state.error) {
      return React.createElement(
        'div',
        { className: 'kf-plugin-error' },
        `插件贡献渲染失败：${this.state.error?.message || String(this.state.error)}`,
      )
    }
    return React.createElement(EntryContent, { renderEntry: this.props.renderEntry })
  }
}

function slotKey(slot) {
  return typeof slot === 'string' ? slot : slot?.name
}

export function createSlotHost({
  viewSlot = 'demo.prototype.page',
  auxSlot = 'demo.prototype.aux',
  additionalSlots = [],
  shellRender = null,
} = {}) {
  const core = new SlotCore()
  const snapshots = new Map()
  const diagnostics = []

  function kindOf(key) {
    return core.specDynamic(key)?.kind ?? (key === 'root' ? 'list' : 'list')
  }

  function subscribe(key, callback) {
    return core.subscribe(key, callback)
  }

  function getVersion(key) {
    return core.getVersion(key)
  }

  // Stable snapshot per version for useSyncExternalStore.
  function entries(key) {
    const version = core.getVersion(key)
    const cached = snapshots.get(key)
    if (cached && cached.version === version) return cached.entries
    const list = [...core.entriesOfSlot(key)]
    if (kindOf(key) === 'list' || key === 'root') {
      // Same-order contributions need a deterministic tie-breaker; a stable
      // contribution identity is independent of load order and locale.
      list.sort((a, b) => {
        const order = (a.options.order ?? 0) - (b.options.order ?? 0)
        if (order) return order
        const left = String(a.options.id ?? '')
        const right = String(b.options.id ?? '')
        return left < right ? -1 : left > right ? 1 : 0
      })
    }
    const snapshot = Object.freeze(list)
    snapshots.set(key, { version, entries: snapshot })
    return snapshot
  }

  function reportEntryError(key, entry, error) {
    const meta = contributionFor(entry)
    diagnostics.push({
      code: 'plugin/render-error',
      slot: key,
      plugin: meta?.pluginId ?? null,
      localId: meta?.localId ?? null,
      entry: entry?.options?.id ?? entry?.options?.key,
      message: error?.message || String(error),
    })
    try {
      // Keep the failing cell registered so its local fallback can stay visible;
      // the owning plugin can retry/re-register on recovery.
      core.reportEntryError(key, entry, error)
    } catch {
      // The entry may already be gone; the diagnostic above is the useful part.
    }
  }

  function renderSlotFor(entry) {
    return (slot, props) => {
      const key = slotKey(slot)
      if (!key) return null
      if (entry && !Object.hasOwn(entry.children ?? {}, key)) {
        reportEntryError(key, entry, new Error(`Undeclared child slot render: ${key}`))
        return null
      }
      return React.createElement(SlotOutlet, {
        key: `${key}:${entry?.options?.id ?? entry?.options?.key ?? 'root'}`,
        host: hostApi,
        slot: key,
        ownerProps: props ?? {},
        parentEntry: entry ?? null,
      })
    }
  }

  function renderEntry(entry, ownerProps) {
    const component = entry.component
    if (typeof component !== 'function') {
      return React.createElement('div', { className: 'kf-plugin-error' }, '插件贡献不是可渲染组件')
    }
    return component({
      ...(ownerProps ?? {}),
      renderSlot: renderSlotFor(entry),
    })
  }

  // SlotCore only projects id/label/order/kind into options. Identity and
  // React icons are bound to the actual registered entry object, so a failed
  // duplicate registration or an old disposer cannot pollute a newer entry.
  const entryMeta = new WeakMap()
  const entryIcons = new WeakMap()
  const pluginIcons = new Map()

  function contributionId(identity, key, localId) {
    return `${identity?.id ? `plugin:${identity.id}` : 'host'}:${key}:${localId}`
  }

  function register(slot, entry, identity = null) {
    const key = slotKey(slot)
    if (!key) throw new Error('slot definition has no name')
    const localId = entry.id ?? entry.key
    if (localId === undefined || localId === null || localId === '') {
      throw new Error('slot contribution requires an explicit id or key')
    }
    const internalId = contributionId(identity, key, localId)
    const options = {
      name: key,
      id: internalId,
      key: slot.kind === 'keyed' ? entry.key ?? entry.id : undefined,
      order: entry.order,
      label: entry.title,
      children: entry.children,
    }
    const component = (props) => entry.render(props)
    const dispose = core.register(options, component)
    const registered = core.entriesOfSlot(key).find((candidate) => candidate.options.id === internalId)
    if (registered) {
      entryMeta.set(registered, {
        pluginId: identity?.id ?? null,
        localId,
        slot: key,
        contributionId: internalId,
      })
      if (entry.icon !== undefined) entryIcons.set(registered, entry.icon)
    }
    return dispose
  }

  function contributionFor(entry) {
    return entryMeta.get(entry) ?? entry?.contribution ?? null
  }

  /** Register the icon declared by a plugin manifest. Contributions that do
   * not ship their own React icon inherit this host-resolved asset URL; the
   * host never hard-codes a plugin id or SVG. */
  function setPluginIcon(pluginId, url) {
    if (!pluginId) return
    if (typeof url === 'string' && url.trim()) pluginIcons.set(pluginId, url.trim())
    else pluginIcons.delete(pluginId)
  }

  function pluginIconUrl(entry) {
    const pluginId = contributionFor(entry)?.pluginId
    return pluginId ? pluginIcons.get(pluginId) ?? null : null
  }

  function iconFor(entry) {
    const explicit = entryIcons.get(entry)
    if (explicit !== undefined) return explicit
    const url = pluginIconUrl(entry)
    if (!url) return undefined
    return React.createElement('img', {
      className: 'kf-plugin-icon',
      src: url,
      alt: '',
      'aria-hidden': 'true',
      draggable: false,
      onError: (event) => { event.currentTarget.hidden = true },
    })
  }

  function entriesWithIcons(slot) {
    const key = slotKey(slot)
    if (!key) return []
    return entries(key).map((entry) => {
      const icon = iconFor(entry)
      const contribution = contributionFor(entry)
      return icon === undefined && contribution === null
        ? entry
        : { ...entry, icon, contribution }
    })
  }

  function registerShell() {
    const children = {
      [viewSlot]: { kind: 'list', scope: 'root' },
      [auxSlot]: { kind: 'list', scope: 'root' },
    }
    for (const definition of additionalSlots) {
      children[definition.name] = { kind: definition.kind ?? 'list', scope: 'root' }
    }
    let shellEntry = null
    const component = () =>
      shellRender
        ? shellRender({
            host: hostApi,
            renderSlot: renderSlotFor(shellEntry),
          })
        : React.createElement(
            'div',
            { className: 'prototype-content' },
            React.createElement(SlotOutlet, {
              host: hostApi,
              slot: viewSlot,
              ownerProps: {},
              parentEntry: shellEntry,
            }),
            React.createElement(SlotOutlet, {
              host: hostApi,
              slot: auxSlot,
              ownerProps: {},
              parentEntry: shellEntry,
            }),
          )
    const dispose = core.register({ name: 'root', id: 'prototype.shell', children }, component)
    shellEntry = core.entries('root').find((entry) => entry.options.id === 'prototype.shell') ?? null
    return dispose
  }

  function registerRootView(view, identity) {
    const children = {}
    for (const child of view.slots ?? []) {
      children[child.name] = { kind: child.kind, scope: 'root' }
    }
    const id = `view:${identity.id}:${view.id}`
    let entryRef = null
    const component = () =>
      view.render({
        renderSlot: renderSlotFor(entryRef),
      })
    const dispose = core.register(
      { name: viewSlot, id, order: view.order ?? 0, label: view.title, children },
      component,
    )
    entryRef = core.entries(viewSlot).find((entry) => entry.options.id === id) ?? null
    if (entryRef) {
      entryMeta.set(entryRef, {
        pluginId: identity.id,
        localId: view.id,
        slot: viewSlot,
        contributionId: id,
      })
    }
    return dispose
  }

  function onDeclared(slot, listener) {
    const key = slotKey(slot)
    if (!key) throw new Error('slot definition has no name')
    const notify = () => listener(!!core.specDynamic(key))
    const off = core.subscribeDeclaration(key, notify)
    notify()
    return off
  }

  const hostApi = {
    subscribe,
    getVersion,
    entries,
    entriesWithIcons,
    contributionFor,
    setPluginIcon,
    pluginIconUrl,
    register,
    registerShell,
    registerRootView,
    onDeclared,
    reportEntryError,
    renderEntry,
    count: (key) => core.entriesOfSlot(key).length,
    kindOf,
    diagnostics,
    core,
  }

  return hostApi
}

export function SlotOutlet({ host, slot, ownerProps = {}, parentEntry = null }) {
  const key = slotKey(slot)
  const subscribe = React.useCallback((callback) => host.subscribe(key, callback), [host, key])
  const getSnapshot = React.useCallback(() => host.getVersion(key), [host, key])
  const version = React.useSyncExternalStore(subscribe, getSnapshot, getSnapshot)
  const list = React.useMemo(() => host.entries(key), [host, key, version])

  if (!list.length) return null

  return React.createElement(
    React.Fragment,
    null,
    list.map((entry, index) =>
      React.createElement(
        EntryBoundary,
        {
          key: entry.options.key ?? entry.options.id ?? `${key}:${index}`,
          host,
          slot: key,
          entry,
          renderEntry: () => host.renderEntry(entry, ownerProps, parentEntry),
        },
      ),
    ),
  )
}

/** Render an activity icon behind its own boundary; a broken icon degrades to
 * the caller's fallback without taking down the activity bar. */
export function PluginContributionIcon({ host, slot, entry, fallback = null }) {
  return React.createElement(
    ContributionIconBoundary,
    { host, slot, entry, fallback },
    entry?.icon ?? fallback,
  )
}

/** Render one host entry with its own error boundary (tabs, views, menus). */
export function PluginEntry({ host, slot, entry, ownerProps = {}, parentEntry = null }) {
  return React.createElement(EntryBoundary, {
    host,
    slot,
    entry,
    renderEntry: () => host.renderEntry(entry, ownerProps, parentEntry),
  })
}
