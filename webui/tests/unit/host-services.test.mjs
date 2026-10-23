import { test } from 'node:test'
import assert from 'node:assert/strict'
import { createHostServices } from '../../src/plugin-host/host-services.js'
import { api } from '../../src/api.js'

function services(api) {
  return createHostServices({ api })
}

test('symbols list maps the actual HTTP DTO and preserves zero, false and empty text', async () => {
  const dto = {
    id: 'sym-1', name: 'R_0', library_id: 'lib-1', type_id: 'resistor', type: 'Resistor',
    mpn: '', value: '0', footprint: '', pins: 0, locked: false,
  }
  const before = structuredClone(dto)
  const host = services({ symbols: async (...args) => {
    assert.deepEqual(args, ['R', 'lib-1'])
    return [dto]
  } })
  assert.deepEqual(await host.symbols.list({ q: 'R', libraryId: 'lib-1' }), [{
    id: 'sym-1', name: 'R_0', libraryId: 'lib-1', typeId: 'resistor', typeName: 'Resistor',
    mpn: '', value: '0', footprint: '', pins: 0, locked: false,
  }])
  assert.deepEqual(dto, before, 'the adapter must not mutate the raw API response')
})

test('symbols get uses one single-symbol request and maps its response', async () => {
  const host = services({
    symbol: async (id) => {
      assert.equal(id, 'sym-1')
      return { id, name: 'R', library_id: 'lib-1' }
    },
    symbols: () => { throw new Error('get must not fetch the complete symbol list') },
  })
  assert.deepEqual(await host.symbols.get('sym-1'), { id: 'sym-1', name: 'R', libraryId: 'lib-1' })
})

test('binding maps identities and status, including a valid unbound symbol', async () => {
  const host = services({ getBinding: async () => ({
    footprint_id: '', footprint_name: '', footprint_status: 'unassigned', model_id: '', model_name: '',
  }) })
  assert.deepEqual(await host.symbols.binding('sym-1'), {
    symbolId: 'sym-1', footprintId: '', footprintName: '', footprintStatus: 'unassigned',
    modelId: '', modelName: '',
  })
})

test('matches maps the actual per-symbol match DTO', async () => {
  const host = services({ symbolMatches: async () => [{
    footprint_id: 'fp-1', footprint: 'Package:SOIC', score: 0,
    footprint_status: 'stale', footprint_reason: 'missing_file',
  }] })
  assert.deepEqual(await host.symbols.matches('sym-1'), [{
    footprintId: 'fp-1', name: 'Package:SOIC', score: 0,
    footprintStatus: 'stale', reason: 'missing_file',
  }])
})

test('symbol libraries map file_path and retain empty libraries', async () => {
  const host = services({ libraries: async () => [{
    id: 'lib-1', name: 'Empty', file_path: 'D:/组件/Empty.kicad_sym', symbol_count: 0, locked: false,
  }] })
  assert.deepEqual(await host.libraries.symbolLibraries(), [{
    id: 'lib-1', name: 'Empty', path: 'D:/组件/Empty.kicad_sym', symbolCount: 0, locked: false,
  }])
})

test('component libraries map paths and all false flags', async () => {
  const host = services({ compLibraries: async () => [{
    id: 'comp-1', name: 'Parts', symbol_path: 'D:/组件/Parts.kicad_sym', footprint_path: '',
    enabled: false, locked: false, is_protected: false,
  }] })
  assert.deepEqual(await host.libraries.componentLibraries(), [{
    id: 'comp-1', name: 'Parts', symbolPath: 'D:/组件/Parts.kicad_sym', footprintPath: '',
    enabled: false, locked: false, isProtected: false,
  }])
})

test('component catalog preserves an unregistered relative-path library with empty id', async () => {
  const host = services({ componentSymbolLibraries: async (id) => {
    assert.equal(id, 'comp-1')
    return { ok: true, can_create: false, case_sensitive: false, libraries: [
      { id: '', name: 'Empty', path: 'nested/Empty.kicad_sym' },
    ] }
  } })
  assert.deepEqual(await host.libraries.componentSymbolLibraries('comp-1'), {
    canCreate: false, caseSensitive: false,
    libraries: [{ id: '', name: 'Empty', path: 'nested/Empty.kicad_sym' }],
  })
})

test('footprint search maps candidate identities, pad counts and unavailable states', async () => {
  const host = services({ searchFootprints: async (...args) => {
    assert.deepEqual(args, ['SOIC', 'sym-1'])
    return [{ id: 'fp-1', name: 'SOIC', library: 'Package', score: 0, pad_count: 0,
      footprint_status: 'stale', footprint_reason: 'missing_file' }]
  } })
  assert.deepEqual(await host.footprints.search('SOIC', 'sym-1'), [{
    footprintId: 'fp-1', name: 'SOIC', library: 'Package', score: 0, padCount: 0,
    footprintStatus: 'stale', reason: 'missing_file',
  }])
})

test('empty query results stay empty arrays', async () => {
  const host = services({ symbols: async () => [], libraries: async () => [],
    compLibraries: async () => [], searchFootprints: async () => [],
    searchModels: async () => [], symbolMatches: async () => [],
    componentSymbolLibraries: async () => ({ ok: true, can_create: true, libraries: [] }),
  })
  assert.deepEqual(await host.symbols.list(), [])
  assert.deepEqual(await host.symbols.matches('sym-1'), [])
  assert.deepEqual(await host.libraries.symbolLibraries(), [])
  assert.deepEqual(await host.libraries.componentLibraries(), [])
  assert.deepEqual(await host.footprints.search(''), [])
  assert.deepEqual(await host.footprints.searchModels(''), [])
  assert.deepEqual(await host.libraries.componentSymbolLibraries('comp-1'), {
    libraries: [], canCreate: true,
  })
})

test('only explicit HTTP 404 single-symbol and binding reads become null', async () => {
  const absent = Object.assign(new Error('Symbol not found'), { kind: 'http', status: 404 })
  const host = services({ symbol: async () => { throw absent }, getBinding: async () => { throw absent } })
  assert.equal(await host.symbols.get('missing'), null)
  assert.equal(await host.symbols.binding('missing'), null)
})

test('transport and JSON parsing errors are propagated unchanged', async () => {
  for (const error of [
    Object.assign(new Error('Database unavailable'), { kind: 'http', status: 500 }),
    Object.assign(new Error('Invalid JSON response'), { kind: 'parse' }),
    Object.assign(new Error('Network error'), { kind: 'network' }),
  ]) {
    const reject = async () => { throw error }
    const host = services({ symbol: reject, getBinding: reject, componentSymbolLibraries: reject,
      libraries: reject, compLibraries: reject, symbols: reject, searchFootprints: reject,
      searchModels: reject, symbolMatches: reject,
    })
    for (const invoke of [() => host.symbols.get('sym-1'), () => host.symbols.binding('sym-1'),
      () => host.symbols.list(), () => host.symbols.matches('sym-1'),
      () => host.libraries.symbolLibraries(), () => host.libraries.componentLibraries(),
      () => host.libraries.componentSymbolLibraries('comp-1'),
      () => host.footprints.search('x'), () => host.footprints.searchModels('x'),
    ]) await assert.rejects(invoke, (actual) => actual === error)
  }
})

test('HTTP 200 business failures reject reads and writes with the server reason', async () => {
  const failure = async () => ({ ok: false, error: 'Library is locked', error_code: 'library_locked' })
  const host = services({ symbol: failure, getBinding: failure, componentSymbolLibraries: failure,
    libraries: failure, compLibraries: failure, symbols: failure, searchFootprints: failure,
    searchModels: failure, symbolMatches: failure, bindFootprint: failure, bindModel: failure,
  })
  for (const invoke of [() => host.symbols.get('sym-1'), () => host.symbols.binding('sym-1'),
    () => host.symbols.list(), () => host.symbols.matches('sym-1'),
    () => host.symbols.bindFootprint('sym-1', 'fp-1'), () => host.symbols.bindModel('fp-1', 'm-1'),
    () => host.libraries.symbolLibraries(), () => host.libraries.componentLibraries(),
    () => host.libraries.componentSymbolLibraries('comp-1'),
    () => host.footprints.search('x'), () => host.footprints.searchModels('x'),
  ]) await assert.rejects(invoke, (error) => error.message === 'Library is locked' && error.code === 'library_locked')
})

test('successful binding writes resolve void and pass their identities unchanged', async () => {
  const calls = []
  const host = services({
    bindFootprint: async (...args) => { calls.push(args); return { ok: true } },
    bindModel: async (...args) => { calls.push(args); return { ok: true } },
  })
  assert.equal(await host.symbols.bindFootprint('sym-1', 'fp-1'), undefined)
  assert.equal(await host.symbols.bindModel('fp-1', 'm-1'), undefined)
  assert.deepEqual(calls, [['sym-1', 'fp-1'], ['fp-1', 'm-1']])
})

test('malformed query envelopes cannot masquerade as an empty successful result', async () => {
  for (const response of [null, {}, { ok: true }, 'invalid']) {
    const invalid = async () => response
    const host = services({ symbols: invalid, libraries: invalid, compLibraries: invalid,
      searchFootprints: invalid, searchModels: invalid, symbolMatches: invalid,
      componentSymbolLibraries: invalid,
    })
    for (const invoke of [() => host.symbols.list(), () => host.symbols.matches('sym-1'),
      () => host.libraries.symbolLibraries(), () => host.libraries.componentLibraries(),
      () => host.footprints.search('x'), () => host.footprints.searchModels('x'),
      () => host.libraries.componentSymbolLibraries('comp-1'),
    ]) await assert.rejects(invoke, /Invalid .* response/)
  }
})

test('binding writes require an explicit successful response', async () => {
  for (const response of [null, {}, [], 'invalid', { ok: 'true' }]) {
    const host = services({ bindFootprint: async () => response, bindModel: async () => response })
    await assert.rejects(() => host.symbols.bindFootprint('sym-1', 'fp-1'), /Invalid .* response/)
    await assert.rejects(() => host.symbols.bindModel('fp-1', 'm-1'), /Invalid .* response/)
  }
})

test('malformed collection rows and single-read values reject instead of leaking invalid SDK data', async () => {
  for (const response of [null, [], 'invalid']) {
    const invalid = async () => response
    const host = services({ symbol: invalid, getBinding: invalid })
    await assert.rejects(() => host.symbols.get('sym-1'), /Invalid .* response/)
    await assert.rejects(() => host.symbols.binding('sym-1'), /Invalid .* response/)
  }
  for (const row of [null, [], 1]) {
    const invalid = async () => [row]
    const host = services({ symbols: invalid, libraries: invalid, compLibraries: invalid,
      searchFootprints: invalid, searchModels: invalid, symbolMatches: invalid,
    })
    for (const invoke of [() => host.symbols.list(), () => host.symbols.matches('sym-1'),
      () => host.libraries.symbolLibraries(), () => host.libraries.componentLibraries(),
      () => host.footprints.search('x'), () => host.footprints.searchModels('x'),
    ]) await assert.rejects(invoke, /Invalid .* response/)
  }
})

test('catalog capabilities must contain booleans instead of truthy text', async () => {
  for (const response of [
    { libraries: [], can_create: 'false' },
    { libraries: [], can_create: true, case_sensitive: 'false' },
  ]) {
    const host = services({ componentSymbolLibraries: async () => response })
    await assert.rejects(() => host.libraries.componentSymbolLibraries('comp-1'), /Invalid .* response/)
  }
})

test('real HTTP client encodes dynamic identities and keeps one-symbol reads bounded', async (t) => {
  const previousFetch = globalThis.fetch
  t.after(() => { globalThis.fetch = previousFetch })
  const calls = []
  globalThis.fetch = async (url) => {
    calls.push(url)
    return { ok: true, json: async () => url.includes('/symbols/')
      ? { id: 'sym-1', name: 'R', library_id: 'lib-1' } : [] }
  }
  const host = createHostServices()
  await host.symbols.list({ q: 'R & 0', libraryId: '库&other=1' })
  assert.deepEqual(await host.symbols.get('符号?#/'), {
    id: 'sym-1', name: 'R', libraryId: 'lib-1',
  })
  await api.getBinding('符号?#/')
  await api.symbolMatches('符号?#/')
  await api.bindFootprint('符号?#/', 'fp')
  await api.bindModel('封装?#/', 'model')
  const symbolId = encodeURIComponent('符号?#/')
  assert.deepEqual(calls, [
    `/api/symbols?q=${encodeURIComponent('R & 0')}&library=${encodeURIComponent('库&other=1')}`,
    `/api/symbols/${symbolId}`,
    `/api/symbols/${symbolId}/binding`,
    `/api/symbols/${symbolId}/matches`,
    `/api/symbols/${symbolId}/bind-footprint`,
    `/api/footprints/${encodeURIComponent('封装?#/')}/bind-model`,
  ])
})

test('real HTTP errors distinguish not found, backend failure and malformed JSON', async (t) => {
  const previousFetch = globalThis.fetch
  t.after(() => { globalThis.fetch = previousFetch })
  const host = createHostServices()
  globalThis.fetch = async () => ({ ok: false, status: 404, json: async () => ({ error: 'missing' }) })
  assert.equal(await host.symbols.get('missing'), null)
  assert.equal(await host.symbols.binding('missing'), null)
  globalThis.fetch = async () => ({ ok: false, status: 500, json: async () => ({ error: 'DB failed' }) })
  await assert.rejects(() => host.symbols.get('sym-1'),
    (error) => error.kind === 'http' && error.status === 500 && error.message === 'DB failed')
  globalThis.fetch = async () => ({ ok: true, json: async () => { throw new SyntaxError('bad JSON') } })
  await assert.rejects(() => host.symbols.get('sym-1'), (error) => error.kind === 'parse')
  globalThis.fetch = async () => { throw new Error('connection refused') }
  await assert.rejects(() => host.symbols.get('sym-1'), (error) => error.kind === 'network')
})

test('refreshAll runs each required refresh exactly once and resolves void', async () => {
  const calls = []
  const host = createHostServices({ getRefresh: () => ({
    symbols: async () => { calls.push('symbols') },
    libraries: async () => { calls.push('libraries') },
    status: async () => { calls.push('status') },
  }) })
  assert.equal(await host.commands.run('refreshAll'), undefined)
  assert.deepEqual(calls, ['symbols', 'libraries', 'status'])
})

test('commands reject unknown names and missing callbacks before partial refresh', async () => {
  let calls = 0
  const host = createHostServices({ getRefresh: () => ({ symbols: async () => { calls += 1 } }) })
  await assert.rejects(() => host.commands.run('unknown'), /Unknown command/)
  await assert.rejects(() => host.commands.run('refreshAll'), /refresh.*not available/i)
  assert.equal(calls, 0)
})

test('commands preserve a refresh rejection and do not report a completed refresh', async () => {
  const failure = new Error('symbols read failed')
  let laterCalls = 0
  const host = createHostServices({ getRefresh: () => ({
    symbols: async () => { throw failure },
    libraries: async () => { laterCalls += 1 },
    status: async () => { laterCalls += 1 },
  }) })
  await assert.rejects(() => host.commands.run('refreshAll'), (error) => error === failure)
  assert.equal(laterCalls, 0)
})
