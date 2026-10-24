import * as React from 'react'
import { definePlugin, defineSlot } from '@kiforge/plugin-sdk'

export const __react = React

const tabs = defineSlot<{ label?: string }>('demo.supply.tabs', 'keyed')
const aux = defineSlot('demo.prototype.aux', 'list')

interface SupplierService {
  query(id: string): { id: string; name: string; price: number; stock: number }
  calls(): number
}

function InventoryTab({ supplier }: { supplier: SupplierService }) {
  const part = supplier.query('C123')
  return (
    <div className="kf-tab" data-testid="supply-tab">
      <strong>库存页签</strong> · {part.id} 可用 {part.stock} 件
    </div>
  )
}

function SupplierStatus({ supplier }: { supplier: SupplierService }) {
  return (
    <section className="kf-card" data-testid="supplier-status">
      <h3>供应商状态（独立于供应页）</h3>
      <div>服务版本可用 · 累计查询 {supplier.calls()} 次</div>
    </section>
  )
}

export default definePlugin({
  name: 'demo.supply-tab',
  inject: ['demo.supplier'],
  apply(ctx) {
    const supplier = ctx.contracts.require<SupplierService>('demo.supplier', '^1.0.0')
    let tabDisposer: (() => void) | null = null

    function registerTab() {
      if (tabDisposer) return
      tabDisposer = ctx.slots.register(tabs, {
        id: 'inventory',
        key: 'inventory',
        title: '库存',
        order: 10,
        render: () => <InventoryTab supplier={supplier} />,
      })
    }

    const off = ctx.slots.onDeclared(tabs, (declared) => {
      if (declared) {
        // Declaration creation is the only signal needed: a collapse turns the
        // old registration into a no-op and a later creation must register once.
        tabDisposer = null
        registerTab()
      } else {
        // The parent declaration collapsed; the registration was cascaded by
        // SlotCore. Drop the stale handle so a later declaration registers once.
        tabDisposer = null
      }
    })
    ctx.on('dispose', off)

    ctx.slots.register(aux, {
      id: 'supply-status',
      title: '供应商状态',
      order: 100,
      render: () => <SupplierStatus supplier={supplier} />,
    })
  },
})
