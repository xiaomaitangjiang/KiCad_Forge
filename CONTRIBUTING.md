# Contributing to KiCad Forge / 贡献指南

[中文](#中文) | [English](#english)

---

## 中文

- [开发流程](#开发流程)
- [代码规范](#代码规范)
- [提交规范](#提交规范)
- [提交前检查清单](#提交前检查清单)
- [架构约定](#架构约定)
- [目录结构](#目录结构)
- [添加插件](#添加插件)
- [不应提交的内容](#不应提交的内容)

### 开发流程

1. Fork 仓库并 clone
2. 创建分支：`git checkout -b xxx/xxx`
3. 安装依赖（见 [README 快速开始](README.md#快速开始)）
4. 遵循下方规范提交修改
5. Debug + Release 均通过编译
6. 补全单元测试
7. 提交 PR，附带清晰的改动说明

### 代码规范

**C++**

- C++23，clang（MSYS2 MinGW 目标）
- 文件名 `snake_case`，类型名 `PascalCase`，方法名 `snake_case`
- `#pragma once`，不用 include guard
- `util::Result<T>` 处理可能失败的操作，核心逻辑不抛异常
- 优先用 `std::filesystem::path`，不用字符串拼接路径

**前端（React）**

- 函数组件 + Hooks
- 内联 SVG 做图标，CSS 变量做主题
- `fetch()` 用相对路径（`/api/...`），不硬编码端口
- CSS class 用小写 kebab-case，按组件作用域命名

**构建**

- `xmake` 管理 C++ 编译
- `npm`/Vite 管理前端
- 常规构建使用已提交的 `resources/icons/app.ico`；修改图标素材后手动运行 `scripts/svg2ico.py`
- 使用 Python 3.12、Node.js 24、xmake 3.0.9+；前端依赖用 `npm ci` 安装，PCRE2 与锁定依赖准备见 [docs/development/ci.md](docs/development/ci.md#本地构建)
- `after_build` 复制 webui、plugins、DLL 到输出目录

### 提交规范

采用 Conventional Commits，解释性文字以中文为主，文件名、API、SDK 和框架名保留实际名称。每个分点独立说明问题或新增功能、关键实现及效果，可以使用多句话讲清楚，不为压缩字数省掉必要信息。类型选择、消息格式和示例以 [Git 提交规范](docs/specs/COMMIT_CONVENTIONS.md) 为准。

### 提交前检查清单

1. `xmake build` 两种模式均编译通过
2. `npm run build` 无警告
3. 无新增编译警告
4. 单元测试无报错

### 目录结构

```
KiCad_Forge
├── config
├── docs
├── dist/installers
├── packaging/windows
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

### 添加插件

参考实现：[plugins/lcsc_import/](plugins/lcsc_import/)

1. 创建 `plugins/<name>/`，放入：
   - `manifest.json`（id、name、icon、actions 等）
   - `icon.svg`（24×24 SVG）
   - `plugin.py`（接收 `action` + JSON args，输出 JSON 到 stdout）
2. 通用表单支持 `text`、`library_picker`；LCSC 专用的 `lcsc_import` 模板使用 `component_library_picker` 和 `footprint_choice`，完整示例见 [README 插件系统](README.md#插件系统)
3. 构建——`after_build` 自动复制 `plugins/`

### 不应提交的内容

- `build/` 构建产物
- `*.db`、`*.db-wal`、`*.db-shm` 运行时数据库
- IDE 文件（`.idea/`、`*.user`）
- DLL 和 exe 等二进制文件

`.vscode/` 除外——包含共享的调试和构建配置。

---

## English

- [Workflow](#workflow)
- [Code Conventions](#code-conventions)
- [Commit Messages](#commit-messages)
- [Pre-submit Checklist](#pre-submit-checklist)
- [Architecture Conventions](#architecture-conventions)
- [Directory Structure](#directory-structure)
- [Adding a Plugin](#adding-a-plugin)
- [What Not to Commit](#what-not-to-commit)

### Workflow

1. Fork & clone
2. Branch: `git checkout -b feature/xxx`
3. Install dependencies (see [README Quick Start](docs/README.en.md#quick-start))
4. Follow conventions below
5. Both debug & release build cleanly
6. Submit a PR with a clear description

### Commit Messages

Use Conventional Commits with Chinese explanatory text while preserving actual file names, API and SDK identifiers, and framework names. Each numbered item should describe one change, its key implementation, and its effect. Use multiple sentences when needed for clarity; do not omit necessary context to shorten the message. See the [commit specification](docs/specs/COMMIT_CONVENTIONS.md) for the full rules and examples.

### Code Conventions

**C++**

- C++23, clang (MSYS2 MinGW target)
- `snake_case` filenames, `PascalCase` types, `snake_case` methods
- `#pragma once` over include guards
- `util::Result<T>` for fallible operations
- Prefer `std::filesystem::path` over string concatenation
- Never patch third-party library source

**Frontend (React)**

- Functional components + Hooks
- Inline SVG icons, CSS variables for theming
- `fetch()` with relative URLs (`/api/...`)
- kebab-case CSS classes scoped by component

**Build**

- `xmake` for C++, `npm`/Vite for frontend
- Use Python 3.12, Node.js 24, and xmake 3.0.9+; install frontend dependencies with `npm ci`. See [docs/development/ci.md](docs/development/ci.md#本地构建) for PCRE2 and locked dependency preparation
- Normal builds use the tracked `app.ico`; run `scripts/svg2ico.py` manually when changing the artwork
- `after_build` copies webui + plugins + DLLs to output

### Pre-submit Checklist

1. `xmake build` passes for both modes
2. `npm run build` passes without warnings
3. No new compiler warnings
4. Smoke test: launch → window renders → heartbeat works → process exits on close

### Architecture Conventions

| Convention | Detail |
|---|---|
| **Portable-first** | Runtime data at `./data/`, fallback `%APPDATA%/KiCad_Forge/data/` |
| **Pipeline import** | `ImportPipeline(db) \| symbols_from{...} \| footprints_from{...} \| models_from{...} \| execute` |
| **CRTP platform** | `AppWindow<Derived>` compile-time dispatch |
| **Heartbeat lifecycle** | Frontend pings `/api/status` every 1s; the fallback timeout is 120s and is suspended while the Windows window is minimized |
| **Plugin isolation** | Plugins run as separate Python processes |
| **Plugin discovery** | `exe_dir/plugins/` scanned at startup; `manifest.json` drives all UI |

### Directory Structure

```
src/
  api/                  HTTP routes
  core/                 Domain models
  sexpr/                S-Expression parser
  parser/               KiCad file parsers
  storage/              SQLite database
  classifier/           Classification rules
  correspondence/       Symbol↔Footprint↔3D tracker
  services/             Business logic
  plugin/               Plugin infrastructure
  platform/             Cross-platform window (CRTP)
  util/                 Utilities
plugins/                Plugin bundles
webui/                  Frontend
resources/              icons, branding and licenses
packaging/windows/      Windows .rc, resource.h and Inno Setup script
scripts/                Build helpers
```

### Adding a Plugin

Reference: [plugins/lcsc_import/](plugins/lcsc_import/)

1. Create `plugins/<name>/` with `manifest.json`, `icon.svg`, `plugin.py`
2. Generic forms support `text` and `library_picker`. The dedicated LCSC `lcsc_import` template uses `component_library_picker` and `footprint_choice`; see the [README plugin example](docs/README.en.md#plugin-system)
3. Build — `after_build` copies `plugins/` automatically

### What Not to Commit

- `build/` directory
- `*.db`, `*.db-wal`, `*.db-shm`
- IDE files (`.idea/`, `*.user`)
- DLL and exe binaries

`.vscode/` is tracked — it contains shared debug and build configs.
