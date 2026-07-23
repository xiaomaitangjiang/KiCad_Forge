# KiCad Forge

图形化 KiCad 元件库管理工具。扫描本地 `.kicad_sym` 符号库和 `.kicad_mod` 封装库，提供 Web 界面浏览、分类、对应关系检查，支持 LCSC 一键导入插件。

## 快速开始

### 依赖
- [MSYS2](https://www.msys64.org/)（提供 clang、sqlite3、pthread）
- Python 3.10+（LCSC 插件）
- Node.js（前端构建）

### 构建

```powershell
# 首次：设置 MSYS2 到 PATH
$env:PATH = "D:\msys64\mingw64\bin;$env:PATH"

# 构建前端
cd webui && npm install && npm run build && cd ..

# 构建后端
xmake f --toolchain=clang -m release -c
xmake build
```

输出：`build/mingw/x86_64/release/KiCad_Forge.exe`

### 运行

双击 `KiCad_Forge.exe`，浏览器打开 `http://localhost:8080`。

在 Settings → Library Paths 中设置符号库路径，点击 Save & Import。

## 架构

```
src/
├── api/             HTTP 路由层（httplib + 自研 net/http_server）
├── core/            领域模型（Symbol, Footprint, Model3D, ComponentType）
├── sexpr/           S-Expression 解析器（tokenizer → dom → writer）
├── parser/          KiCad 文件解析器（.kicad_sym / .kicad_mod）
├── storage/         SQLite 数据库层（Repository 模式）
├── classifier/      分类规则引擎
├── correspondence/  符号↔封装↔3D 对应关系追踪
├── services/        业务编排层
├── plugin/          插件管理器（Python 脚本插件）
├── platform/        Windows 原生窗口（WebView2）
└── net/             内置 HTTP 服务器

plugins/             插件目录（运行时动态加载）
  lcsc_import/       LCSC 一键导入插件
webui/               React 前端
```

## 数据库

SQLite，位置 `%APPDATA%/kicad_forge/meta.db`。

| 表 | 说明 |
|---|---|
| `libraries` | 符号库元数据 |
| `symbols` | 符号实例（含引脚序列化） |
| `footprints` | 封装实例 |
| `models_3d` | 3D 模型引用 |
| `symbol_footprint_links` | 符号↔封装关联 |
| `footprint_model_links` | 封装↔3D 模型关联 |
| `settings` | 键值配置 |

## 插件系统

插件放在 `plugins/` 目录下，每个插件一个文件夹，包含 `manifest.json` + Python 脚本。

```json
{
  "id": "com.example.my_plugin",
  "name": "My Plugin",
  "version": "1.0.0",
  "capabilities": ["import"],
  "entry": "plugin.py",
  "one_click": true
}
```

主程序通过 `python plugin.py <action> '<json_args>'` 调用，插件输出 JSON 到 stdout。

LCSC 导入插件自带 [easyeda2kicad](https://github.com/uPesy/easyeda2kicad.py)（MIT），无需 `pip install`。

## 技术栈

| 层面 | 选择 |
|---|---|
| 语言 | C++23 |
| 编译器 | clang 22 (MSYS2) |
| 构建 | xmake |
| 前端 | React 19 + Vite |
| 数据库 | SQLite3 |
| HTTP | httplib (header-only) |
| JSON | nlohmann/json |
| 许可证 | MIT |
