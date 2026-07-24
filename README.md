# KiCad Forge

Desktop library manager for KiCad — browse, classify, and import symbols and footprints with a modern web UI and plugin system.

## Quick Start

### Prerequisites
- [MSYS2](https://www.msys64.org/) with `mingw-w64-x86_64-clang`
- Python 3.10+ (for LCSC plugin)
- Node.js (for frontend)

### Build (Windows)

```powershell
# One-time: install dependencies
cd webui && npm install && cd ..

# Every build
.\make.ps1
```

Output: `build/mingw/x86_64/release/KiCad_Forge.exe`

Double-click to run. The app opens in an Edge `--app` window (Win11 built-in, no extra install).

### Build (macOS / Linux)

```bash
xmake f --toolchain=clang -m release -c
xmake build
```

Opens in system default browser. Close the browser tab to stop the server.

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
├── platform/         Cross-platform app window (CRTP)
└── net/              Built-in HTTP server (legacy, replaced by httplib)

plugins/lcsc_import/  LCSC one-click import plugin (self-contained)
webui/                React 19 + Vite frontend
```

## Database

SQLite in `./data/meta.db` (portable mode, next to exe).  
Installer mode (future): `%APPDATA%/KiCad_Forge/meta.db`.

| Table | Purpose |
|---|---|
| `libraries` | Symbol/footprint library metadata |
| `symbols` | Symbol instances with pin serialization |
| `footprints` | Footprint instances |
| `models_3d` | 3D model references |
| `symbol_footprint_links` | Symbol↔Footprint relationships |
| `footprint_model_links` | Footprint↔3D model relationships |
| `settings` | Key-value configuration |

## Plugin System

Plugins live in `plugins/` as folders with `manifest.json` + Python scripts.
The main app invokes them via CLI: `python plugin.py <action> '<json_args>'`.
Plugin output is JSON on stdout.

Example manifest:
```json
{
  "id": "com.example.my_plugin",
  "name": "My Plugin",
  "capabilities": ["import"],
  "entry": "plugin.py",
  "one_click": true
}
```

The LCSC import plugin bundles [easyeda2kicad](https://github.com/uPesy/easyeda2kicad.py) (MIT) — no `pip install` needed.

## Tech Stack

| Layer | Choice |
|---|---|
| Language | C++23 |
| Compiler | clang 22 (MSYS2) |
| Build | xmake |
| Frontend | React 19 + Vite |
| Database | SQLite3 |
| HTTP | httplib (header-only, MIT) |
| JSON | nlohmann/json |
| License | MIT |
