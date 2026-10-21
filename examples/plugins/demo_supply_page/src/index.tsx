import * as React from 'react'
import { definePlugin, defineSlot } from '@kiforge/plugin-sdk'
import './style.css'

export const __react = React

const tabs = defineSlot<{ label?: string }>('demo.supply.tabs', 'keyed')

interface SupplierService {
  query(id: string): { id: string; name: string; price: number; stock: number }
  calls(): number
}

function SupplierPanel({
  supplier,
  renderSlot,
  showPrices,
}: {
  supplier: SupplierService
  renderSlot: (slot: typeof tabs) => React.ReactNode
  showPrices: boolean
}) {
  const [revision, setRevision] = React.useState(0)
  const part = supplier.query('C123')
  return (
    <section className="kf-card" data-testid="supply-page">
      <h3>供应信息 · {part.name}</h3>
      <div>
        编号 {part.id}
        {showPrices ? ` · 价格 ¥${part.price}` : ' · 价格已隐藏'} · 库存 {part.stock} · 查询{' '}
        {supplier.calls()} 次
      </div>
      <button onClick={() => setRevision((value) => value + 1)}>
        刷新视图（{revision}）
      </button>
      <div className="kf-tabs">{renderSlot(tabs)}</div>
    </section>
  )
}

export default definePlugin({
  name: 'demo.supply-page',
  inject: ['demo.supplier'],
  apply(ctx) {
    const supplier = ctx.contracts.require<SupplierService>('demo.supplier', '^1.0.0')
    const showPrices = ctx.config.get().showPrices !== false
    ctx.views.register({
      id: 'demo.supply',
      title: '供应信息',
      order: 20,
      slots: [tabs],
      render: ({ renderSlot }) => (
        <SupplierPanel supplier={supplier} renderSlot={renderSlot} showPrices={showPrices} />
      ),
    })
  },
})
