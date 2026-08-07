# KiCad Forge API 文档

Base URL: `http://127.0.0.1:{port}/api`

所有请求体和响应均为 `application/json`。GET 参数放 query string，POST 参数放 body。

---

## 1. 状态

### GET /api/status

后端心跳 + 库统计。前端每 1 秒递归 `fetch` 保持心跳。

```
发送: GET /api/status
返回: { "ok": true, "symbols": 22776, "footprints": 1952 }
```

### GET /api/import-status

导入进度。前端每 2 秒轮询，`importing` 变 `false` 时触发数据刷新。

```
发送: GET /api/import-status
返回: { "importing": false, "symbols": 22776, "footprints": 1952, "models": 7241 }
```

### POST /api/bye

前端窗口关闭时 `navigator.sendBeacon` 发送，通知后端退出。

```
发送: POST /api/bye  (body 空或任意)
返回: { "ok": true }
```

---

## 2. 符号

### GET /api/symbols

| query 参数 | 类型 | 必需 | 说明 |
|---|---|---|---|
| `q` | string | 否 | 按名称模糊搜索 |
| `library` | string | 否 | 按库 ID 过滤 |
| `limit` | int | 否 | 返回条数上限，0=不限（默认） |
| `offset` | int | 否 | 跳过前 N 条 |

```
发送: GET /api/symbols?library=xxx&limit=50&offset=0
发送: GET /api/symbols?q=STM32
返回:
[
  {
    "id": "uuid",
    "name": "STM32F103C8T6",
    "library_id": "uuid",
    "value": "STM32F103C8T6",
    "footprint": "Package_QFP:LQFP-48_7x7mm_P0.5mm",
    "pins": 48,
    "mpn": "STM32F103C8T6",
    "type": "Microcontroller",
    "has_footprint": true,
    "has_3d_model": false
  }
]
```

### GET /api/footprints

```
发送: GET /api/footprints
返回:
[
  {
    "id": "uuid",
    "name": "LQFP-48_7x7mm_P0.5mm",
    "description": "LQFP, 48 pins, 7x7mm, 0.5mm pitch",
    "pad_count": 48
  }
]
```

---

## 3. 库管理

### GET /api/libraries

列出所有符号库，按 `group`（所属元件库名称）分组。

```
发送: GET /api/libraries
返回:
[
  {
    "id": "uuid",
    "name": "Diode",
    "file_path": "D:/KiCad/.../Diode.kicad_sym",
    "description": "Imported from Diode.kicad_sym",
    "group": "KiCad Libraries",
    "symbol_count": 763
  }
]
```

### POST /api/libraries

```
发送: { "action": "delete", "id": "uuid" }
返回: { "ok": true, "deleted_file": "D:/path/to/file.kicad_sym" }

发送: { "action": "delete_symbol", "id": "uuid" }
返回: { "ok": true }

发送: { "name": "My Library" }
返回: { "ok": true }
```

### POST /api/import

按目录路径导入，触发 `import_directory()`。

```
发送: POST /api/import?dir=D%3A%5CKiCad%5Csymbols
返回:
{
  "ok": true,
  "symbols": 1207,
  "footprints": 0,
  "errors": 0
}
```

---

## 4. 元件库（Component Libraries）

每个元件库是一组 `symbol_path / footprint_path / model_3d_path` 的绑定。侧边栏按此分组。

### GET /api/component-libraries

```
发送: GET /api/component-libraries
返回:
[
  {
    "id": "default",
    "name": "KiCad Libraries",
    "symbol_path": "D:/KiCad/10.0/share/kicad/symbols",
    "footprint_path": "",
    "model_3d_path": "",
    "enabled": true,
    "sort_order": 0
  }
]
```

### POST /api/component-libraries

```
发送: { "action": "add", "name": "MyLib", "symbol_path": "D:\\path" }
返回: { "ok": true, "library": { "id": "uuid", "name": "MyLib" } }

发送: { "action": "update", "id": "uuid", "name": "Renamed", "symbol_path": "D:\\newpath" }
返回: { "ok": true }

发送: { "action": "remove", "id": "uuid" }
返回: { "ok": true }

发送: { "action": "import" }
返回:
{
  "ok": true,
  "imported_symbols": 22776,
  "imported_footprints": 1952,
  "imported_models_3d": 7241,
  "auto_linked": 1234,
  "libraries": [{ "name": "KiCad Libraries", "symbols": 22776, "footprints": 1952, "models_3d": 7241 }]
}
```

---

## 5. 插件

### GET /api/plugins

```
发送: GET /api/plugins
返回:
[
  {
    "id": "com.kicad_forge.git_version",
    "name": "Git Version Control",
    "version": "1.0.0",
    "author": "",
    "description": "Git version management for libraries",
    "status": "loaded",
    "icon_url": "/api/plugins/com.kicad_forge.git_version/icon/icon.svg",
    "actions": [
      {
        "id": "init",
        "name": "Init Repo",
        "trigger": "inline",
        "button": { "show": true, "style": "icon", "location": "sidebar", "tooltip": "Initialize Git" },
        "schema": null
      }
    ]
  }
]
```

### POST /api/plugins/execute

```
发送: POST /api/plugins/execute?id=com.kicad_forge.git_version
body: { "action": "init" }
返回:
{
  "ok": true,
  "plugin_result": { "ok": true, "output": "Initialized empty Git repository..." }
}

发送: POST /api/plugins/execute?id=com.kicad_forge.lcsc_import
body: { "action": "import", "target_library": "uuid", "lcsc_ids": "C14663,C8734" }
返回:
{
  "ok": true,
  "imported_symbols": 2,
  "imported_footprints": 2,
  "plugin_result": { "ok": true, "output": "...imported..." }
}
```

### POST /api/plugins/load

```
发送: { "id": "com.kicad_forge.git_version" }
返回: { "ok": true }
```

### GET /api/plugins/{id}/icon/{filename}

返回 SVG/PNG 图标文件（非 JSON）。

### GET /api/plugins/{id}/icon

返回默认图标文件（非 JSON）。

---

## 6. 设置

### GET /api/settings

返回所有键值对。

```
发送: GET /api/settings
返回:
{
  "symbol_lib_path": "",
  "footprint_lib_path": "",
  "model_3d_path": "",
  "write_kf_id_to_file": "false"
}
```

### POST /api/settings

传入对象会合并写入 DB。旧版路径字段会触发目录导入。

```
发送: { "write_kf_id_to_file": "true" }
返回: { "ok": true, "imported_symbols": 0, "imported_footprints": 0 }

发送: { "symbol_lib_path": "D:\\KiCad\\symbols" }
返回: { "ok": true, "imported_symbols": 22776, "imported_footprints": 1952 }
```

---

## 7. 分类 / 检查 / 匹配

### POST /api/classify

对所有符号运行分类规则，更新类型。

```
发送: POST /api/classify  (无 body)
返回:
{
  "total": 100,
  "matched": 85,
  "type_updated": 50,
  "results": [
    { "name": "STM32F103", "library": "IC_MCU", "type": "Microcontroller", "confidence": 95 }
  ]
}
```

### POST /api/check

检查对应关系（缺封装、缺 3D 等）。

```
发送: POST /api/check  (无 body)
返回:
[
  { "symbol": "STM32F100", "issue": "Missing footprint", "severity": "warning" }
]
```

### POST /api/automatch

启发式匹配符号→封装。输入上限 2000 symbols × 1000 footprints。

```
发送: POST /api/automatch  (无 body)
返回:
[
  { "symbol": "STM32F100", "footprint": "LQFP-48", "score": 92 }
]
```

---

## 8. 规则 / 类型管理

### GET /api/rules

```
发送: GET /api/rules
返回: [{ "name": "Resistors", "target": "Resistor", "confidence": 100 }]
```

### GET /api/component-types

```
发送: GET /api/component-types
返回: [{ "name": "Resistor", "icon": "⊟" }]
```

### POST /api/component-types

```
发送: { "action": "add", "name": "Sensor" }
发送: { "action": "remove", "name": "Sensor" }
返回: { "ok": true }
```

### GET /api/package-types

```
发送: GET /api/package-types
返回: [{ "name": "QFN-32", "category": "SMD IC" }]
```

### POST /api/package-types

```
发送: { "action": "add", "name": "QFN-32", "category": "SMD IC" }
发送: { "action": "remove", "name": "QFN-32" }
返回: { "ok": true }
```

---

## 9. 其他

### POST /api/db/reset

清空 symbols/footprints/models 表，保留设置和 component_libraries。

```
发送: POST /api/db/reset  (无 body)
返回: { "ok": true, "message": "All data cleared. Reimport or restart." }
```

### POST /api/pick-folder

打开系统原生文件夹选择对话框（阻塞 HTTP 线程直到用户选择完毕）。

```
发送: POST /api/pick-folder  (无 body)
返回: { "ok": true, "path": "D:\\Selected\\Path" }
// 用户取消:
返回: { "ok": false, "path": "" }
```

---

## 前端调用对照

| 前端 api.js 方法 | 请求 |
|---|---|
| `api.status()` | `GET /api/status` |
| `api.symbols(q, lib)` | `GET /api/symbols?q=&library=` |
| `api.libraries()` | `GET /api/libraries` |
| `api.createLibrary(name)` | `POST /api/libraries {name}` |
| `api.deleteLibrary(id)` | `POST /api/libraries {action:delete, id}` |
| `api.deleteSymbol(id)` | `POST /api/libraries {action:delete_symbol, id}` |
| `api.plugins()` | `GET /api/plugins` |
| `api.executePlugin(id, body)` | `POST /api/plugins/execute?id= {action, ...}` |
| `api.classify()` | `POST /api/classify` |
| `api.check()` | `POST /api/check` |
| `api.autoMatch()` | `POST /api/automatch` |
| `api.settings()` | `GET /api/settings` |
| `api.saveSettings(s)` | `POST /api/settings {key:val, ...}` |
| `api.rules()` | `GET /api/rules` |
| `api.compTypes()` | `GET /api/component-types` |
| `api.pkgTypes()` | `GET /api/package-types` |
| `api.manageType(url, action, name, extra)` | `POST url {action, name, ...}` |
| `api.resetDb()` | `POST /api/db/reset` |
| `api.pickFolder()` | `POST /api/pick-folder` |
| `api.importDir(dir)` | `POST /api/import?dir=` |
| `api.compLibraries()` | `GET /api/component-libraries` |
| `api.saveCompLibrary(body)` | `POST /api/component-libraries {action, ...}` |
