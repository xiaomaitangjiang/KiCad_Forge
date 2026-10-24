import { definePlugin } from '@kiforge/plugin-sdk'

// Test fixture: requires a contract major that the host does not provide.
// The loader must reject only this plugin before activation.
export default definePlugin({
  name: 'test.bad-version',
  apply(ctx) {
    ctx.contracts.require('demo.supplier', '^2.0.0')
  },
})
