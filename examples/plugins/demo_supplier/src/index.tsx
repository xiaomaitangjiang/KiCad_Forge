import * as React from 'react'
import { definePlugin } from '@kiforge/plugin-sdk'

// Identity probes used by the prototype: the plugin must see the same React
// module as the host. Namespace objects differ from the default export even
// for one module, so compare a function identity instead.
export const __react = React
export const __reactUseState = React.useState
export const __reactVersion = React.version

interface SupplierService {
  version: number
  query(id: string): { id: string; name: string; price: number; stock: number }
  calls(): number
}

export default definePlugin({
  name: 'demo.supplier',
  apply(ctx) {
    let calls = 0
    const service: SupplierService = {
      version: 1,
      query(id) {
        calls += 1
        return {
          id,
          name: `Mock Part ${id}`,
          price: 1.23,
          stock: 42,
        }
      },
      calls: () => calls,
    }
    ctx.provide('demo.supplier', service)
    ctx.contracts.provide('demo.supplier', '1.0.0', service)
    ctx.logger.info('demo.supplier ready')
  },
})
