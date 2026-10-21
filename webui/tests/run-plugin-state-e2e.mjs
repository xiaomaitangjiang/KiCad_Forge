// Restart-persistence check in a disposable portable package.
// KFORGE_STATE_E2E=1 KFORGE_BUILD_ROOT=<empty-data package> node <this file>
import { check, requireOptIn, runAppFixture } from './plugin-e2e-harness.mjs'

const pluginId = 'com.kicad_forge.lcsc_import'

async function main() {
  if (!requireOptIn('KFORGE_STATE_E2E')) return
  await runAppFixture('plugin-state', async ({ launch, stop, json }) => {
    const stateUrl = (origin) => `${origin}/api/plugins/${pluginId}/state`
    const setEnabled = (origin, enabled) => json(stateUrl(origin), {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ enabled }),
    })
    const first = await launch({ connect: false })
    try {
      const initial = await json(stateUrl(first.origin))
      check(initial.ok === true && initial.enabled === true, `clean fixture plugin unavailable: ${JSON.stringify(initial)}`)
      const disabled = await setEnabled(first.origin, false)
      check(disabled.ok === true && disabled.enabled === false, `disable failed: ${JSON.stringify(disabled)}`)
      check(Number.isInteger(disabled.revision) && disabled.revision > initial.revision, `disable revision did not advance: ${JSON.stringify(disabled)}`)
    } finally { await stop(first) }

    // Reuse this fixture's data only, after the first process has fully exited.
    const second = await launch({ connect: false })
    try {
      const persisted = await json(stateUrl(second.origin))
      check(persisted.ok === true && persisted.enabled === false, `enabled state did not persist: ${JSON.stringify(persisted)}`)
      check(persisted.revision === 0, `process-local revision did not reset: ${JSON.stringify(persisted)}`)
      const reenabled = await setEnabled(second.origin, true)
      check(reenabled.ok === true && reenabled.enabled === true, `re-enable failed: ${JSON.stringify(reenabled)}`)
    } finally { await stop(second) }
  })
}

main().catch((error) => {
  console.error(error.stack || error.message)
  process.exitCode = 1
})
