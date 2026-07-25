# Contributing to KiCad Forge

We welcome contributions! Here's how to get started.

## Development workflow

1. Fork & clone the repo
2. Create a feature branch: `git checkout -b feature/your-feature`
3. Install dependencies (see README Quick Start)
4. Make your changes, following the conventions below
5. Test both `debug` and `release` builds
6. Submit a PR with a clear description of what changed and why

## Code conventions

### C++
- C++23, clang (MinGW target via MSYS2)
- `printf` for output — **not** `std::println` (MinGW libstdc++ does not implement `__write_to_terminal`)
- `snake_case` filenames, PascalCase types, snake_case methods
- `util::Result<T>` for fallible operations — no exceptions in core logic
- `#pragma once` for headers (not include guards)
- Prefer `std::filesystem::path` over string concatenation for path manipulation

### Frontend (React)
- Functional components with hooks (`useState`, `useEffect`)
- Inline SVG for icons, CSS variables for theming
- `fetch()` with relative URLs (`/api/...`) — never hardcode ports or hosts
- CSS class naming: kebab-case, scoped by component

### Build tooling
- `xmake` for C++, `npm`/Vite for frontend
- `python scripts/svg2ico.py` generates `resources/app.ico` from `webui/public/favicon.svg`
- `after_build` in `xmake.lua` copies webui + plugins + required DLLs to the output directory
- `before_build` regenerates the ICO when the source SVG is newer

## Before submitting a PR

1. Both `debug` and `release` modes build cleanly (`xmake build`)
2. `npm run build` in `webui/` passes without warnings
3. No new compiler warnings (existing warnings in `api_server.cpp` are known)
4. Manual smoke test:
   - Launch exe — Edge window opens, toolbar loads
   - Plugin button appears, dialog opens and works
   - Close Edge — server exits within 1 second (heartbeat)

## Project architecture conventions

| Convention | Detail |
|---|---|
| **Portable-first** | Runtime data in `./data/` next to exe; falls back to `%APPDATA%/KiCad_Forge/data/` |
| **No Qt** | Pure C++23 + httplib + SQLite3, web UI via React, Edge `--app` for native window |
| **CRTP platform** | `AppWindow<Derived>` — compile-time dispatch, zero virtual overhead |
| **Heartbeat lifecycle** | Frontend pings `/api/status` every 500ms. Server exits 1s after last ping. No process monitoring. |
| **Plugin isolation** | Plugins run as separate Python processes via `CreateProcess(CREATE_NO_WINDOW)` |
| **Plugin discovery** | `exe_dir/plugins/` scanned at startup. `manifest.json` drives all UI (actions, buttons, dialogs) |

## Directory structure

```
src/                    C++ backend
  api/                  HTTP routes (httplib)
  core/                 Domain models
  sexpr/                S-Expression parser
  parser/               KiCad file parsers
  storage/              SQLite database layer
  classifier/           Classification rules
  correspondence/       Symbol↔Footprint↔3D tracker
  services/             Business logic
  plugin/               Plugin infrastructure
  platform/             Cross-platform window (CRTP)
plugins/                Plugin bundles
webui/                  React frontend
resources/              Windows .rc + auto-generated .ico
scripts/                Build helper scripts
```

## Adding a plugin

1. Create `plugins/<name>/manifest.json` with:
   - `id` — unique reverse-domain identifier
   - `name` — display name
   - `icon` — SVG icon filename
   - `actions[]` — each with `id`, `name`, `trigger`, `schema.fields[]`
2. Add `icon.svg` (24x24 SVG) to the plugin directory
3. Write `plugin.py` — receives `action` + JSON args from CLI, outputs JSON to stdout
4. Define `actions[].schema.fields[]` for any dialog-triggered actions. Supported field types:
   - `text` — text input
   - `library_picker` — library dropdown
   - `select` — static options
   - `file_picker` — file path input
5. Rebuild — `after_build` copies `plugins/` automatically

See [plugins/lcsc_import/](plugins/lcsc_import/) for the reference implementation.

## What NOT to commit

- `build/` — build outputs (in `.gitignore`)
- `*.db`, `*.db-wal`, `*.db-shm` — runtime databases
- `resources/app.ico` — auto-generated from `favicon.svg` by `before_build`
- IDE files (`.idea/`, `*.user`)
- DLLs and exe files (`*.dll`, `*.exe`) — build artifacts

The `.vscode/` directory IS tracked — it contains shared debug and build configurations.
