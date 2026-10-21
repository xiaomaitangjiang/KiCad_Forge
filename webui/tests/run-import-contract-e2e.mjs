// Offline public SDK -> HTTP -> Python staging -> host file/DB commit.
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { check, requireOptIn, runAppFixture } from './plugin-e2e-harness.mjs'

const providerId = 'test.contract-provider'
const consumerId = 'test.contract-consumer'

function stageFixtures(root) {
  const provider = path.join(root, 'plugins', providerId)
  const consumer = path.join(root, 'plugins', consumerId)
  mkdirSync(provider, { recursive: true })
  mkdirSync(path.join(consumer, 'dist'), { recursive: true })
  const base = { manifestVersion: 2, version: '1.0.0', license: 'MIT' }
  writeFileSync(path.join(provider, 'manifest.json'), JSON.stringify({ ...base,
    id: providerId, name: providerId, python: { entry: 'plugin.py', actions: [
      { id: 'inspect_part', name: 'Inspect', cancelable: true },
      { id: 'commit_staged', name: 'Import', cancelable: true,
        protocol: 'staged-import-v1', analyze_action: 'inspect_part' },
    ] },
  }))
  writeFileSync(path.join(provider, 'plugin.py'), String.raw`
import json, sys
from pathlib import Path
root = Path(__file__).parent
action, args = sys.argv[1], json.loads(sys.argv[2])
external = args.get('lcsc_id') or args.get('source')
if action == 'info':
    print((root / 'manifest.json').read_text(encoding='utf-8'))
elif action == 'inspect_part':
    candidates = json.loads((root / 'candidates.json').read_text()) if (root / 'candidates.json').exists() else []
    print(json.dumps({'ok': True, 'lcsc_id': external, 'symbol_names': ['Part_' + external],
                      'footprint_name': 'ContractFP', 'candidates': candidates}))
elif action == 'commit_staged':
    assert args['protocol'] == 'staged-import-v1'
    staging = Path(args['staging_dir'])
    name = 'Part_' + external
    text = '(kicad_symbol_lib (version 20231120) (symbol "' + name + '" (property "Reference" "U") (property "Value" "' + name + '") (symbol "' + name + '_1_1" (pin passive line (at 0 0 0) (length 2.54) (name "A") (number "1")) (pin passive line (at 0 -2.54 0) (length 2.54) (name "B") (number "2")))))'
    (staging / 'symbol.kicad_sym').write_text(text, encoding='utf-8')
    mode = args['footprint_choice']['mode']
    footprint = None
    if mode == 'new':
        (staging / 'footprint.kicad_mod').write_text('(footprint "ContractFP" (version 20240108) (layer "F.Cu") (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu" "F.Paste" "F.Mask")) (pad "2" smd rect (at 2 0) (size 1 1) (layers "F.Cu" "F.Paste" "F.Mask")))', encoding='utf-8')
        footprint = {'name': 'ContractFP', 'file': 'footprint.kicad_mod'}
    (staging / 'manifest.json').write_text(json.dumps({'protocol': 'staged-import-v1',
        'lcsc_id': external, 'symbols': [{'name': name, 'file': 'symbol.kicad_sym'}],
        'footprint': footprint, 'footprint_choice': mode}), encoding='utf-8')
    print(json.dumps({'ok': True, 'staged': True}))
else:
    raise RuntimeError('Unexpected provider action: ' + action)
`)
  writeFileSync(path.join(consumer, 'manifest.json'), JSON.stringify({ ...base,
    id: consumerId, name: consumerId, engines: { sdk: '^1.0.0' },
    frontend: { root: 'dist', entry: 'index.js' },
    python: { entry: 'plugin.py', actions: [{ id: 'identity', name: 'Identity', cancelable: true }] },
  }))
  writeFileSync(path.join(consumer, 'plugin.py'), `import json\nprint(json.dumps({'ok': True, 'owner': '${consumerId}'}))\n`)
  writeFileSync(path.join(consumer, 'dist', 'index.js'), `export default {
    name: '${consumerId}', apply(ctx) {
      window.__IMPORT_CONTRACT_FIXTURE__ = {
        analyze: (input) => ctx.imports.analyze(input),
        operation: (id) => ctx.imports.operation(id),
        symbol: (id) => ctx.symbols.get(id),
        symbols: (query) => ctx.symbols.list(query),
        binding: (id) => ctx.symbols.binding(id),
        matches: (id) => ctx.symbols.matches(id),
        symbolLibraries: () => ctx.libraries.symbolLibraries(),
        componentLibraries: () => ctx.libraries.componentLibraries(),
        catalog: (id) => ctx.libraries.componentSymbolLibraries(id),
        footprints: (q) => ctx.footprints.search(q),
        async matchError(id) {
          try { await ctx.symbols.matches(id); return { ok: true } }
          catch (error) { return { ok: false, status: error.status } }
        },
        async commit(input) {
          const handle = await ctx.imports.commit(input)
          const initial = handle.getSnapshot()
          if (handle.getSnapshot() !== initial) throw new Error('Unstable initial snapshot')
          const terminal = await handle.done
          if (handle.getSnapshot() !== terminal) throw new Error('Unstable terminal snapshot')
          return terminal
        },
        async identity() { const handle = await ctx.tasks.run('identity'); return handle.done },
      }
      ctx.on('dispose', () => { delete window.__IMPORT_CONTRACT_FIXTURE__ })
    },
  }`)
  return provider
}

async function main() {
  if (!requireOptIn('KFORGE_IMPORTS_E2E')) return
  await runAppFixture('import-contract', async ({ root, results, launch, stop, json, waitFor }) => {
    const provider = stageFixtures(root)
    const symbols = path.join(root, 'contract-library', 'symbols')
    const footprints = path.join(root, 'contract-library', 'Contract.pretty')
    mkdirSync(symbols, { recursive: true })
    mkdirSync(footprints)
    const session = await launch()
    try {
      const { origin, cdp } = session
      const added = await json(`${origin}/api/component-libraries`, { method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ action: 'add', name: 'ContractLibrary', symbol_path: symbols, footprint_path: footprints }),
      })
      check(added.ok === true && added.library?.id, `package registration failed: ${JSON.stringify(added)}`)
      await waitFor(() => cdp.eval('Boolean(window.__IMPORT_CONTRACT_FIXTURE__)'), 20000, 'public imports consumer')
      const call = (method, input) => cdp.eval(`window.__IMPORT_CONTRACT_FIXTURE__.${method}(${JSON.stringify(input)})`)
      const identity = await call('identity')
      check(identity.state === 'succeeded' && identity.result?.owner === consumerId,
        `imports changed the caller tasks identity: ${JSON.stringify(identity)}`)
      const firstInput = { provider: providerId, externalId: 'C10001' }
      const analysis = await call('analyze', firstInput)
      check(analysis.externalId === firstInput.externalId && analysis.symbolNames?.[0] === 'Part_C10001'
        && analysis.footprintName === 'ContractFP', `bad public analysis: ${JSON.stringify(analysis)}`)
      const selection = { ...firstInput, targetComponentLibraryId: added.library.id,
        symbolLibraryChoice: { mode: 'new', name: 'Selected' }, footprintChoice: { mode: 'new' },
        operationId: 'contract-new-10001' }
      const committed = await call('commit', selection)
      check(committed.state === 'succeeded' && committed.result?.ok === true,
        `full staged import failed: ${JSON.stringify(committed)}`)
      const result = committed.result
      check(result.operationId === selection.operationId && result.symbolId && result.footprintId,
        `unmapped public import result: ${JSON.stringify(result)}`)
      const symbolFile = path.join(symbols, 'Selected.kicad_sym')
      check(existsSync(symbolFile) && readFileSync(symbolFile, 'utf8').includes('Part_C10001'), 'selected sublibrary not written')
      const originalSymbol = readFileSync(symbolFile, 'utf8')
      const footprintFile = path.join(footprints, 'ContractFP.kicad_mod')
      check(existsSync(footprintFile), 'new footprint not committed')
      const originalFootprint = readFileSync(footprintFile, 'utf8')
      const operation = await call('operation', selection.operationId)
      check(operation.symbolId === result.symbolId && operation.operationId === selection.operationId, 'operation lookup lost result identity')
      const replay = await call('commit', selection)
      check(replay.state === 'succeeded' && replay.result?.symbolId === result.symbolId && replay.result?.replayed === true,
        `operation replay was not idempotent: ${JSON.stringify(replay)}`)
      check(readFileSync(symbolFile, 'utf8') === originalSymbol
        && readFileSync(footprintFile, 'utf8') === originalFootprint, 'operation replay changed committed files')
      writeFileSync(path.join(provider, 'candidates.json'), JSON.stringify([
        { id: result.footprintId, name: 'ContractFP', library: 'Contract', pad_count: 2 },
      ]))
      const secondInput = { provider: providerId, externalId: 'C10002' }
      const secondAnalysis = await call('analyze', secondInput)
      check(secondAnalysis.candidates?.[0]?.footprintId === result.footprintId, 'candidate UUID was not normalized')
      const reused = await call('commit', { ...secondInput, targetComponentLibraryId: added.library.id,
        symbolLibraryChoice: { mode: 'existing', path: 'Selected.kicad_sym' },
        footprintChoice: { mode: 'reuse', footprintId: secondAnalysis.candidates[0].footprintId },
        operationId: 'contract-reuse-10002' })
      check(reused.state === 'succeeded' && reused.result?.mode === 'reuse' && reused.result?.footprintId === result.footprintId,
        `candidate reuse failed: ${JSON.stringify(reused)}`)
      check(readFileSync(symbolFile, 'utf8').includes('Part_C10002'), 'existing selected sublibrary was not updated')
      check(readFileSync(footprintFile, 'utf8') === originalFootprint, 'reuse changed the selected footprint file')
      check(readdirSync(symbols).filter((name) => name.endsWith('.kicad_sym')).join() === 'Selected.kicad_sym', 'import escaped the selected sublibrary')
      for (const imported of [result, reused.result]) {
        const binding = await json(`${origin}/api/symbols/${encodeURIComponent(imported.symbolId)}/binding`)
        check(binding.footprint_id === result.footprintId && binding.footprint_status === 'available',
          `database binding did not match the committed files: ${JSON.stringify(binding)}`)
      }
      const summary = await call('symbol', result.symbolId)
      check(summary?.id === result.symbolId && summary.name === 'Part_C10001'
        && typeof summary.libraryId === 'string' && summary.libraryId && summary.locked === false,
        `public single-symbol DTO mismatch: ${JSON.stringify(summary)}`)
      check(await call('symbol', 'missing-symbol') === null, 'missing symbol must be null')
      const rows = await call('symbols', { q: 'Part_C10001', libraryId: summary.libraryId })
      check(rows.length === 1 && rows[0].id === result.symbolId, 'combined public query lost its target')
      check((await call('symbols', { q: 'Part_C10001', libraryId: 'missing-library' })).length === 0,
        'combined public query escaped its library filter')
      const binding = await call('binding', result.symbolId)
      check(binding.symbolId === result.symbolId && binding.footprintId === result.footprintId
        && binding.footprintStatus === 'available', `public binding mismatch: ${JSON.stringify(binding)}`)
      check(await call('binding', 'missing-symbol') === null, 'missing binding must be null')
      const missingMatches = await call('matchError', 'missing-symbol')
      check(missingMatches.ok === false && missingMatches.status === 404, 'missing match query looked like an empty success')
      check(Array.isArray(await call('matches', result.symbolId)), 'match suggestions must be an array')
      const candidates = await call('footprints', 'ContractFP')
      check(candidates.some((item) => item.footprintId === result.footprintId
        && item.padCount === 2 && item.footprintStatus === 'available'), 'public footprint query DTO mismatch')
      const libraries = await call('symbolLibraries')
      check(libraries.some((item) => item.id === summary.libraryId && item.path === symbolFile
        && item.symbolCount === 2), 'public symbol-library DTO mismatch')
      const packages = await call('componentLibraries')
      check(packages.some((item) => item.id === added.library.id && item.symbolPath === symbols
        && item.footprintPath === footprints), 'public component-library DTO mismatch')
      const direct = path.join(root, 'Direct 元件.KICAD_SYM')
      writeFileSync(direct, '(kicad_symbol_lib (version 20231120))\n')
      const directPackage = await json(`${origin}/api/component-libraries`, { method: 'POST',
        headers: { 'content-type': 'application/json' }, body: JSON.stringify({ action: 'add', name: 'DirectFile',
          symbol_path: direct, footprint_path: path.join(root, 'Direct.pretty') }),
      })
      check(directPackage.ok === true && directPackage.library?.id, 'single-file package registration failed')
      const catalog = await call('catalog', directPackage.library.id)
      check(catalog.canCreate === false && catalog.libraries.length === 1
        && catalog.libraries[0].id === '' && catalog.libraries[0].path === 'Direct 元件.KICAD_SYM',
        `single-file/unregistered catalog mismatch: ${JSON.stringify(catalog)}`)
      check(await call('operation', 'contract-operation-missing') === null, 'unknown operation must return null')
      check(session.errors.length === 0, `browser errors: ${session.errors.join(' | ')}`)
      writeFileSync(path.join(results, 'import-contract.tasks.json'), JSON.stringify({ analysis, committed, replay, reused }, null, 2))
    } finally { await stop(session) }
  })
}

main().catch((error) => { console.error(error.stack || error.message); process.exitCode = 1 })
