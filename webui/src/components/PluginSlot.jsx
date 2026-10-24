import React, { useCallback, useEffect, useMemo, useState, useSyncExternalStore } from 'react'
import { appPluginRuntime } from '../plugin-host/app-host'
import { PluginEntry, SlotOutlet } from '../plugin-host/slot-host'

export function usePluginEntries(slot, host = appPluginRuntime.host) {
  const subscribe = useCallback((callback) => host.subscribe(slot, callback), [host, slot])
  const getSnapshot = useCallback(() => host.getVersion(slot), [host, slot])
  const version = useSyncExternalStore(subscribe, getSnapshot, getSnapshot)
  return useMemo(
    () => (host.entriesWithIcons ? host.entriesWithIcons(slot) : host.entries(slot)),
    [host, slot, version],
  )
}

export function PluginSections({ slot, ownerProps = {}, host = appPluginRuntime.host }) {
  const entries = usePluginEntries(slot, host)
  return entries.map((entry, index) => {
    const key = entry.options.key ?? entry.options.id ?? `${slot}:${index}`
    return (
      <section className="section plugin-section" key={key}>
        {entry.options.label ? <div className="section-title">{entry.options.label}</div> : null}
        <PluginEntry host={host} slot={slot} entry={entry} ownerProps={ownerProps} />
      </section>
    )
  })
}

export function PluginSlot({ slot, ownerProps = {}, host = appPluginRuntime.host }) {
  return <SlotOutlet host={host} slot={slot} ownerProps={ownerProps} />
}

export function PluginEntries({ slot, ownerProps = {}, host = appPluginRuntime.host }) {
  const entries = usePluginEntries(slot, host)
  return entries.map((entry, index) => (
    <PluginEntry
      key={entry.options.key ?? entry.options.id ?? `${slot}:${index}`}
      host={host}
      slot={slot}
      entry={entry}
      ownerProps={ownerProps}
    />
  ))
}

/**
 * Inspector tabs: no plugin tabs means the existing inspector is rendered
 * untouched (no tab bar). With plugin tabs, the default tab keeps the current
 * content and each plugin tab renders with its own error boundary.
 */
export function PluginTabs({ slot, ownerProps = {}, defaultLabel = '详情', children, host = appPluginRuntime.host }) {
  const entries = usePluginEntries(slot, host)
  const [active, setActive] = useState('__default__')

  useEffect(() => {
    if (active !== '__default__' && !entries.some((entry) => entry.options.key === active)) {
      setActive('__default__')
    }
  }, [active, entries])

  if (!entries.length) return children

  const activeEntry = entries.find((entry) => entry.options.key === active)
  return (
    <>
      <div className="plugin-tabs" role="tablist">
        <button
          role="tab"
          aria-selected={active === '__default__'}
          className={active === '__default__' ? 'active' : ''}
          onClick={() => setActive('__default__')}
        >
          {defaultLabel}
        </button>
        {entries.map((entry) => {
          const key = entry.options.key ?? entry.options.id
          return (
            <button
              key={key}
              role="tab"
              aria-selected={active === key}
              className={active === key ? 'active' : ''}
              onClick={() => setActive(key)}
            >
              {entry.options.label ?? key}
            </button>
          )
        })}
      </div>
      {activeEntry ? (
        <PluginEntry
          key={activeEntry.options.key ?? activeEntry.options.id}
          host={host}
          slot={slot}
          entry={activeEntry}
          ownerProps={ownerProps}
        />
      ) : (
        children
      )}
    </>
  )
}
