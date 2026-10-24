import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { fileURLToPath, URL } from 'node:url'

const here = (path) => fileURLToPath(new URL(path, import.meta.url))

// Build the host app, the prototype page and the shared runtime entries in one
// Rollup graph so React/Cordis/SlotCore are emitted once into shared chunks.
export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      '@kiforge/plugin-sdk': here('./packages/plugin-sdk/src/index.ts'),
    },
  },
  build: {
    rollupOptions: {
      // Runtime entries are imported by plugin bundles at runtime; their
      // exports must survive tree-shaking even though the host build does not
      // import every name itself.
      preserveEntrySignatures: 'strict',
      input: {
        main: here('./index.html'),
        prototype: here('./prototype/index.html'),
        'runtime/react': here('./src/runtime/react.js'),
        'runtime/react-dom': here('./src/runtime/react-dom.js'),
        'runtime/jsx-runtime': here('./src/runtime/jsx-runtime.js'),
        'runtime/i18n': here('./src/runtime/i18n.js'),
        'runtime/sdk': here('./src/runtime/sdk.js'),
      },
      output: {
        entryFileNames: (chunk) =>
          chunk.name.startsWith('runtime/') ? `${chunk.name}.js` : 'assets/[name]-[hash].js',
        chunkFileNames: 'assets/[name]-[hash].js',
        assetFileNames: 'assets/[name]-[hash][extname]',
        manualChunks(id) {
          if (id.includes('packages/plugin-sdk')) return 'plugin-sdk'
          if (id.includes('@deepseek-ai/cordis')) return 'cordis'
          if (id.includes('dsh-client-ui-slots')) return 'slot-core'
          if (id.includes('node_modules/i18next') || id.includes('node_modules/react-i18next')) return 'i18n'
          if (id.includes('node_modules/react-dom')) return 'react-dom'
          if (id.includes('node_modules/react')) return 'react'
        },
      },
    },
  },
})
