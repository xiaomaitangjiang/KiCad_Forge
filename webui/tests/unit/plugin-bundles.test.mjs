import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(here, '..', '..', '..')

const bundles = [
  'demo_supplier',
  'demo_supply_page',
  'demo_supply_tab',
]

for (const name of bundles) {
  test(`${name}: bundle externalizes React/JSX/SDK`, () => {
    const file = path.join(repoRoot, 'examples', 'plugins', name, 'dist', 'index.js')
    const text = readFileSync(file, 'utf8')
    assert.match(text, /from "react"/, 'React must stay external')
    assert.match(text, /@kiforge\/plugin-sdk/, 'SDK must stay external')
    assert.doesNotMatch(text, /react-dom/, 'ReactDOM must not be bundled')
    // A bundled React would add the production marker and large runtime code.
    assert.ok(text.length < 20_000, `bundle unexpectedly large: ${text.length} bytes`)
  })
}
