# Contributing to KiCad Forge / 贡献指南

[中文](#中文) | [English](#english)

---

## 中文

- [开发流程](#开发流程)
- [代码规范](#代码规范)
- [提交前检查清单](#提交前检查清单)
- [架构约定](#架构约定)
- [目录结构](#目录结构)
- [添加插件](#添加插件)
- [不应提交的内容](#不应提交的内容)

### 开发流程

1. Fork 仓库并 clone
2. 创建 feature 分支：`git checkout -b feature/xxx`
3. 安装依赖（见 [README 快速开始](README.md#快速开始)）
4. 遵循下方规范提交修改
5. Debug + Release 均通过编译
6. 提交 PR，附带清晰的改动说明

### 代码规范

**C++**

- C++23，clang（MSYS2 MinGW 目标）
- 文件名 `snake_case`，类型名 `PascalCase`，方法名 `snake_case`
- `#pragma once`，不用 include guard
- `util::Result<T>` 处理可失败操作，核心逻辑不抛异常
- 优先用 `std::filesystem::path`，不用字符串拼接路径
- 不为第三方库做兼容修改

**前端（React）**

- 函数组件 + Hooks
- 内联 SVG 做图标，CSS 变量做主题
- `fetch()` 用相对路径（`/api/...`），不硬编码端口
- CSS class 用小写 kebab-case，按组件作用域命名

**构建**

- `xmake` 管理 C++ 编译，`npm`/Vite 管理前端
- `before_build` 检查 SVG 是否更新，按需重生成 `app.ico`
- `after_build` 复制 webui、plugins、DLL 到输出目录

### 提交前检查清单

1. `xmake build` 两种模式均编译通过
2. `npm run build` 无警告
3. 无新增编译警告
4. 冒烟测试：启动 → 窗口正常显示 → 心跳正常 → 关闭窗口后进程退出

### 架构约定

| 约定 | 说明 |
|---|---|
| **便携优先** | 运行时数据在 `./data/`（exe 同级目录），回退 `%APPDATA%/KiCad_Forge/data/` |
| **管道导入** | `ImportPipeline(db) \| symbols_from{...} \| footprints_from{...} \| models_from{...} \| execute` |
| **CRTP 平台** | `AppWindow<Derived>` 编译期派发 |
| **心跳生命周期** | 前端每 1s 调用 `/api/status`，后端 30s 无心跳则退出 |
| **插件隔离** | 插件以独立 Python 进程运行 |
| **插件发现** | 启动时扫描 `exe_dir/plugins/`，`manifest.json` 驱动全部 UI |

### 目录结构

```
src/
  api/                  HTTP 路由
  core/                 领域模型
  sexpr/                S-Expression 解析
  parser/               KiCad 文件解析
  storage/              SQLite 数据库
  classifier/           分类规则
  correspondence/       Symbol↔Footprint↔3D 追踪
  services/             业务逻辑
  plugin/               插件基础设施
  platform/             跨平台窗口（CRTP）
  util/                 工具类
plugins/                插件包
webui/                  前端
resources/              Windows .rc + app.ico
scripts/                构建辅助脚本
```

### 添加插件

参考实现：[plugins/lcsc_import/](plugins/lcsc_import/)

1. 创建 `plugins/<name>/`，放入：
   - `manifest.json`（id、name、icon、actions 等）
   - `icon.svg`（24×24 SVG）
   - `plugin.py`（接收 `action` + JSON args，输出 JSON 到 stdout）
2. 支持的字段类型：`text`、`library_picker`、`select`、`file_picker`
3. 构建——`after_build` 自动复制 `plugins/`

### 不应提交的内容

- `build/` 构建产物
- `*.db`、`*.db-wal`、`*.db-shm` 运行时数据库
- `resources/app.ico`（构建时自动生成）
- IDE 文件（`.idea/`、`*.user`）
- DLL 和 exe 等二进制文件

`.vscode/` 除外——包含共享的调试和构建配置。

---

## English

- [Workflow](#workflow)
- [Code Conventions](#code-conventions)
- [Pre-submit Checklist](#pre-submit-checklist)
- [Architecture Conventions](#architecture-conventions)
- [Directory Structure](#directory-structure)
- [Adding a Plugin](#adding-a-plugin)
- [What Not to Commit](#what-not-to-commit)

### Workflow

1. Fork & clone
2. Branch: `git checkout -b feature/xxx`
3. Install dependencies (see [README Quick Start](README.md#quick-start))
4. Follow conventions below
5. Both debug & release build cleanly
6. Submit a PR with a clear description

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
- `before_build` regenerates `app.ico` when source SVG changes
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
| **Heartbeat lifecycle** | Frontend pings `/api/status` every 1s; server exits after 30s of silence |
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
resources/              Windows .rc + app.ico
scripts/                Build helpers
```

### Adding a Plugin

Reference: [plugins/lcsc_import/](plugins/lcsc_import/)

1. Create `plugins/<name>/` with `manifest.json`, `icon.svg`, `plugin.py`
2. Supported field types: `text`, `library_picker`, `select`, `file_picker`
3. Build — `after_build` copies `plugins/` automatically

### What Not to Commit

- `build/` directory
- `*.db`, `*.db-wal`, `*.db-shm`
- `resources/app.ico` (auto-generated)
- IDE files (`.idea/`, `*.user`)
- DLL and exe binaries

`.vscode/` is tracked — it contains shared debug and build configs.
