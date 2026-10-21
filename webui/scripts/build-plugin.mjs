import { build } from 'vite'
import react from '@vitejs/plugin-react'
import { resolve } from 'node:path'
import { existsSync } from 'node:fs'

// Phase-1 shared build driver. Each plugin still owns its package.json,
// tsconfig and lock; the host neither bundles nor rebuilds the plugin.
const dir = resolve(process.argv[2] || '.')
const entry = ['src/index.tsx', 'src/index.ts'].map((name) => resolve(dir, name)).find(existsSync)
if (!entry) {
  console.error(`No src/index.tsx or src/index.ts under ${dir}`)
  process.exit(1)
}

await build({
  configFile: false,
  root: dir,
  plugins: [react()],
  build: {
    target: 'es2022',
    minify: false,
    emptyOutDir: true,
    lib: {
      entry,
      formats: ['es'],
      fileName: () => 'index.js',
      cssFileName: 'style',
    },
    rollupOptions: {
      external: [
        'react',
        'react-dom',
        'react-dom/client',
        'react/jsx-runtime',
        'react-i18next',
        '@kiforge/plugin-sdk',
      ],
    },
  },
})
