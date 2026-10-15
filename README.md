<div align="center">

<img src="resources/app.ico" width="112" height="112" alt="Kicad_Forge 图标">

# KiCad Forge

KiCad 元件库（符号、封装库）管理器

 [![Version](https://img.shields.io/github/v/release/xiaomaitangjiang/KiCad_Forge?style=flat-square&label=release&color=45658D)](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) [![License](https://img.shields.io/github/license/xiaomaitangjiang/KiCad_Forge?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/blob/master/LICENSE) [![Platform](https://img.shields.io/badge/platform-Windows-665A8A?style=flat-square)](#下载) [![Bugs](https://img.shields.io/github/issues/xiaomaitangjiang/KiCad_Forge/bug?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/issues?q=is%3Aopen+is%3Aissue+label%3Abug)

Kicad_Forge 是一个现代化的元件库图形管理工具，旨在为 KiCad 多库文件、多元器件管理提供一种更加方便、迅捷、直观的管理工具。

简体中文 ~ [English](docs/README_en.md)

[跨平台构建、自动测试与手动发布 / Cross-platform CI and releases](docs/CI.md)

![screenshot](resources/main_view.png)

[功能](#功能) **·** [快速开始](#快速开始) **·** [下载](#下载) **·** [贡献](CONTRIBUTING.md) **·** [注意事项](docs/Attention.md)
</div>

---

## 功能

- **多元件库管理** — 将符号库 / 封装库 / 3D 模型目录绑定为元件库，侧边栏按元件库分组浏览
- **一键批量导入** — 扫描 KiCad 库目录，自动解析 `.kicad_sym` / `.kicad_mod` 文件入库（实测 22,000+ 符号秒级导入）
- **搜索与过滤** — 按名称模糊搜索、按库筛选，快速定位器件
- **自动分类** — 规则引擎自动为符号标注类型（电阻、MCU、传感器……），支持自定义类型与规则
- **关联检查与自动匹配** — 检查 Symbol↔Footprint↔3D 模型关联完整性，启发式自动匹配封装
- **插件系统** — Python 插件以独立进程运行，`manifest.json` 驱动界面；内置 [LCSC 元件导入](plugins/lcsc_import/) 与 [Git 版本管理](plugins/git_version/) 插件
- **双语界面** — 中文 / English（i18next）

## 快速开始

### 下载

从 [Releases](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) 下载最新版本：

- **Windows**：提供安装包，下载后直接运行
- 其他平台：暂无预编译包，请从源码构建

### 环境要求

- [xmake](https://xmake.io/)
- [MSYS2](https://www.msys64.org/)：`mingw-w64-x86_64-clang` 工具链，以及 `mingw-w64-x86_64-sqlite3`、`mingw-w64-x86_64-fmt`
- Python 3.10+：`pip install cairosvg Pillow`（用于从 SVG 生成应用图标）
- Node.js（前端 React 19 + Vite）

> 默认使用 `D:/msys64/mingw64`，可通过环境变量 `MSYS2_DIR` 指定其他 MSYS2 安装路径。

### 构建与运行

```bash
# 1. 安装前端依赖
cd webui && npm install && cd ..

# 2. 配置并构建（debug）
xmake f -p mingw -c
xmake build

# 或构建 release 版本
xmake f -m release && xmake build
```

构建产物在 `build/mingw/x86_64/<mode>/KiCad_Forge.exe`。构建完成后 `after_build` 会自动将 webui、plugins、config、运行时 DLL 复制到输出目录，双击 exe 即可运行（内嵌 WebView2 窗口）。

### 数据目录

便携式优先：数据（SQLite 数据库、配置）存放在 exe 同级的 `data/` 目录；目录不可写时回退到 `%APPDATA%/KiCad_Forge/data/`。

## 项目结构

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

## 插件系统

> **⚠️ 注意：插件以独立 Python 进程运行，拥有与主程序相同的文件系统访问权限，Kicad_Forge无法提供任何安全保证。请仅安装来自可信来源的插件，恶意插件可能读取、篡改或删除任意文件。**

插件放在 `plugins/` 目录下，每个插件一个文件夹，包含 `manifest.json` + Python 脚本。后端通过 CLI 调用：`python plugin.py <action> '<json_args>'`，插件输出 JSON 到 stdout。

**manifest.json**

```json
{
  "id": "com.kicad_forge.lcsc_import",
  "name": "LCSC Component Importer",
  "version": "1.1.0",
  "author": "KiCad Forge",
  "description": "Download symbols, footprints, and 3D models from LCSC by part number",
  "license": "MIT",
  "entry": "plugin.py",
  "entry_function": "import_component",
  "icon": "icon.svg",
  "capabilities": ["import"],
  "one_click": true,
  "actions": [
    {
      "id": "import",
      "name": "Import LCSC",
      "description": "Import a component from LCSC by part number",
      "trigger": "dialog",
      "template": "two_step",
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
          { "key": "lcsc_id", "label": "LCSC Part Number", "type": "text", "required": true },
          { "key": "target_library", "label": "Target Symbol Library", "type": "library_picker", "required": true },
          { "key": "footprint_choice", "label": "Footprint", "type": "search_picker",
            "source": "footprints", "near_from": "footprint_name" }
        ]
      }
    },
    {
      "id": "analyze",
      "name": "Analyze LCSC",
      "description": "Download and inspect without writing to the library",
      "trigger": "hidden",
      "button": { "show": false }
    }
  ],
  "dependencies": {
    "python": ">=3.10",
    "pip": ["easyeda2kicad"]
  }
}
```

**顶层字段**

| 字段 | 说明 |
|---|---|
| `id` / `name` | 插件唯一 ID 与显示名称（必填） |
| `version` / `author` / `description` / `license` | 插件元信息 |
| `entry` | 入口脚本，默认 `plugin.py` |
| `entry_function` | 入口函数名（预留字段） |
| `icon` | 插件图标（SVG） |
| `capabilities` | 能力声明（如 `import`、`tool`） |
| `one_click` | 是否支持一键执行 |
| `actions` | 动作列表，见下表 |
| `dependencies` | 运行依赖声明：`python` 版本约束与 `pip` 依赖包 |
| `min_app_version` / `platforms` | 可选：最低应用版本 / 适用平台 |

**actions[] 字段**

| 字段 | 说明 |
|---|---|
| `trigger` | `"dialog"` 弹窗表单 / `"inline"` 一键执行（默认） / `"hidden"` 隐藏动作（供模板引用） |
| `template` | UI 模板：`"two_step"` 两步流程（先分析预览、再确认导入） |
| `analyze_action` | `two_step` 模板引用的隐藏分析 action 的 id |
| `protocol` | `"staged-import-v1"` 分阶段导入协议 |
| `preflight` | 弹窗前预执行的 action id，结果（如 Git 状态）显示在表单顶部 |
| `auto_args` | 由宿主自动填充的参数键（如 `target_library`） |
| `button.show` | 是否显示按钮（默认 `true`） |
| `button.style` | `"icon"` / `"text"` / `"both"`（默认） |
| `button.location` | `"toolbar"`（默认） / `"sidebar"` 侧边栏 |
| `button.tooltip` | 按钮悬停提示 |

**schema.fields[] 支持的字段类型**

| `type` | 说明 |
|---|---|
| `text` | 文本输入框（默认） |
| `library_picker` | 符号库下拉选择 |
| `search_picker` | 可搜索候选列表（如封装搜索），配合 `two_step` 模板使用 |

**style：icon**
![icon](resources/with_icon.png)

**style：both**
![both](resources/icon_and_text.png)

**style：text**
![text](resources/without_icon.png)

## API

内置 HTTP 服务提供 REST API，完整文档见 [docs/API.md](docs/API.md)。

| 端点 | 方法 | 说明 |
|---|---|---|
| `/api/status` | GET | 健康检查 / 心跳 |
| `/api/symbols` | GET | 符号列表（支持 `?q=`、`?library=`） |
| `/api/libraries` | GET/POST | 库的增删查改 |
| `/api/plugins` | GET | 插件列表 |
| `/api/plugins/{id}/icon` | GET | 插件图标 |
| `/api/plugins/execute` | POST | 执行插件 action |
| `/api/classify` | POST | 运行分类规则 |
| `/api/check` | POST | 检查 Symbol↔Footprint↔3D 关联 |
| `/api/settings` | GET/POST | 读写配置 |
| `/api/import` | POST | 导入目录（`?dir=`） |



## 技术栈

| 层 | 选择 |
|---|---|
| 语言 | C++23 |
| 编译器 | clang 22（MSYS2，MinGW 目标） |
| 构建 | xmake |
| 前端 | React 19 + Vite |
| 数据库 | SQLite3 |
| HTTP | httplib（header-only） |
| 日志 | spdlog |
| WebView | [webview/webview](https://github.com/webview/webview)（Windows 下为 WebView2 Runtime） |
| 许可证 | MIT |

---

## 贡献

欢迎提交 Issue 与 PR，流程与代码规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。提交 BUG 时请附上详细描述与软件 Log 文件；谢绝"许愿式 Issue"，详见[注意事项](docs/Attention.md)。
