#!/usr/bin/env node
// Build the minimal SDK and every example plugin. The host is intentionally
// not touched: plugin sources are not inputs to webui/dist.

import { spawnSync } from 'node:child_process'
import { existsSync, readdirSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const repoRoot = path.resolve(here, '..')
const tsc = path.join(repoRoot, 'webui', 'node_modules', 'typescript', 'bin', 'tsc')
const pluginBuild = path.join(repoRoot, 'webui', 'scripts', 'build-plugin.mjs')
const examples = path.join(repoRoot, 'examples', 'plugins')

function run(command, args, cwd) {
  const result = spawnSync(command, args, { cwd, stdio: 'inherit', shell: false })
  if (result.error) throw result.error
  if (result.status !== 0) throw new Error(`${command} ${args.join(' ')} exited ${result.status}`)
}

if (!existsSync(tsc)) throw new Error(`TypeScript is missing: ${tsc}`)
run(process.execPath, [tsc, '-p', path.join(repoRoot, 'webui', 'packages', 'plugin-sdk')], repoRoot)

for (const entry of readdirSync(examples, { withFileTypes: true })) {
  if (!entry.isDirectory()) continue
  const dir = path.join(examples, entry.name)
  if (!existsSync(path.join(dir, 'manifest.json'))) continue
  console.log(`  -> plugin ${entry.name}`)
  run(process.execPath, [pluginBuild, dir], repoRoot)
}

// First-party plugins keep their frontend bundle inside the plugin directory;
// the host never imports or bundles these sources.
const firstPartyFrontends = [
  path.join(repoRoot, 'plugins', 'git_version', 'frontend'),
  path.join(repoRoot, 'plugins', 'lcsc_import', 'frontend'),
]
for (const dir of firstPartyFrontends) {
  if (!existsSync(path.join(dir, 'package.json'))) continue
  console.log(`  -> plugin frontend ${path.relative(repoRoot, dir)}`)
  run(process.execPath, [pluginBuild, dir], repoRoot)
}
