<div align="center">

<img src="resources/icons/app.ico" width="112" height="112" alt="Kicad_Forge 图标">

# KIForge

KiCad 元件库（符号、封装库）管理器

 [![Version](https://img.shields.io/github/v/release/xiaomaitangjiang/KiCad_Forge?style=flat-square&label=release&color=45658D)](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) [![License](https://img.shields.io/github/license/xiaomaitangjiang/KiCad_Forge?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/blob/master/LICENSE) [![Platform](https://img.shields.io/badge/platform-Windows-665A8A?style=flat-square)](#下载) [![Bugs](https://img.shields.io/github/issues/xiaomaitangjiang/KiCad_Forge/bug?style=flat-square)](https://github.com/xiaomaitangjiang/KiCad_Forge/issues?q=is%3Aopen+is%3Aissue+label%3Abug)

Kicad_Forge 是一个现代化的元件库图形管理工具，旨在为 KiCad 多库文件、多元器件管理提供一种更加方便、迅捷、直观的管理工具。

简体中文 ~ [English](docs/README.en.md)





![screenshot](docs/assets/screenshots/main.png)

[功能](#功能) **·** [快速开始](#快速开始) **·** [下载](#下载) **·** [贡献](CONTRIBUTING.md) **·** [注意事项](docs/user/usage-notes.zh-CN.md)
</div>

---

## 功能

- **多元件库管理**： 元件库是符号库目录与封装库目录的集合，可附加 3D 模型目录；侧边栏按元件库分组浏览
- **批量导入**： 扫描 KiCad 库目录，自动解析 `.kicad_sym` / `.kicad_mod` 文件入库
- **搜索与过滤**： 按名称模糊搜索、按库筛选，快速定位器件
- **元件类型匹配**： 先按精确特例匹配，再执行 PCRE2 正则规则，未命中默认分类为 `Unknown`；支持选择名称、值、型号或位号字段，并编辑类型图标与颜色
- **自动关联检查**： 自动检查 Symbol↔Footprint↔3D 模型关联完整性
- **自动匹配**：启发式自动匹配封装
- **LCSC 导入**： 输入立创器件编号查找，选择目标元件库及其中的符号子库（已有或新建），再从自动找到的多个封装候选中选择复用，或新建封装
- **插件系统**： Python 插件以独立进程运行，`manifest.json` 驱动界面；内置 [LCSC 元件导入](plugins/lcsc_import/) 与 [Git 版本管理](plugins/git_version/) 插件
- **多语言** ： 目前只支持中文 / English

匹配规则支持样本预览和数据库预览。保存规则只更新配置；点击应用后才补齐现有数据中未分类的元件，已指定的类型保持不变。普通目录导入和 LCSC 导入使用同一份已保存规则。

## 快速开始

### 下载

从 [Releases](https://github.com/xiaomaitangjiang/KiCad_Forge/releases) 下载最新版本：

- **Windows**：提供安装包，下载后直接运行
- 其他平台：暂无预编译包，请从源码构建

### 如何使用
如果是**便携版**，解压到一个内文件夹后即可双击应用开始使用
如果是**安装版**，双击安装程序完成安装后即可在桌面上看到该应用，双击以打开

首次进入应用会先自动扫描并导入Kicad官方元件库，待导入完成后便可进入设置配置第三方元件库。

**注意**：目前元件库路径**只支持**配置为目录路径，不支持使用.kicad_sym文件路径配置。
```
目前默认支持的元件库格式为：
EXT_Internal_kicad_Lib
├── 3DModels
├── Docs
├── Footprints
│   ├── EXT_ACDC.pretty
│   └── EXT_Example.pretty
└── Symbols
```

![setting]()



### 环境要求

- [xmake](https://xmake.io/) 3.0.9+
- [MSYS2](https://www.msys64.org/)：`mingw-w64-x86_64-clang`、`mingw-w64-x86_64-lld`、`mingw-w64-x86_64-gcc`
- Python 3.12
- Node.js 24（前端 React 19 + Vite）

首次构建前按 [docs/development/ci.md](docs/development/ci.md#本地构建) 准备 PCRE2 和其他锁定依赖。常规构建直接使用仓库中的 `resources/icons/app.ico`；只有修改图标素材时才需安装 CairoSVG、Pillow，并手动运行 `scripts/svg2ico.py`。

> 默认使用 `D:/msys64/mingw64`，可通过环境变量 `MSYS2_DIR` 指定其他 MSYS2 安装路径。

### 构建与运行

```bash
# 1. 安装前端依赖
cd webui && npm ci && cd ..

# 2. 配置并构建（debug）
xmake f -p mingw -c
xmake build

# 或构建 release 版本
xmake f -m release && xmake build
```

构建产物在 `build/mingw/x86_64/<mode>/KiCad_Forge.exe`。构建完成后 `after_build` 会自动将 webui、plugins、config、运行时 DLL 复制到输出目录，双击 exe 即可运行（内嵌 WebView2 窗口）。

### 数据目录

便携版：exe 同级的 `data/` 目录；
安装版：`%APPDATA%/KiCad_Forge/data/`。

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
| `template` | `"lcsc_import"` 使用 LCSC 专用流程：查找器件 → 选择目标元件库 → 选择已有或新建符号子库 → 复用或新建封装；普通插件使用通用表单 |
| `analyze_action` | `lcsc_import` 模板引用的隐藏分析 action 的 id |
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
| `library_picker` | 通用表单的符号库下拉选择 |
| `component_library_picker` | `lcsc_import` 专用：选择已登记且可写的目标元件库（符号库与封装库目录） |
| `symbol_library_choice` | `lcsc_import` 专用：从目标包内的实际 `.kicad_sym` 文件选择符号子库（包含空库），或输入新子库名；不默认选择 |
| `footprint_choice` | `lcsc_import` 专用：从自动候选或搜索结果中按 UUID 复用封装，或显式选择新建 |

**style：icon**
![icon](docs/assets/screenshots/plugin-style/with-icon.png)

**style：both**
![both](docs/assets/screenshots/plugin-style/icon-and-text.png)

**style：text**
![text](docs/assets/screenshots/plugin-style/without-text.png)

## API

内置 HTTP 服务提供 REST API，完整文档见 [docs/api/http-api.md](docs/api/http-api.md)。

导入、LCSC 工作流和编辑命令的行为约束与验收场景见 [功能规格](docs/specs/README.md)。规格分别标注当前实现、拟定规则和未覆盖项。

| 端点 | 方法 | 说明 |
|---|---|---|
| `/api/status` | GET | 健康检查 / 心跳 |
| `/api/symbols` | GET | 符号列表（支持 `?q=`、`?library=`） |
| `/api/libraries` | GET/POST | 符号库的增删查改 |
| `/api/component-libraries` | GET/POST | 元件库目录集合的管理与导入 |
| `/api/plugins` | GET | 插件列表 |
| `/api/plugins/{id}/icon` | GET | 插件图标 |
| `/api/classify` | POST | 运行分类规则 |
| `/api/check` | POST | 检查 Symbol↔Footprint↔3D 关联 |
| `/api/config` | GET/POST | 读写配置 |
| `/api/component-type-matching` | GET/POST | 读取或保存元件类型匹配规则；保存不执行分类 |
| `/api/component-type-matching/preview` | POST | 预览样本或数据库中的匹配结果，不写入数据库 |
| `/api/component-type-matching/apply` | POST | 按指定版本应用已保存规则，补齐未分类元件 |
| `/api/component-type-matching/reload` | POST | 重新加载数据目录中的匹配规则配置 |
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

## Star History

<a href="https://www.star-history.com/?repos=xiaomaitangjiang%2Fkicad_forge&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=xiaomaitangjiang/kicad_forge&type=date&legend=top-left" />
 </picture>
</a>

## 贡献

欢迎提交 Issue 与 PR，流程与代码规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。提交 BUG 时请附上详细描述与软件 Log 文件；谢绝"许愿式 Issue"，详见[注意事项](docs/user/usage-notes.zh-CN.md)。
