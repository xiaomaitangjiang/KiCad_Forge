<div align="center">

<img src="../resources/icons/app.ico" width="112" height="112" alt="KiCad Forge icon">

# KIForge

A library manager for KiCad (symbols & footprints)

 [![Version](https://img.shields.io/github/v/release/xiaomaitangjiang/KiCad_Forge?style=flat-square&label=release&color=45658D)](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) [![License](https://img.shields.io/github/license/xiaomaitangjiang/KiCad_Forge?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/blob/master/LICENSE) [![Platform](https://img.shields.io/badge/platform-Windows-665A8A?style=flat-square)](#download) [![Bugs](https://img.shields.io/github/issues/xiaomaitangjiang/KiCad_Forge/bug?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/issues?q=is%3Aopen+is%3Aissue+label%3Abug)

KiCad Forge is a modern graphical library manager, built to make managing multi-library files and large component collections in KiCad faster and more intuitive.

English ~ [简体中文](../README.md)

[Cross-platform builds, automated tests, and manual releases](development/ci.md)

![screenshot](assets/screenshots/main.png)

[Features](#features) · [Quick Start](#quick-start) · [Download](#download) · [Contributing](../CONTRIBUTING.md) · [Important Notes](user/usage-notes.en.md)
</div>

---

## Features

- **Multiple component libraries** — Each component library groups symbol and footprint directories, with an optional 3D model directory; browse libraries in groups in the sidebar
- **One-click batch import** — Scan a KiCad library directory and parse `.kicad_sym` / `.kicad_mod` files automatically (22,000+ symbols imported in seconds in real tests)
- **Search & filter** — Fuzzy search by name, filter by library, locate parts quickly
- **Component type matching** — Exact exceptions take precedence over PCRE2 regex rules; unmatched parts remain `Unknown`. Choose the name, value, model/MPN, or reference field, and edit type icons and colors
- **Link checking & auto-matching** — Check Symbol↔Footprint↔3D model link integrity, heuristic auto-matching for footprints
- **LCSC import** — Look up an LCSC part number, choose the destination component library and an existing or new symbol sublibrary, then reuse one of the automatically found footprint candidates or create a new footprint
- **Plugin system** — Python plugins run as independent processes, UI driven by `manifest.json`; ships with an [LCSC component importer](../plugins/lcsc_import/) and a [Git version manager](../plugins/git_version/) plugin
- **Bilingual UI** — 中文 / English (i18next)
- **Portable storage** — Local SQLite database, data directory travels with the app

Preview matching rules against a sample or the database before applying them. Saving changes only updates the configuration. Applying saved rules fills unclassified parts and preserves assigned types. Directory imports and LCSC imports use the same saved rules.

## Quick Start

### Prerequisites

- [xmake](https://xmake.io/) 3.0.9+
- [MSYS2](https://www.msys64.org/): `mingw-w64-x86_64-clang`, `mingw-w64-x86_64-lld`, and `mingw-w64-x86_64-gcc`
- Python 3.12
- Node.js 24 (React 19 + Vite frontend)

Before the first build, prepare PCRE2 and the other locked dependencies as described in [docs/development/ci.md](development/ci.md#本地构建). Normal builds use the tracked `resources/icons/app.ico`. Only changes to the icon artwork require CairoSVG and Pillow, followed by a manual run of `scripts/svg2ico.py`.

> Defaults to `D:/msys64/mingw64`; set the `MSYS2_DIR` environment variable to point to a different MSYS2 installation.

### Build & Run

```bash
# 1. Install frontend dependencies
cd webui && npm ci && cd ..

# 2. Configure and build (debug)
xmake f -p mingw -c
xmake build

# Or build a release version
xmake f -m release && xmake build
```

The binary lands at `build/mingw/x86_64/<mode>/KiCad_Forge.exe`. After building, `after_build` automatically copies webui, plugins, config, and the runtime DLLs into the output directory — just double-click the exe to run (embedded WebView2 window).

### Data Directory

Portable-first: data (SQLite database, settings) lives in a `data/` folder next to the exe; if that is not writable, it falls back to `%APPDATA%/KiCad_Forge/data/`.

## Project Structure

```
KiCad_Forge
├── config
├── docs
├── plugins
├── resources
├── scripts
├── sql
├── src
│   ├── classifier
│   ├── core
│   │   ├── db
│   │   ├── io
│   │   │   ├── parser
│   │   │   └── sexpr
│   │   │       ├── dom
│   │   │       └── token
│   │   ├── model
│   │   └── repo
│   ├── correspondence
│   ├── interface
│   │   ├── api
│   │   ├── manager
│   │   │   ├── binding
│   │   │   └── setup
│   │   └── service
│   │       ├── classification
│   │       ├── correspondence
│   │       ├── import
│   │       └── library
│   ├── platform
│   ├── plugin
│   ├── third_party
│   └── util
│       ├── container
│       └── template
├── tests
└── webui
```

## Plugin System

> **⚠️ Note: Plugins run as independent Python processes with the same filesystem access as the host application — KiCad Forge cannot provide any security guarantee. Only install plugins from trusted sources; a malicious plugin can read, modify, or delete arbitrary files.**

Plugins live in `plugins/`, one folder per plugin, containing `manifest.json` + Python scripts. The backend invokes them via CLI: `python plugin.py <action> '<json_args>'`; the plugin outputs JSON to stdout.

**manifest.json**

```json
{
  "id": "com.kicad_forge.lcsc_import",
  "name": "LCSC Component Importer",
  "version": "2.1.0",
  "author": "KiCad Forge",
  "description": "Download symbols, footprints, and 3D models from LCSC by part number",
  "license": "MIT",
  "capabilities": [
    "import"
  ],
  "one_click": true,
  "entry": "plugin.py",
  "entry_function": "import_component",
  "icon": "icon.svg",
  "actions": [
    {
      "id": "import",
      "name": "Import LCSC",
      "description": "Find a part by LCSC number and import it into a component library package",
      "trigger": "dialog",
      "template": "lcsc_import",
      "analyze_action": "analyze",
      "protocol": "staged-import-v1",
      "icon": "icon.svg",
      "button": {
        "show": true,
        "style": "both",
        "location": "sidebar",
        "tooltip": "Download from LCSC"
      },
      "schema": {
        "fields": [
          {
            "key": "lcsc_id",
            "label": "LCSC Part Number",
            "type": "text",
            "required": true
          },
          {
            "key": "target_component_library_id",
            "label": "Component Library Package",
            "type": "component_library_picker",
            "required": true
          },
          {
            "key": "symbol_library_choice",
            "label": "Symbol Library",
            "type": "symbol_library_choice",
            "source": "component_symbol_libraries",
            "modes": [
              "existing",
              "new"
            ],
            "required": true
          },
          {
            "key": "footprint_choice",
            "label": "Footprint",
            "type": "footprint_choice",
            "source": "footprints",
            "near_from": "footprint_name",
            "modes": [
              "reuse",
              "new"
            ],
            "required": true
          }
        ]
      }
    },
    {
      "id": "analyze",
      "name": "Analyze LCSC",
      "description": "Download and inspect without writing to the library",
      "trigger": "hidden",
      "hidden": true,
      "button": {
        "show": false
      }
    }
  ],
  "dependencies": {
    "python": ">=3.10",
    "pip": [
      "easyeda2kicad"
    ]
  }
}
```

**Top-level fields**

| Field | Description |
|---|---|
| `id` / `name` | Unique plugin ID and display name (required) |
| `version` / `author` / `description` / `license` | Plugin metadata |
| `entry` | Entry script, defaults to `plugin.py` |
| `entry_function` | Entry function name (reserved) |
| `icon` | Plugin icon (SVG) |
| `capabilities` | Capability declarations (e.g. `import`, `tool`) |
| `one_click` | Whether the plugin supports one-click execution |
| `actions` | Action list, see table below |
| `dependencies` | Runtime dependencies: `python` version constraint and `pip` packages |
| `min_app_version` / `platforms` | Optional: minimum app version / supported platforms |

**actions[] fields**

| Field | Description |
|---|---|
| `trigger` | `"dialog"` popup form / `"inline"` one-click execution (default) / `"hidden"` hidden action (referenced by templates) |
| `template` | `"lcsc_import"` opens the dedicated LCSC flow: look up the part → choose the destination component library → choose an existing or new symbol sublibrary → reuse or create a footprint; other plugins use the generic form |
| `analyze_action` | ID of the hidden analysis action referenced by `lcsc_import` |
| `protocol` | `"staged-import-v1"` staged import protocol |
| `preflight` | action id executed before the dialog opens; its result (e.g. Git status) is shown at the top of the form |
| `auto_args` | Argument keys filled in automatically by the host (e.g. `target_library`) |
| `button.show` | Show the button (default `true`) |
| `button.style` | `"icon"` / `"text"` / `"both"` (default) |
| `button.location` | `"toolbar"` (default) / `"sidebar"` |
| `button.tooltip` | Button tooltip |

**schema.fields[] supported field types**

| `type` | Description |
|---|---|
| `text` | Text input (default) |
| `library_picker` | Symbol library dropdown in the generic form |
| `component_library_picker` | `lcsc_import` only: choose a registered, writable component library with symbol and footprint directories |
| `symbol_library_choice` | `lcsc_import` only: choose a physical `.kicad_sym` file within the destination package, including empty libraries, or enter a new sublibrary name; no default selection |
| `footprint_choice` | `lcsc_import` only: reuse a footprint by UUID from automatic candidates or search results, or explicitly create a new one |

**style: icon**
![icon](assets/screenshots/plugin-style/with-icon.png)

**style: both**
![both](assets/screenshots/plugin-style/icon-and-text.png)

**style: text**
![text](assets/screenshots/plugin-style/without-text.png)

## API

The built-in HTTP server exposes a REST API. Full documentation: [docs/api/http-api.md](api/http-api.md) (Chinese).

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | GET | Health check / heartbeat |
| `/api/symbols` | GET | List symbols (`?q=`, `?library=`) |
| `/api/libraries` | GET/POST | Symbol library CRUD |
| `/api/component-libraries` | GET/POST | Manage and import component library directory groups |
| `/api/plugins` | GET | List plugins |
| `/api/plugins/{id}/icon` | GET | Plugin icon |
| `/api/classify` | POST | Run classification rules |
| `/api/check` | POST | Check Symbol↔Footprint↔3D links |
| `/api/config` | GET/POST | Read/write configuration |
| `/api/component-type-matching` | GET/POST | Read or save type matching rules; saving does not classify parts |
| `/api/component-type-matching/preview` | POST | Preview a sample or the database without database writes |
| `/api/component-type-matching/apply` | POST | Apply a specified revision of saved rules to unclassified parts |
| `/api/component-type-matching/reload` | POST | Reload matching rules from the data directory |
| `/api/import` | POST | Import a directory (`?dir=`) |

## Download

Grab the latest release from [Releases](https://github.com/xiaomaitangjiang/KiCad_Forge/releases):

- **Windows**: an installer is provided — download and run
- Other platforms: no prebuilt packages yet, please build from source (see [Quick Start](#quick-start))

## Tech Stack

| Layer | Choice |
|---|---|
| Language | C++23 |
| Compiler | clang 22 (MSYS2, MinGW target) |
| Build | xmake |
| Frontend | React 19 + Vite |
| Database | SQLite3 |
| HTTP | httplib (header-only) |
| Logging | spdlog |
| WebView | [webview/webview](https://github.com/webview/webview) (WebView2 Runtime on Windows) |
| License | MIT |

---

## Contributing

Issues and PRs are welcome — workflow and code conventions in [CONTRIBUTING.md](../CONTRIBUTING.md). When filing a bug, please include a detailed description and the application's log file. "Wishful" issues are discouraged; see [Important Notes](user/usage-notes.en.md).
