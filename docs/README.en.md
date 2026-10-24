<div align="center">

<img src="../resources/icons/app.ico" width="112" height="112" alt="KiCad Forge icon">

# KIForge

A library manager for KiCad (symbols & footprints)

 [![Version](https://img.shields.io/github/v/release/xiaomaitangjiang/KiCad_Forge?style=flat-square&label=release&color=45658D)](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) [![License](https://img.shields.io/github/license/xiaomaitangjiang/KiCad_Forge?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/blob/master/LICENSE) [![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-665A8A?style=flat-square)](#download) [![Bugs](https://img.shields.io/github/issues/xiaomaitangjiang/KiCad_Forge/bug?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/issues?q=is%3Aopen+is%3Aissue+label%3Abug)

KiCad Forge is a modern graphical library manager, built to make managing multi-library files and large component collections in KiCad faster and more intuitive.

English ~ [简体中文](../README.md)

[Build from Source](#build-from-source) · [Packaging and CI](#packaging-and-ci)

![screenshot](assets/screenshots/main.png)

[Features](#features) · [Quick Start](#quick-start) · [Download](#download) · [Contributing](../CONTRIBUTING.md) · [Important Notes](user/usage-notes.en.md)
</div>

---

## Features

- **Multiple component libraries** — Each component library groups symbol and footprint directories, with an optional 3D model directory; browse libraries in groups in the sidebar
- **Batch import** — Scan a KiCad library directory and parse `.kicad_sym` / `.kicad_mod` files automatically
- **Search & filter** — Fuzzy search by name, filter by library, locate parts quickly
- **Component type matching** — Exact exceptions take precedence over PCRE2 regex rules; unmatched parts remain `Unknown`. Choose the name, value, model/MPN, or reference field, and edit type icons and colors
- **Link checking & auto-matching** — Check Symbol↔Footprint↔3D model link integrity, heuristic auto-matching for footprints
- **LCSC import** — Look up an LCSC part number, choose the destination component library and an existing or new symbol sublibrary, then reuse one of the automatically found footprint candidates or create a new footprint
- **Git version control** — View library changes, commit graphs, branch labels, and file diffs in the left sidebar; initialize repositories and commit changes
- **Symbol editing** — Copy, paste, cut/move, select all, and undo/redo within the current session; whole imports cannot be undone
- **Workbench** — Page groups, saved layouts, and a closable All Components tab; complete LCSC and Git interfaces stay in the left sidebar, with a drawer on narrow windows
- **Plugin system** — Independent Python processes and TypeScript frontend plugins; includes an [LCSC component importer](../plugins/lcsc_import/) and [Git version control](../plugins/git_version/), with frontend extensions through the public SDK
- **Cross-platform packages** — Windows x64, macOS Apple Silicon / Intel, and Linux x64; distribution packages include Python
- **Bilingual UI** — The host supports Chinese and English; translation coverage depends on each plugin

Preview matching rules against a sample or the database before applying them. Saving changes only updates the configuration. Applying saved rules fills unclassified parts and preserves assigned types. Directory imports and LCSC imports use the same saved rules.

## Quick Start

### Download

Choose a version and CPU architecture from [Releases](https://github.com/xiaomaitangjiang/KiCad_Forge/releases). The current source and CI configure the formats below; available downloads depend on the **Assets** attached to each release. Build from source if a package has not been uploaded.

| Platform | Recommended format | Other downloads |
| --- | --- | --- |
| Windows x64 | EXE installer | ZIP, no installer required |
| macOS Apple Silicon (arm64) | DMG | ZIP containing the `.app` bundle |
| macOS Intel (x86_64) | DMG | ZIP containing the `.app` bundle |
| Linux x64 (Ubuntu/Debian) | DEB installer | tar.gz / ZIP, no installer required |

Distribution packages on all three operating systems include Python 3.12.15 and a certificate bundle. The built-in plugins require no separate Python installation or manual pip setup. The Git plugin still requires Git on the system `PATH`; LCSC lookup and downloads need an internet connection.

### How to Use

- **Windows**: Run the EXE installer, or extract the ZIP and launch `KiCad_Forge.exe`. The installer uses a directory under the current user's profile and adds Start menu shortcuts; a desktop shortcut is optional. The embedded UI requires an available WebView2 Runtime.
- **macOS**: Open the DMG and copy `KiCad Forge.app` into Applications before opening it. Extracting the ZIP produces the same `.app` bundle. Neither format needs an installation wizard. Developer ID signing and notarization are not configured yet, so macOS may restrict the first launch.
- **Linux**: Open the DEB in the system software installer, or use `sudo apt install ./filename.deb`. For the versions without an installer, extract the files and run `KiCad_Forge`; system library requirements still apply.

macOS and Linux display the UI in the system browser and need a graphical desktop session. Linux also needs `xdg-open`, usually provided by `xdg-utils`.

On first launch, the app tries to detect and import the official libraries from a local KiCad installation. If none are found, or to add third-party libraries, configure symbol, footprint, and optional 3D model paths under Settings → Library Paths.

Symbol paths can be directories or individual `.kicad_sym` files. Footprint and 3D model paths must be directories. One possible layout is shown below; the folder names are not required:

```text
EXT_Internal_kicad_Lib
├── 3DModels
├── Docs
├── Footprints
│   ├── EXT_ACDC.pretty
│   └── EXT_Example.pretty
└── Symbols
```

![Settings](assets/screenshots/setting.png)

### Data Directory

| Platform | Default data directory |
| --- | --- |
| Windows | `%APPDATA%/KiCad_Forge/data/` |
| macOS `.app` | `~/Library/Application Support/KiCad_Forge/data/` |
| Linux | `~/.KiCad_Forge/` |

On Windows and Linux, an existing `data/` directory next to the executable takes precedence. To keep data with the app, create that directory before the first launch and ensure it is writable. Downloading a version without an installer does not enable portable data storage automatically. macOS `.app` bundles always use the user data directory.

Back up the database, configuration, and library files before upgrading. Windows now uses a per-user NSIS installer. An older system-wide Inno Setup installation has a separate uninstall entry; uninstall that version separately if you want to remove it.

## Build from Source

These tools are for development and packaging. Users of published distribution packages do not need them.

### Prerequisites

- [xmake](https://xmake.io/) 3.0.9+ (CI uses 3.1.1)
- Python 3.12, Node.js 24, and CMake
- Windows: [MSYS2](https://www.msys64.org/) packages `mingw-w64-x86_64-clang`, `mingw-w64-x86_64-lld`, and `mingw-w64-x86_64-gcc`; add the toolchain's `bin` directory to `PATH`
- Linux: GCC 14 / G++ 14
- macOS: Xcode Command Line Tools / Apple Clang

PCRE2 and the other native dependencies use locked archives, recipes, and checksums under `dependencies/`. Prepare dependencies and configure the project before building. The Windows example assumes MSYS2 is at `D:/msys64/mingw64`; adjust it for your machine.

Normal builds use the tracked `resources/icons/app.ico`. Only icon artwork changes require CairoSVG and Pillow, followed by a manual run of `scripts/svg2ico.py`.

### Prepare the Frontend

Run from the repository root:

```sh
npm ci --prefix webui
npm run build --prefix webui
node scripts/build_plugins.mjs
```

### Build the Native Application

Windows (PowerShell):

```powershell
$env:MSYS2_DIR = "D:/msys64/mingw64"
python -B scripts/prepare_dependencies.py --plat mingw --arch x86_64 --mode release
$env:PCRE2_SDK_DIR = (Resolve-Path "build/dependencies/pcre2/mingw/x86_64/release/install").Path
xmake f -y -c -p mingw -a x86_64 -m release --portable_deps=y --mingw="$env:MSYS2_DIR"
xmake build -y -j 2 KiCad_Forge
```

Linux x64:

```sh
python3 -B scripts/prepare_dependencies.py --plat linux --arch x86_64 --mode release --cc gcc-14
xmake f -y -c -p linux -a x86_64 -m release --portable_deps=y --cc=gcc-14 --cxx=g++-14 --ld=g++-14 --sh=g++-14
xmake build -y -j 2 KiCad_Forge
```

macOS Apple Silicon (on Intel Macs, replace both occurrences of `arm64` with `x86_64`):

```sh
python3 -B scripts/prepare_dependencies.py --plat macosx --arch arm64 --mode release --cc clang
xmake f -y -c -p macosx -a arm64 -m release --portable_deps=y
xmake build -y -j 2 KiCad_Forge
```

The application is written to `build/<platform>/<architecture>/release/`: `KiCad_Forge.exe` on Windows and `KiCad_Forge` elsewhere. The build copies the frontend, plugins, configuration, licenses, and Windows runtime DLLs into that directory.

When run directly from a source build, plugins still use `python` on the Windows `PATH`, or `python3` on macOS/Linux. The XPack step below adds the private Python runtime to distribution packages.

## Packaging and CI

Complete the Release build above on the target operating system, then prepare the locked Python runtime, certificates, and packaging tools separately. Downloads are allowed only in this explicit preparation step with `--download`; every file is checked against `dependencies/packaging.lock.json`. Omit the flag when the cache is complete.

Windows (PowerShell):

```powershell
python -B scripts/prepare_packaging.py --platform mingw --arch x86_64 --download
xmake pack --autobuild=n -j 1 -o dist/installers
```

Linux x64 (requires `dpkg-deb`, `dpkg-shlibdeps`, and `objdump`, usually provided by `dpkg`, `dpkg-dev`, and `binutils`):

```sh
python3 -B scripts/prepare_packaging.py --platform linux --arch x86_64 --download
xmake pack --autobuild=n -j 1 -o dist/installers
```

macOS Apple Silicon (replace `arm64` with `x86_64` on Intel Macs):

```sh
python3 -B scripts/prepare_packaging.py --platform macosx --arch arm64 --download
xmake pack --autobuild=n -j 1 -o dist/installers
```

The [XPack configuration](../packaging/xpack.lua) generates every format listed for the target platform: NSIS on Windows, system DMG tools on macOS, and native dpkg tools for Linux DEB files. Final downloads go to `dist/installers/`; `build/` contains compiled output and dependencies. Both directories are excluded from Git.

The [CI workflow](../.github/workflows/ci.yml) checks the frontend and SDK, then runs Debug and Release builds and tests across four platform/architecture combinations, for eight native jobs. Release jobs generate and upload distribution packages automatically. Ordinary pushes and pull requests do not publish releases. Only a manual run with `publish_release` enabled publishes after all checks pass, using a new stable tag in `vMAJOR.MINOR.PATCH` format.

## Project Structure

```
KiCad_Forge
├── config
├── dependencies
├── docs
├── examples
├── packaging
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
│   │       ├── editing
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

> Defaults to `D:/msys64/mingw64`; set the `MSYS2_DIR` environment variable to point to a different MSYS2 installation.

Plugins can contain an independent Python backend, a TypeScript frontend, or both. The complete interfaces for the built-in LCSC and Git plugins live in the left sidebar.

- Python backends receive an action name and JSON arguments through the CLI and return JSON on stdout. The host manages tasks, status, and cancellation. Distribution packages use the bundled interpreter; source builds use system Python.
- Frontends register sidebar panels, toolbar actions, and other public slots through `@kiforge/plugin-sdk`, using the host's React and shared runtime. Plugin sources are built independently and do not import host internals.
- Built-in plugins use `manifestVersion: 2`: `python` declares the backend entry and actions, `frontend` declares frontend assets, and `engines` specifies host and SDK compatibility. Legacy v1 manifests remain supported through a compatibility path.
- User plugins live in a `plugins/` directory alongside the data directory. Built-in plugins are under `plugins/` in the distribution's resources; on macOS this is `.app/Contents/Resources/plugins/`.

Development references:

- [Public plugin SDK](../webui/packages/plugin-sdk/README.md): interfaces, slots, tasks, configuration, and build rules.
- [Example plugins](../examples/plugins/README.md): independently built frontend examples.
- [LCSC manifest](../plugins/lcsc_import/manifest.json) / [Git manifest](../plugins/git_version/manifest.json): the v2 manifests used by the built-in plugins.


## API

The built-in HTTP server exposes a REST API. Full documentation: [docs/api/http-api.md](api/http-api.md) (Chinese).

Import, LCSC, editing, and workbench contracts and acceptance scenarios are in the [feature specifications](specs/README.md) (Chinese). The specifications distinguish implemented behavior, planned rules, and verification gaps.

| Endpoint | Method | Description |
|---|---|---|
| `/api/status` | GET | Health check / heartbeat |
| `/api/symbols` | GET | List symbols (`?q=`, `?library=`) |
| `/api/libraries` | GET/POST | Symbol library CRUD |
| `/api/component-libraries` | GET/POST | Manage and import component library directory groups |
| `/api/plugins` | GET | List plugins |
| `/api/plugins/{id}/icon` | GET | Plugin icon |
| `/api/tasks` | POST | Submit a plugin action and create a task |
| `/api/tasks/{task_id}` | GET | Read task status and results |
| `/api/tasks/{task_id}/cancel` | POST | Request cancellation of a cancelable task |
| `/api/classify` | POST | Run classification rules |
| `/api/check` | POST | Check Symbol↔Footprint↔3D links |
| `/api/config` | GET/POST | Read/write configuration |
| `/api/component-type-matching` | GET/POST | Read or save type matching rules; saving does not classify parts |
| `/api/component-type-matching/preview` | POST | Preview a sample or the database without database writes |
| `/api/component-type-matching/apply` | POST | Apply a specified revision of saved rules to unclassified parts |
| `/api/component-type-matching/reload` | POST | Reload matching rules from the data directory |
| `/api/import` | POST | Import a directory (`?dir=`) |

## Tech Stack

| Layer | Choice |
|---|---|
| Language | C++23 |
| Compiler | Windows: MSYS2 Clang; Linux: GCC 14; macOS: Apple Clang |
| Build and packaging | xmake + XPack |
| Frontend | React 19 + Vite + TypeScript plugin SDK |
| Database | SQLite3 |
| HTTP | httplib (header-only) |
| Logging | spdlog |
| Desktop UI | Windows: [webview/webview](https://github.com/webview/webview) + WebView2; macOS/Linux: system browser |
| Plugin runtime | Python 3.12.15 bundled in distributions |
| License | MIT |

---

## Star History

<a href="https://www.star-history.com/?repos=xiaomaitangjiang%2Fkicad_forge&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&legend=top-left" />
 </picture>
</a>

## Contributing

Issues and PRs are welcome — workflow and code conventions in [CONTRIBUTING.md](../CONTRIBUTING.md). When filing a bug, please include a detailed description and the application's log file. "Wishful" issues are discouraged; see [Important Notes](user/usage-notes.en.md).
