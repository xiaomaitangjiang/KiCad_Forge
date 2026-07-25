# KiCad Forge

Desktop library manager for KiCad — browse, classify, and import symbols and footprints with a modern web UI and extensible plugin system.

## Quick Start

### Prerequisites
- [MSYS2](https://www.msys64.org/) with `mingw-w64-x86_64-clang` and `mingw-w64-x86_64-cairo`
- Python 3.10+ with `cairosvg` and `Pillow` (`pip install cairosvg Pillow`)
- Node.js (for frontend)

### Build

```powershell
# One-time: install frontend dependencies
cd webui && npm install && cd ..

# Build
.\make.ps1
```

Output: `build/mingw/x86_64/release/KiCad_Forge.exe`.

Double-click to run. The app opens in an Edge `--app` window (no extra install required). The exe is self-contained — required DLLs are copied alongside it automatically.

Debug build: `xmake f -m debug && xmake build`. Output: `build/mingw/x86_64/debug/KiCad_Forge.exe`.

### Debugging (VSCode)

Press `F5` with the CodeLLDB extension installed. LLVM's lldb at `C:\Program Files\LLVM\bin\lldb.exe` is used for debugging. Set breakpoints by clicking in the gutter.

Configuration files in `.vscode/`:
- `launch.json` — LLDB launch config
- `settings.json` — lldb path override
- `tasks.json` — xmake build task (`Ctrl+Shift+B`)

## Architecture

```
src/
├── api/              HTTP routes (httplib)
├── core/             Domain models: Symbol, Footprint, Model3D, ComponentType
├── sexpr/            S-Expression parser (tokenizer → DOM → writer)
├── parser/           KiCad file parsers (.kicad_sym / .kicad_mod)
├── storage/          SQLite database (Repository pattern)
├── classifier/       Classification rule engine
├── correspondence/   Symbol↔Footprint↔3D relationship tracker
├── services/         Business logic orchestration
├── plugin/           Python-based plugin system
└── platform/         Cross-platform app window (CRTP, Edge --app on Windows)

plugins/lcsc_import/  LCSC one-click import plugin (self-contained)
webui/                React 19 + Vite frontend
resources/            Windows resource file + auto-generated app icon
scripts/              svg2ico.py — SVG → ICO converter (called by build)
```

## Plugin System

Plugins live in `plugins/` as folders with `manifest.json` + Python scripts. The backend invokes them via CLI: `python plugin.py <action> '<json_args>'`. Plugin output is JSON on stdout.

### Manifest format

```json
{
  "id": "com.kicad_forge.lcsc_import",
  "name": "LCSC Component Importer",
  "version": "1.0.0",
  "author": "KiCad Forge",
  "description": "Download symbols, footprints, and 3D models from LCSC",
  "license": "MIT",
  "entry": "plugin.py",
  "icon": "icon.svg",
  "capabilities": ["import"],
  "one_click": true,
  "actions": [
    {
      "id": "import",
      "name": "Import LCSC",
      "description": "Import a component from LCSC by part number",
      "trigger": "dialog",
      "button": { "show": true, "style": "both", "tooltip": "Download from LCSC" },
      "schema": {
        "fields": [
          { "key": "lcsc_id", "label": "LCSC Part Number", "type": "text", "required": true },
          { "key": "target_library", "label": "Target Symbol Library", "type": "library_picker", "required": true }
        ]
      }
    }
  ]
}
```

| Field | Description |
|---|---|
| `actions[].trigger` | `"dialog"` — popup form, `"inline"` — one-click execute, `"panel"` — side panel |
| `actions[].button.show` | `false` to hide toolbar button |
| `actions[].button.style` | `"icon"` / `"text"` / `"both"` |
| `actions[].schema.fields` | Form field definitions for `trigger: "dialog"`. Types: `text`, `library_picker`, `select`, `file_picker` |

Plugin icons (`icon.svg`, 24×24 SVG) are served via `/api/plugins/{id}/icon` and displayed in the toolbar and plugin panel.

## API

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | GET | Server health check |
| `/api/symbols` | GET | List symbols (supports `?q=`, `?library=`) |
| `/api/libraries` | GET/POST | List/create/delete libraries |
| `/api/plugins` | GET | List plugins with actions, icon URLs, button config |
| `/api/plugins/{id}/icon` | GET | Serve plugin icon file |
| `/api/plugins/execute` | POST | Execute a plugin action |
| `/api/classify` | POST | Run classification rules |
| `/api/check` | POST | Check symbol↔footprint↔3D correspondence |
| `/api/settings` | GET/POST | Read/write settings |
| `/api/import` | POST | Import directory (supports `?dir=`) |

## Contributing

### Code style
- C++23, clang (MinGW target)
- `printf` for output (not `std::println` — MinGW libstdc++ doesn't support it)
- Use `util::Result<T>` for fallible operations (no exceptions in core logic)
- `src/` code: `snake_case` files, PascalCase types, snake_case methods
- Frontend: React functional components, `useState`/`useEffect` hooks

### Before submitting
1. Build passes both `debug` and `release` modes
2. `npm run build` in `webui/` produces clean output
3. Manual smoke test: launch exe, verify toolbar loads, LCSC import dialog opens
4. No new compiler warnings

### Project conventions
- **Portable-first**: all runtime data goes in `./data/` next to the exe when available
- **No Qt**: pure C++23 with httplib + SQLite3, web UI via React
- **Plugin isolation**: plugins run as separate Python processes via `CreateProcess(CREATE_NO_WINDOW)`
- **CRTP for platform**: `AppWindow<Derived>` pattern — zero virtual dispatch, compile-time dispatch

### Adding a plugin
1. Create `plugins/<name>/manifest.json` with `id`, `name`, `entry`, `actions[]`
2. Add `icon.svg` (24×24) for the toolbar button
3. Write the Python entry script (receives action + JSON from CLI)
4. Rebuild — `after_build` copies plugins automatically

## Tech Stack

| Layer | Choice |
|---|---|
| Language | C++23 |
| Compiler | clang 22 (MSYS2, MinGW target) |
| Build | xmake |
| Frontend | React 19 + Vite |
| Database | SQLite3 |
| HTTP | httplib (header-only, MIT) |
| JSON | nlohmann/json |
| License | MIT |
