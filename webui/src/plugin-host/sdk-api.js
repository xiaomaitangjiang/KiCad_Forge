// SDK faces bound to one plugin fiber. Registration disposers are owned by the
// current Cordis fiber, so disabling a plugin releases its slots and views.

export function createSlotApi(host, ctx, identity) {
  return {
    register(slot, entry) {
      const dispose = host.register(slot, entry, identity)
      // Cordis effect() executes the callback immediately and treats its
      // return value as the disposer; passing the disposer directly would run
      // it at once.
      ctx.effect(() => dispose)
      return dispose
    },
    onDeclared(slot, listener) {
      const off = host.onDeclared(slot, listener)
      ctx.effect(() => off)
      return off
    },
  }
}

export function createViewApi(host, ctx, identity) {
  return {
    register(view) {
      const dispose = host.registerRootView(view, identity)
      ctx.effect(() => dispose)
      return dispose
    },
  }
}

export function createContractApi(contracts, identity, ctx) {
  return {
    provide(name, version, implementation) {
      const off = contracts.provide(identity, name, version, implementation)
      ctx.effect(() => off)
    },
    require(name, range = '*') {
      return contracts.require(identity, name, range)
    },
    get(name) {
      return contracts.get(name)
    },
  }
}

export function createDiagnosticsApi(host, identity) {
  return {
    report(report) {
      host.diagnostics.push({
        code: report?.code || 'plugin/report',
        severity: report?.severity || 'info',
        message: report?.message || '',
        detail: report?.detail,
        plugin: identity.id,
      })
    },
  }
}
