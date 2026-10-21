import { definePlugin } from '@kiforge/plugin-sdk'

// Test fixture: a second provider for an already-provided contract. The host
// must quarantine this plugin and keep the original provider/consumers alive.
export default definePlugin({
  name: 'test.duplicate-provider',
  apply(ctx) {
    ctx.contracts.provide('demo.supplier', '9.9.9', { duplicate: true })
  },
})
