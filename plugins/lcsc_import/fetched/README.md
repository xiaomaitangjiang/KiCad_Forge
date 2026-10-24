# Legacy fetched samples

These directories are historical `easyeda2kicad` download results kept as
representative fixtures (C2765181, C2862740, C347222, C37593, C5120765).

They are **not** the current import cache. The active workflow stages work in
`staging/` and writes directly to the configured component library; packaging
excludes both `fetched/` and `staging/` (see `scripts/package.py`,
`scripts/sync_runtime_assets.py`, and `packaging/windows/kiforge.iss`).

Do not add runtime output here. New automated fixtures belong under
`tests/fixtures/`.
