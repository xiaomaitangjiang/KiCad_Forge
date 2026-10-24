# KiCad Forge API 文档

Base URL: `http://127.0.0.1:{port}/api`

导入的状态、提交/取消、幂等和界面验收规则见 [功能规格索引](../specs/README.md)。其中现状与拟定功能分开记录；规格不代表新增 HTTP 路由。

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

导入进度。前端在上一次请求结束后等待 2 秒再请求；只有有效响应中的
`importing` 明确为 `false` 时触发数据刷新。关闭弹窗不取消后台导入。

```
发送: GET /api/import-status
返回:
{
  "importing": true,
  "progress": { "phase": "symbols_parse", "completed": 35, "total": 123 }
}
```

- 本接口只读取导入任务的内存状态，不查询数据库，也不返回全库条目统计。
  全库符号、封装总数继续通过 `/api/status` 获取。
- `progress` 不依赖数据库会话。`completed`/`total` 表示当前阶段已处理文件数
  和文件总数；`total:0` 表示总量未知或该阶段不按文件计数，不应显示为百分比。
- 阶段为 `idle`、`scanning`、`symbols_parse`、`symbols_import`、`footprints_parse`、`footprints`、`models`、
  `linking`，结束阶段为 `done`、`failed` 或 `cancelled`。阶段或当前目录切换时文件计数
  可以重新开始。处理失败或跳过的文件同样推进处理进度，不能把处理文件数当作数据库条目数。

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
| `q` | string | 否 | 按名称、描述或型号（MPN）模糊搜索 |
| `library` | string | 否 | 按库 ID 过滤 |
| `limit` | int | 否 | 返回条数上限，0=不限（默认） |
| `offset` | int | 否 | 跳过前 N 条 |

`q` 与 `library` 同时提供时，先在指定符号子库内搜索，再应用 `limit` / `offset`；
不会返回其他库的同名结果。HTTP 参数名为 `library`，SDK 对应参数名为 `libraryId`。

```
发送: GET /api/symbols?library=xxx&limit=50&offset=0
发送: GET /api/symbols?q=STM32
发送: GET /api/symbols?q=STM32&library=xxx&limit=50&offset=0
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
    "type_id": "Microcontroller",
    "locked": false,
    "footprint_status": "available",
    "footprint_reason": "ok",
    "footprint_reference": "Package_QFP:LQFP-48_7x7mm_P0.5mm",
    "footprint_id": "uuid",
    "footprint_resolved_reference": "Package_QFP:LQFP-48_7x7mm_P0.5mm",
    "footprint_name": "LQFP-48_7x7mm_P0.5mm",
    "has_footprint": true,
    "model_status": "unassigned",
    "model_reason": "no_model_link",
    "model_id": "",
    "model_name": "",
    "has_3d_model": false
  }
]
```

**存在性语义（U02）**：`has_footprint` / `has_3d_model` 只由状态推导，不再看引用是否非空、
UUID 是否非空或来源键是否非空。

| 字段 | 取值 | 说明 |
|---|---|---|
| `footprint_status` | `unassigned` | 符号没有填写封装引用（不反向恢复陈旧关联） |
| | `not_found` | 有引用，但库里没有匹配的封装行 |
| | `ambiguous` | 有引用，但匹配到多条（同名跨来源），不猜一个 |
| | `stale` | 定位到了 DB 行，但文件缺失/被目录替代/来源身份不一致 |
| | `available` | DB 行 + 真实普通文件 + 来源身份一致（唯一正常态） |
| | `check_failed` | 文件系统查询本身失败（权限等），不是「不存在」 |
| `footprint_reason` | 稳定原因码 | `no_reference` `no_match` `multiple_matches` `empty_path` `missing_file` `not_regular_file` `source_key_mismatch` `stat_failed` `ok` |
| `footprint_reference` | string | 符号里的原始引用，任何状态下都保留 |
| `footprint_id` | string | 唯一定位出的真实 DB UUID；没有唯一定位时为空串 |
| `footprint_resolved_reference` | string | 规范 `Lib:Name`，可定位时提供 |
| `model_status` | `unassigned` / `stale` / `available` / `check_failed` | 至少一个 DB 关联模型文件是普通文件才算 `available` |
| `model_reason` | 稳定原因码 | `no_model_link` `model_files_missing` `footprint_unavailable` `model_check_failed` `ok` |

封装不可用时（`not_found`/`stale`/`ambiguous`/`check_failed`），`model_status` 不会是 `available`：
3D 状态建立在封装本身可用的前提上。DB 读失败返回 `500` + `{"error": "..."}`，
不会退化成空数组。

### GET /api/symbols/{id}

按符号 UUID 获取基础摘要，返回一个对象。字段与列表的基础字段一致：
`id`、`name`、`library_id`、`value`、`footprint`、`pins`、`mpn`、`type_id`、
`type`、`locked`。其中 `locked` 来自符号所属元件库的锁定状态。

```json
{
  "id": "uuid",
  "name": "STM32F103C8T6",
  "library_id": "uuid",
  "value": "STM32F103C8T6",
  "footprint": "Package_QFP:LQFP-48_7x7mm_P0.5mm",
  "pins": 48,
  "mpn": "STM32F103C8T6",
  "type_id": "Microcontroller",
  "type": "Microcontroller",
  "locked": false
}
```

本接口不检查封装或模型文件，不返回 `footprint_status`、`model_status` 等状态字段；
需要可用状态时调用 `/api/symbols/{id}/binding`。符号不存在返回 `404` +
`{"error":"..."}`；读取符号或锁定状态失败返回 `500` + `{"error":"..."}`，
不会把读取错误当成空摘要。

### GET /api/footprints

`footprint_status` / `footprint_reason` 与 `/api/symbols` 同义（只判定封装本身，
不检查模型文件）；`stale` 的候选仍会返回，前端负责禁用选择。

```
发送: GET /api/footprints
返回:
[
  {
    "id": "uuid",
    "name": "LQFP-48_7x7mm_P0.5mm",
    "description": "LQFP, 48 pins, 7x7mm, 0.5mm pitch",
    "pad_count": 48,
    "footprint_status": "available",
    "footprint_reason": "ok"
  }
]
```

### GET /api/symbols/:id/binding

与列表同一套状态字段（`footprint_status`/`footprint_reason`/`footprint_id`/
`footprint_name`/`model_status`/`model_id`/`model_name`/`has_footprint`/`has_3d_model`）。

* 符号不存在 → `404` + `{"error": "Symbol not found: ..."}`
* DB 读失败 → `500` + `{"error": "..."}`

失败时**不会**返回一个「空绑定」的正常响应（那会被前端当成「没有绑封装」）。

### POST /api/symbols/:id/bind-footprint · POST /api/footprints/:id/bind-model

```
发送: POST /api/symbols/<uuid>/bind-footprint   body: { "footprint_id": "<uuid>" }
发送: POST /api/footprints/<uuid>/bind-model    body: { "model_id": "<uuid>" }
返回: { "ok": true } 或 { "ok": false, "error": "<原因>" }
```

* 业务拒绝（目标不存在、封装文件缺失/被目录替代/来源身份不一致、符号库锁定）→
  `200` + `ok:false` + 具体原因，不写入任何东西；拒绝时源文件、旧关联、模型全部保持原样。
* 数据库读失败是 `DbError` 语义：**不会**被改写成「目标不存在」，前端按 `ok:false` 的
  原因文案显示，不得当成成功。
* 提交是两阶段的：选中 UUID 的 `name` / `footprint_lib_name` / `library_path` /
  `source_key` 必须在写库前复核为同一身份，封装内容摘要也要一致 —— 同 UUID 在等待
  期间被改名/换源/挪到另一份真实文件时整次拒绝（写下的引用与模型关联必须对应
  用户当初选中的那一行）。
* 成功替换关联时会清掉该符号的全部旧关联，只留下目标那一条。

### GET /api/footprints/search

候选保留 `id` / `name` / `library` / `score` / `pad_count`，追加
`footprint_status` / `footprint_reason`。索引构建或快照读取失败返回 `500` + JSON error，
不会被当成「没有候选」；失效候选照常返回（前端显示原因并禁用选择）。

---

## 3. 库管理

### GET /api/libraries

列出数据库登记的所有符号库，按 `group`（所属元件库名称）分组。没有符号的库也返回，
其 `symbol_count` 为 `0`；不需要附加查询参数。尚未登记到数据库的物理文件由下述
`/api/component-libraries/{id}/symbol-libraries` 接口列出。

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
  "models_3d": 0,
  "errors": 0,
  "warnings": 0,
  "error_count": 0,
  "warning_count": 0,
  "failed_files": 0,
  "messages": [],
  "error_messages": [],
  "warning_messages": []
}
```

`ok` 来自本次真实导入报告：`error_count > 0` 时为 `false`，并提供 `error`；已经提交的
文件计数仍保留。`errors` / `warnings` 是完整事件计数，分别与 `error_count` /
`warning_count` 相同；`error_messages` / `warning_messages` 是有限明细，不能用数组长度
代替完整计数。合并的 `messages` 错误在前、告警在后，最多 100 条。`failed_files` 统计未提交
的来源文件，一次提交错误可以对应多个失败文件。

响应还包含 `skipped`、`linked_symbols`、`linked_models`，以及本次符号匹配的 `matched`、
`unmatched`、`conflicts`、`input_too_long`、`evaluation_limited`、`evaluation_errors`。
`symbols` 统计已提交的符号库文件数；它与库中元件条目总数不同。匹配配置未加载或加载失败时，
新导入返回真实错误，不再返回固定的 `ok:true` / `errors:0`。

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

### GET /api/component-libraries/{id}/symbol-libraries

列出指定元件库包中可供 LCSC 导入选择的符号子库。读取包的符号路径下真实的
`.kicad_sym` 文件，包含物理空文件、空符号库和尚未登记到数据库的文件，不按元件数量过滤。

```json
{
  "ok": true,
  "libraries": [
    { "name": "Device", "path": "nested/Device.kicad_sym", "id": "uuid" },
    { "name": "Empty", "path": "Empty.kicad_sym", "id": "" }
  ],
  "can_create": true,
  "case_sensitive": false
}
```

- `path` 是相对包的符号根目录的路径，分隔符统一为 `/`；选择子库时按此路径提交，
  不按显示名称或数据库 UUID 推断。已登记文件返回其库 `id`，未登记文件的 `id` 可为空。
- 包的符号路径直接配置为单个 `.kicad_sym` 文件时，`path` 是该文件的 basename，
  `can_create:false`，不允许在旁边新建其他符号子库。
- 目录模式下，符号根目录尚未创建时可以返回空 `libraries` 和 `can_create:true`。
  文件系统访问错误返回失败，不伪装成「没有子库」。
- `case_sensitive` 用于界面比较新名称与已有文件：Windows 为 `false`，其他平台为 `true`。
  宿主提交前仍检查实际目标文件是否存在；界面检查不能替代最终检查。
- 此接口只列出目标，不创建目录、文件或数据库记录。

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
  "libraries": [],
  "errors": 0,
  "warnings": 0,
  "error_count": 0,
  "warning_count": 0,
  "failed_files": 0,
  "messages": [],
  "error_messages": [],
  "warning_messages": []
}
```

`action:"import"` 同步返回与 `/api/import` 相同的完整报告计数及有限明细；有错误时
`ok:false`，仍保留已提交的 `imported_*` / `auto_linked` 计数。`imported_symbols` 同样按
符号库文件计数。手动重导忽略本次文件时间缓存；它在健康检查前不会先清空旧缓存。

新增或编辑路径时，`symbol_path` 接受目录或单个 `.kicad_sym` 文件；`footprint_path`
必须是 `.pretty` 目录或包含封装库的目录，不能填写 `.kicad_sym` 或 `.kicad_mod` 文件。
`model_3d_path` 必须是目录，不能填写单个 `.step`、`.stp`、`.wrl`、`.iges` 或 `.igs` 文件。
已存在的路径优先按实际文件类型检查，名称以 `.STEP` 结尾的真实目录仍可使用。
合法但尚未创建的目录可以保存，供后续 LCSC 新建来源使用；实际扫描导入要求来源存在。
尚未创建但带上述模型文件扩展的路径会被拒绝，不会被当成待建模型目录。
路径检查失败不修改登记，返回：

```json
{"ok":false,"error_code":"invalid_library_path","field":"footprint_path","error":"具体路径及修正说明"}
```

旧配置在开始导入时也会检查，错误来源会在写入及关联重算前报错，不再按空目录忽略。
同一执行器已有手动导入时，重复请求立即返回 `ok:false` 和错误说明，不排队重复导入。

### POST /api/library-manifest

组件库语义清单：按仓库根与目标元件库导出可比较的符号、封装和 3D 模型快照，供 Git 语义 diff 等只读场景使用。
接口只读，不写仓库、不改数据库。

```
发送: { "root": "D:\\libs\\main", "target": "D:\\libs\\main\\Symbols" }
返回: {
  "ok": true, "manifest_version": 1,
  "libraries": [{ "id": "uuid", "name": "Device", "source_key": "...", "component_library_id": "uuid" }],
  "symbols": [{
    "uuid": "uuid", "library_id": "uuid", "name": "R1",
    "footprint_ref": "Resistor_SMD:R_0402", "footprint_uuid": "uuid",
    "kf_id": "KF...", "pre_kf_id": "",
    "pins": [{ "number": "1", "name": "~", "electrical_type": "passive" }],
    "properties": { "Value": "10k" }
  }],
  "footprints": [{
    "uuid": "uuid", "name": "R_0402", "library": "Resistor_SMD",
    "pads": [{ "number": "1", "type": "smd", "shape": "roundrect", "x": 0, "y": 0, "width": 1, "height": 1, "drill": 0 }]
  }],
  "models": [{ "uuid": "uuid", "path": "3dmodels/R_0402.step" }]
}
```

- `root` 是 Git 仓库根（或用于筛选元件库的目录）；`target` 是用户选中的元件库目录，
  可省略。两者共同决定纳入的符号库，不会返回仓库外无关的库。
- 符号按 UUID 识别；`library_source_key` / `footprint_ref` 用于展示和跨版本匹配。
  `Kicad_Forge_ID` 只表示身份版本（name + pin signature + footprint），属性修改不会改它。
- `pads` 在释放数据库会话后从 `.kicad_mod` 解析，不能把解析失败当成「没有焊盘」：
  失败时该封装带 `parse_error`。
- 3D 模型路径尽量相对 `root`；插件侧对实际文件补 `sha256`，文件缺失为 `null`。
- 清单只包含语义字段，不返回数据库整个 `properties_json`；插件写入
  `.kforge/manifest.json` 前会补 `content_sha256` 与模型 `sha256`，差异视图按 UUID
  对清单做 pin/pad/属性/封装引用/3D 哈希差分。

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
    "manifest_version": 2,
    "enabled": true,
    "revision": 3,
    "icon_url": "/api/plugins/com.kicad_forge.git_version/icon/icon.svg",
    "actions": [
      {
        "id": "init",
        "name": "Init Repo",
        "trigger": "inline",
        "cancelable": false,
        "button": { "show": true, "style": "icon", "location": "sidebar", "tooltip": "Initialize Git" },
        "schema": null
      }
    ],
    "frontend": {
      "base_url": "/plugin-assets/com.kicad_forge.git_version/",
      "entry_url": "/plugin-assets/com.kicad_forge.git_version/frontend/dist/index.js",
      "styles": ["/plugin-assets/com.kicad_forge.git_version/frontend/dist/style.css"],
      "version": "1.2.0",
      "provides": [],
      "requires": []
    }
  }
]
```

- `enabled/revision` 来自统一任务服务；`revision` 仅进程内，重启后从 0 开始。
- `actions[].protocol` 导出清单中的协议；公共导入适配器选择唯一的
  `staged-import-v1` 动作，并使用其 `analyze_action`，不依赖固定动作名。
- `frontend` 仅在插件声明前端时出现；`manifest_version` 缺省 1（旧插件无前端）。

### GET /api/plugins/{id}/state

```
发送: GET /api/plugins/com.kicad_forge.lcsc_import/state
返回: { "ok": true, "plugin_id": "...", "enabled": true, "revision": 2 }
```

未知插件 404 `plugin_unknown`；任务服务不可用 503 `unavailable`。

### POST /api/plugins/{id}/state

```
发送: { "enabled": false, "if_revision": 2 }
返回: { "ok": true, "plugin_id": "...", "enabled": false, "revision": 3,
        "cancel_requested": ["<task_id>"] }
```

停用先关闭该插件的新任务准入，再对可取消且未提交的任务发出取消请求；`cancel_requested`
只列本次新请求的任务，不承诺已取消。`if_revision` 不匹配返回 409 `revision_conflict` 与当前值。

### POST /api/tasks

```
发送: {
  "plugin_id": "com.kicad_forge.git_version",
  "action": "status",
  "args": { "target_library": "D:\\libs\\main\\Device.kicad_sym" },
  "client_request_id": "<可选>"
}
返回: { "ok": true, "task_id": "<uuid>", "state": "running" }
```

分派规则：manifest action `protocol:"staged-import-v1"` 走完整宿主暂存/校验/提交；其它动作
执行插件 Python。停用插件返回 409 `plugin_disabled`，并发超限 429 `busy`。

### GET /api/tasks/{task_id}

返回平铺任务记录：

```json
{
  "ok": true, "task_id": "...", "plugin_id": "...", "action": "status",
  "state": "succeeded", "phase": "committing", "cancelable": false,
  "cancel_requested": false, "revision": 3,
  "created_at": 0, "started_at": 1, "finished_at": 2,
  "result": {}, "error": "", "uncertain": false
}
```

`state`: `queued|running|succeeded|failed|cancelled`；`phase`: `queued|staging|committing`
（普通动作可省略）。写动作超时/中断时 `failed` + `uncertain:true`。终态只写一次；
`cancelled` 只在进程收回后发布。Python 进程成功但业务 JSON `ok:false` 时，任务记
`failed`，`result` 仍保留原 JSON，`error` 取其中的具体错误；旧同步入口继续返回原
`plugin_result`（`ok:false`），不把业务失败伪装成成功。

### POST /api/tasks/{task_id}/cancel

```
返回: { "ok": true, "status": "cancel_requested" | "already_requested" |
        "not_cancelable" | "terminal", "task": { ... } }
```

取消与开始提交共用任务锁：取消先到→`tryEnterCommit` 失败且不写库；提交先到→
`not_cancelable`。取消请求本身不写终态。

### GET /api/plugins/{id}/tasks

返回 `{ "ok": true, "plugin_id": "...", "tasks": [ ... ] }`（按创建顺序的当前进程任务）。

### GET /api/imports/operations/{operation_id}

只读回放已提交导入：`{ "ok": true, "operation_id": "...", "result": { ... } }`；
不存在 404 `operation_not_found`，绝不创建记录。

### GET /api/plugin-diagnostics

返回宿主发现阶段的诊断数组：`[{ "code": "...", "plugin_id": "...", "message": "..." }]`。
当前 code：`manifest/invalid`、`manifest/unsupported-version`、`plugin/duplicate-id`。

### GET/POST /api/plugin-config/{id}

`GET` 返回 `{ "ok": true, "config": { ... } }`；`POST` 接受 JSON 对象并返回 `{ "ok": true }`。
底层键为 `ConfigStore` 的 `plugins.<id>.config`，上限 64 KiB；未知插件 404，非法/超限 400。

### GET /plugin-assets/{id}/{path...}

插件前端静态资源（入口 JS/CSS/图片）。路径根为该插件 manifest 的 `frontend.root`，
拒绝绝对路径/`..` 越界；`Cache-Control: no-store`，按扩展名返回 Content-Type。

字段规则：

- `target_component_library_id` 与旧的 `target_library_id`/`target_library`（符号库入口）
  互斥；两者都给或都不给都是错误。包必须存在、enabled、未 locked/protected，
  且符号路径与封装路径齐全。
- `symbol_library_choice` 仅供元件库包模式使用：
  `{"mode":"existing","path":"nested/Device.kicad_sym"}` 选择根目录内的已有文件，
  空文件或尚未登记到数据库的文件同样可选；`{"mode":"new","name":"Switches"}`
  在符号根目录下新建 `Switches.kicad_sym`。`name` 为不带后缀的单个名称，不能是路径；
  界面可接受一次 `.kicad_sym` 后缀，并在提交前移除。选择已有文件时，`path` 使用列表接口
  返回的目录相对路径，不接受任意绝对路径或根目录外路径。
- 新界面不默认选择符号子库；未选择时不能继续选择封装或导入。切换包或子库会清除封装选择，
  并为重新确认的导入生成新的 `operation_id`。子库的新建仅在实际提交时发生，不覆盖已存在文件；
  包的符号路径直接指向单个文件时不允许新建子库。已有子库沿用原库 UUID 和来源路径身份，
  文件与数据库变更仍通过同一套变更事务提交。
- 兼容旧客户端：元件库包请求省略 `symbol_library_choice` 时，仍写入符号目录下的
  `LCSC.kicad_sym`，或包配置指定的单个符号库文件。旧 `target_library_id`/`target_library`
  入口不接受 `symbol_library_choice`。
- 元件库包模式必须显式给 `footprint_choice`：`mode` 只能是 `"new"` 或 `"reuse"`，
  `reuse` 必须带候选的封装 UUID（不默认取匹配度最高的一个，也不把空选择当新建）。
  封装 UUID 三处写法等价：`footprint_choice.footprint_id`（本文档示例）、
  `footprint_choice.id`、顶层 `footprint_id`；同时给出且不一致的请求直接拒绝，
  `mode:"new"` 不允许携带任何旧 UUID。旧符号库入口保持旧协议：`footprint_choice`
  可省略，`mode` 默认 `"new"`，此时 `footprint_choice.library`/`name` 仍按唯一定位解析。
- 字符串字段类型不符给具体消息（如 `provider must be a string`），不返回笼统的
  `Execute error`。
- 真实 LCSC 插件（`com.kicad_forge.lcsc_import`）在元件库包模式下先规范化编号
  （去首尾空白、统一大写、纯数字补 `C`，只接受 `C`+数字）再做来源/幂等判定：
  `" c123 "`、`"C123"`、`"123"` 是同一个来源，非法编号在执行插件之前拒绝。
  其它第三方插件的外部编号原样使用。
- 同一 `operation_id` 重放返回原结果（`"replayed": true`，不重跑插件、不写库）；身份由
  「目标包 ID + 规范目标路径 + 模式」组成，换包/换符号子库/换模式/换复用 UUID 会被拒绝。
  成功的新建请求回放先于子库重名检查，不会因为上次已经创建了文件而拒绝合法重试。
  历史成功回放先于复用行读取：复用的那一行后来被删掉，回放照旧返回当时的结果；
  但**新的**导入请求拿同一个失效 UUID 会被拒绝。
- 新导入先领取已保存匹配配置的健康快照；配置未加载或加载失败时，在执行插件及宿主写入前
  返回错误。既有成功 `operation_id` 的历史结果回放先于该健康门，仍按既有身份约束返回原结果，
  不重新匹配类型，也不改写历史结果。
- 类型匹配使用插件产物合并后的真实 `Name` / `Value` / `MPN` / `Reference`。`model` 指 MPN，
  与 3D 模型无关；没有 `Reference` 属性时不从名称或位号前缀补造。已有类型身份保留，
  只有空值 / 字面 `Unknown` 可被匹配结果补齐。来源更新仍保留符号 UUID。
- `plugin_result.warnings` 是类型匹配的可读告警数组。冲突、输入过长、求值超限或引擎错误时，
  未分类行保留未分类状态，导入可以正常提交；这些告警不把匹配失败伪装成命中。无告警时为空数组。
- `reuse` 是两阶段复核：插件执行前、提交前各查一次那一行 —— UUID、`name`、
  `footprint_lib_name`、`library_path`、`source_key` 必须始终是同一个身份，文件必须是
  真实普通文件。插件执行期间该行被改名/换源/挪到另一份文件、或复用文件被删除时，
  整次导入失败：宿主文件与数据库改动一起回滚（外部对数据库的改动不属于宿主的回滚对象）。
- 数据库读失败原样上报（JSON error），不会包装成「封装不存在」；插件不会被执行。
- `analyze`（`action:"analyze"`，无目标库即可调用）返回规范化编号、器件/封装信息与
  **完整**候选列表（`id`/`name`/`library`/`score`/`pad_count`，以及
  `footprint_status`/`footprint_reason` —— 与 `/api/footprints/search` 同一套存在性
  状态），同名不同 UUID 全部保留；宿主查询不可用时返回 `warning`（新建仍可继续），
  不报成「确定无匹配」。失效候选仍会返回，由前端显示原因并禁用选择。

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

### GET /api/config

返回所有键值对。

```
发送: GET /api/config
返回:
{
  "symbol_lib_path": "",
  "footprint_lib_path": "",
  "model_3d_path": "",
  "write_kf_id_to_file": "false"
}
```

### POST /api/config

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

使用已保存的元件类型匹配配置执行批量应用。只补齐空类型 / 字面 `Unknown`；已有内置、
自定义及未登记历史类型身份均保留。此兼容入口不要求 `expected_revision`，与下文
`/api/component-type-matching/apply` 使用同一应用服务。

```
发送: POST /api/classify  (无 body)
返回:
{
  "ok": true,
  "total": 100,
  "matched": 85,
  "type_updated": 50,
  "results": [
    { "name": "STM32F103", "library": "IC_MCU", "type_id": "Microcontroller", "type": "Microcontroller", "confidence": 95 }
  ]
}
```

`total` 包含受保护的行；`matched` 是未分类行的命中数；`type_updated` 只计已提交更新。
完整响应还提供共同的 `summary` / `details` / `source:"saved"`，`results` 与 `details`
共用相同明细，最多 100 条。执行部分失败时返回 `ok:false` 和 `error`，旧计数字段与真实
`summary` / `details` 仍保留。进入应用报告前失败则只返回错误，不伪造空的成功报告。

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

该接口保留既有推荐库规则展示。下面的元件类型匹配接口管理实际类型判定配置。

### GET /api/component-type-matching

配置作用域固定为 `global`，所有库共用；读取不会重新加载文件或触发分类。正式启动顺序是
类型注册表 → 匹配配置加载 → 导入管理器 → API。匹配配置路径在启动时确定为数据目录下
`config/component_type_matching.json`，API 注册时保存该路径副本。

```json
{
  "schema_version": 1,
  "revision": 7,
  "rules": [{
    "id": "ref-resistor",
    "enabled": true,
    "field": "reference",
    "pattern": "^R[0-9]+$",
    "case_sensitive": false,
    "priority": 0,
    "type_id": "Resistor"
  }],
  "exceptions": [],
  "ok": true,
  "load_error": "",
  "has_snapshot": true,
  "scope": "global",
  "engine": {
    "name": "PCRE2", "dialect": "PCRE2", "code_unit_width": 8,
    "semantics": "byte", "utf": false, "ucp": false,
    "case_folding": "ASCII", "jit": false, "search": "regex_search",
    "newline": "ANYCRLF", "multiline": false, "dotall": false,
    "dollar_end_only": true, "wall_clock_timeout": false
  },
  "limits": {
    "max_rules": 500, "max_exceptions": 500,
    "max_pattern_bytes": 512, "max_exception_key_bytes": 4096,
    "max_source_field_bytes": 4096,
    "min_priority": -2147483648, "max_priority": 2147483647,
    "max_revision": 9007199254740991,
    "max_callouts_per_evaluation": 100000, "match_limit": 100000,
    "depth_limit": 256, "heap_limit_kib": 1024, "parens_nest_limit": 256
  }
}
```

`schema_version/revision/rules/exceptions` 是完整配置 DTO 的四个顶层字段。`schema_version`
固定为 1；`revision` 是非负安全整数。每条 rule 需要示例中的全部字段；exception 需要
`id/enabled/field/key/type_id`。两张表的 `id` 全局唯一；exception 的 `field + key` 组合唯一，
`key` 非空。`type_id` 必须登记为元件类型，禁用条目同样进行结构、类型引用及正则编译校验。

四个匹配来源字段均使用原文、不 trim：`name` 是名称，`value` 是值，`model` 是型号 MPN，
`reference` 是实际 Reference 属性。`model` 与 3D 模型无关。缺失的 Reference 与显式空串不同：
缺失不参与该字段匹配，显式 `"reference":""` 可以命中 `^$`；不按名称或 `reference_prefix`
补造来源。精确特例先于正则、区分大小写；正则采用搜索语义，取命中项的最高数值 priority，
同级最高命中给出多个类型时报告冲突。

`engine` 描述实际 PCRE2 字节语义。UTF/UCP 与 JIT 均关闭；大小写折叠仅限 ASCII；`^$` 使用
整段文本边界，`$` 不提前匹配末尾换行。工作量限制由核心常量提供，触发后报告
`evaluation_limit`，不转成未命中；它不构成墙钟超时承诺。响应没有 `engine.version` 字段。

配置加载失败时仍开放 API：`ok:false`、`load_error` 给出真实错误。若有最后有效快照，继续
展示其四个配置字段和 `has_snapshot:true`；首次加载就失败时为 `has_snapshot:false`，不伪造
空 `rules/exceptions` 或 revision。缺文件时安装默认内存快照并绑定同一个保存路径，首次
普通保存才落盘。未执行启动装配时返回明确的 `assembly error`，不会自动寻找其他配置。

### POST /api/component-type-matching

仅持久化配置，保存成功不会执行数据库预览、应用或导入。必须提供完整 `config` 和
`expected_revision`；不接受浮点数、负数、null 或超出安全整数范围的 revision。

```json
{
  "expected_revision": 7,
  "config": {
    "schema_version": 1,
    "revision": 7,
    "rules": [{
      "id": "ref-resistor", "enabled": true, "field": "reference",
      "pattern": "^R[0-9]+$", "case_sensitive": false,
      "priority": 0, "type_id": "Resistor"
    }],
    "exceptions": []
  }
}
```

成功返回 `{"ok":true,"revision":8}`。`config.revision`、`expected_revision` 与当前保存版本
必须一致；严格校验、CAS 或写盘失败均返回 `{"ok":false,"error":"具体原因"}`，不发布新
快照、不推进 revision。配置处于 `load_error` 时拒绝普通保存，需修复原配置并显式 reload。
达到最大安全 revision 后不能继续自增。

### POST /api/component-type-matching/reload

请求体必须为对象，当前只接受 `{}`。重新读取启动时确定的配置路径，并返回与 GET 相同的
状态形状；失败时还返回 `error`。不接受 `path` 或其他字段，也不会使用请求提供的文件路径。
首次坏加载时仓储尚未绑定保存路径，reload 仍使用启动路径副本，因此修好原文件即可恢复。
坏文件不会因 GET、保存失败或启动失败被覆盖。

### POST /api/component-type-matching/preview

请求体只接受可选的完整草稿 `config` 和可选的 `sample`。仅按 `sample` 键是否存在选择模式：
`{"sample":{}}` 是纯样本预览；`sample:null`、数组、非字符串字段均拒绝。样本只接受
`name/value/model/reference` 四个可选字符串，省略与空串原样保留。

```json
{"sample":{"reference":"R12"}}
```

此请求使用已保存的健康快照；响应示例：

```json
{
  "ok": true, "mode": "sample", "source": "saved", "revision": 7,
  "sample": {"reference":"R12"},
  "status": "matched", "type_id": "Resistor", "reason": "",
  "structured_evidence": [{
    "source": "rule", "id": "ref-resistor", "field": "reference",
    "text": "R12", "priority": 0, "type_id": "Resistor"
  }],
  "load_error": "", "saved_healthy": true
}
```

同时给出 `sample` 与完整 `config` 时，响应为 `mode:"sample",source:"draft"`，revision 来自
草稿。这条路径只编译和求值草稿，不访问 DB、不保存配置、不领取已保存业务快照；已保存配置
损坏时仍可诊断合法草稿，响应通过 `saved_healthy:false` 与真实 `load_error` 标明该状态。
没有草稿的样本预览必须通过已保存配置健康门。

省略 `sample` 时是数据库预览，例如 `{}` 使用已保存配置，`{"config":完整配置DTO}` 使用
草稿求值；二者都调用相同的批量预览服务，都要求已保存配置健康。响应包含
`ok:true,mode:"database",source:"saved"|"draft",summary,details`。预览从不更新数据库，
全部明细的 `write_status` 为 `read_only`。草稿不能绕过 DB 预览的健康门。

### POST /api/component-type-matching/apply

请求体只接受 `{"expected_revision":7}`。仅应用已保存的健康快照，禁止 `config`、`sample`
及其他额外字段；版本不符时在写入前失败。只补齐 SQL NULL / 空串 / 字面 `Unknown`，已存在
的内置、自定义和未登记历史类型身份均受保护，来源字段不会被分类过程改写。

响应包含 `ok/summary/details/source:"saved"`。每 500 个待写候选使用一个短事务；每行同时
复核原类型与四个来源字段，来源变化、类型被并发改写、行被删除分别报告真实跳过状态。
只有 COMMIT 成功才增加 `type_updated`；当前批失败回滚该批，已经提交的前批保持已提交。

进入应用报告前的错误只返回 `ok:false,error`。执行报告内出现错误时同样 `ok:false`，并保留
真实 `summary/details`。下面摘录一个前批 500 条已提交、后批 200 条回滚的报告字段：

```json
{
  "ok": false,
  "error": "具体数据库错误",
  "source": "saved",
  "summary": {
    "total": 700, "revision": 7, "protected_count": 0,
    "matched": 700, "unmatched": 0, "conflicts": 0,
    "input_too_long": 0, "evaluation_limited": 0, "evaluation_errors": 0,
    "would_update": 700, "unchanged": 0, "type_updated": 500,
    "source_changed": 0, "cas_skipped": 0, "deleted": 0,
    "rolled_back": 200, "not_attempted": 0
  },
  "details": [{
    "symbol_id": "某个已提交行的UUID", "name": "R12",
    "raw_type_id": "Unknown", "candidate_type_id": "Resistor",
    "type_id": "Resistor", "status": "matched", "would_update": true,
    "write_status": "updated"
  }]
}
```

### 匹配批量报告字段

数据库 preview、apply 和旧 classify 共用同一 serializer。`summary` 覆盖全部行，`details`
最多 100 条，明细上限不限制计数。上述示例的 details 只摘录一条的部分字段；实际每条返回
下表全部字段。

| summary 字段 | 含义 |
|---|---|
| `total` / `revision` | 仓储读取的全部行数（含受保护行）/ 本次求值快照版本 |
| `protected_count` | 已有类型身份受保护，不产生新候选 |
| `matched` / `unmatched` / `conflicts` | 未分类行的命中、未命中、冲突数 |
| `input_too_long` / `evaluation_limited` / `evaluation_errors` | 未分类行的超长、求值超限、引擎错误数 |
| `would_update` / `unchanged` | 计划需要更新 / Matched 但候选等于原值 |
| `type_updated` | 实际已经提交的类型更新；预览为 0 |
| `source_changed` / `cas_skipped` / `deleted` | 已提交批中来源变化、原类型变化或 CAS 未写、行已删除的跳过数 |
| `rolled_back` / `not_attempted` | 当前失败批已开始但回滚的候选 / 未开始的候选 |

决策计数满足 `total = protected_count + matched + unmatched + conflicts + input_too_long +
evaluation_limited + evaluation_errors`。`matched`、`would_update`、`type_updated` 各有含义，
不能互相替代。

| details 字段 | 含义 |
|---|---|
| `name/library/type_id/type/confidence` | 旧界面展示字段；`type_id` 优先展示候选或原身份，不能据此认定已写入 |
| `symbol_id/raw_type_id/sample` | 同一次仓储读快照的 UUID、原类型与四个真实来源；sample 不补缺失键 |
| `status/reason/evidence` | 决策状态、人读原因、依据摘要 |
| `candidate_type_id/structured_evidence` | 真实候选与结构化依据；protected 行不伪造候选 |
| `is_protected/would_update` | 类型身份保护轴 / 本次读取事实形成的更新计划 |
| `write_status/write_reason` | 实际写入过程的枚举名与原因，与决策状态分开 |

`status` 是 `protected/matched/unmatched/conflict/input_too_long/evaluation_limit/evaluation_error`。
`structured_evidence` 的每项固定含 `source/id/field/text/priority/type_id`，`source` 为
`rule` 或 `exception`。`write_status` 是 `read_only/not_eligible/unchanged/not_attempted/updated/
source_changed/cas_skipped/deleted/rolled_back`；不要从 `reason` 文本反推枚举或更新数量。
仓储当前把 SQL NULL 与空类型都读为空串，`raw_type_id` 不伪造二者的区别。

这些匹配接口沿用 `200` 与 JSON 业务结果；调用方须读取 `ok`、`error` 或 GET 的
`load_error`，不能只按 HTTP 成功判断业务成功。POST body 必须是 JSON 对象，额外请求字段
返回具体错误；不完整配置及错误字段类型由正式核心校验器拒绝。

### GET /api/component-types

```
发送: GET /api/component-types
返回: [{ "id": "Resistor", "name": "Resistor", "icon": "⊟", "color": "#34C759", "builtin": true }]
```

- `id` 是不可变身份（筛选、已落库数据都用它）；`name`/`icon`/`color` 只用于展示：
  改名、重排、改图标/颜色都不改变任何已存的类型归属
- 新增或显式更新的 `icon` 最多 64 UTF-8 字节（可为空 = 界面用通用占位）；
  写入的 `color` 为空串或 `#RRGGBB`，空串 = 沿用默认文字颜色。GET 原样读取历史展示字段，
  具体兼容规则见下文
- `builtin: true` 的内置类型可以改展示字段，但不可删除（ID 也不可变）；
  界面据此禁用删除键，真正的拒绝仍在服务端
- 未登记的历史 `type_id`（例如 `custom:...` 已从配置里消失）在 `/api/symbols`
  的 `type` 字段里原样返回该 ID，不会被改写成 `Unknown`

### POST /api/component-types

```
发送: { "action": "add", "name": "Sensor", "icon": "⊟", "color": "#34C759" }
发送: { "action": "update", "id": "custom:1a2b...", "name": "Sensor2" }
发送: { "action": "remove", "id": "custom:1a2b..." }
返回: { "ok": true, "id": "custom:1a2b..." }
      { "ok": false, "error": "Type not found: custom:1a2b..." }
      { "ok": false, "error": "Type is still referenced; reclassify first", "references": 3 }
```

- `action`：`add` / `update`（`rename` 同义）/ `remove`；其它值返回 `{ "ok": false, "error": "Invalid" }`
- `add`：`name` 必填，`icon` / `color` 省略即空；新增时显式给出的 `icon` / `color` 按下面的规则校验
  （颜色要写 6 位十六进制，例如 `#34C759`）；成功返回新生成的 `id`
- `update`：按 `id` 定位（旧调用只给 `name` 时按唯一名字匹配）；字段省略或为 `null` = 保持原值；`icon` / `color` 显式空串 = 清空（落盘时省略该字段，重新加载按空串读回）；`name` 不得为空，显式空串返回 `{ "ok": false, "error": "Type name must not be empty" }`；ID 不随改名变化
- 展示字段校验（只针对**本次真正写下去的字段**：`add` 的字段与 `update` 里显式给出的字段；
  没有给出的字段不参与校验，因此只改名称不会被旧的颜色/图标挡下）：
  - `icon` 最多 64 UTF-8 字节（emoji/中文按字节算），超限返回
    `{ "ok": false, "error": "Icon must be at most 64 UTF-8 bytes (got N)" }`
  - `color` 只接受空串或 `#RRGGBB`（大小写皆可），否则返回
    `{ "ok": false, "error": "Color must be #RRGGBB or empty: ..." }`
  - 校验失败时内存与文件都保持调用前的样子
  - 限制只约束**新的写入**：`GET`/加载历史配置不走这套校验，旧文件里已有的长图标或非
    `#RRGGBB` 颜色（历史值，例如 `red`）照样加载并原样返回；只改名称也不会把它擦掉或改写成别的值。
    显式把这些历史值改成合法值或清空则照常提交
- `remove`：内置类型与被引用类型拒删。组件类型删除同时检查真实 DB 引用与匹配配置引用
  （含禁用规则、特例及仍被进行中操作持有的旧快照）。正常删除结果返回 `references`、
  `matching_referenced`、`reason`：`references` 只计 DB 行数，匹配引用不伪装成 DB 行数；
  匹配配置健康门不可用时明确报错，不以零引用放行
- 落盘失败一律 `{ "ok": false, "error": ... }`，内存与文件都保持调用前的样子

例如数据库没有使用该类型、但禁用规则或旧快照仍引用它时，拒绝响应中的引用轴分别为
`"references":0,"matching_referenced":true`，同时提供具体 `reason` 和 `error`。
同时存在数据库引用时 `references` 如实返回实际行数。成功删除时返回
`"references":0,"matching_referenced":false,"reason":""`，以及被删除的 `id`。

### GET /api/package-types

```
发送: GET /api/package-types
返回: [{ "name": "QFN-32", "category": "SMD IC" }]
```

### POST /api/package-types

```
发送: { "action": "add", "name": "QFN-32", "category": "SMD IC" }
发送: { "action": "update", "id": "custom:3c4d...", "name": "QFN-32-EP" }
发送: { "action": "remove", "id": "custom:3c4d..." }
返回: { "ok": true, "id": "custom:3c4d..." }
      { "ok": false, "error": "Built-in types cannot be deleted" }
      { "ok": false, "error": "Type is still referenced; reclassify first", "references": 2 }
```

- 与 `/api/component-types` 同一套语义（`action` / `id` / `name` / 省略与空值的区分 / `name` 不得为空 / 保存失败不报 ok），
  差别在字段集合：`category` 仅封装类型会返回与落盘 —— `add` 省略时默认 `Other`；
  `update` 省略或 `null` = 不改，显式空串 = 清空；序列化时 `category` 为空则省略该字段，
  加载时缺失按空串读回（不回填 `Other`），GET 原样返回（可能是空串）
- 封装类型删除检查 DB 封装引用，不依赖元件类型匹配配置健康状态。

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

### GET /api/app-info

返回应用版本、项目主页和日志目录（顶栏“关于/检查更新”使用）。

```
发送: GET /api/app-info
返回: { "ok": true, "version": "0.1.1",
        "homepage": "https://github.com/xiaomaitangjiang/KiCad_Forge",
        "log_dir": "C:\\...\\data" }
```

### POST /api/open-log-dir

在系统文件管理器中打开当前数据目录（日志文件 `forge_YYYY-MM-DD.log` 就在其中）。

```
发送: POST /api/open-log-dir  (无 body)
返回: { "ok": true }
// 无法打开:
返回: { "ok": false, "error": "Could not open the log directory" }
```

### POST /api/open-url

用系统默认浏览器打开项目链接。仅接受项目主页及其子路径，其他 URL 返回 400。

```
发送: { "url": "https://github.com/xiaomaitangjiang/KiCad_Forge/releases" }
返回: { "ok": true }
// 非白名单:
返回: 400 { "ok": false, "error": "URL is not allowed" }
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
| `api.classify()` | `POST /api/classify` |
| `api.check()` | `POST /api/check` |
| `api.autoMatch()` | `POST /api/automatch` |
| `api.settings()` | `GET /api/config` |
| `api.saveSettings(s)` | `POST /api/config {key:val, ...}` |
| `api.rules()` | `GET /api/rules` |
| `api.compTypes()` | `GET /api/component-types` |
| `api.pkgTypes()` | `GET /api/package-types` |
| `api.manageType(url, action, name, extra)` | `POST url {action, name, ...}` |
| `api.resetDb()` | `POST /api/db/reset` |
| `api.pickFolder()` | `POST /api/pick-folder` |
| `api.appInfo()` | `GET /api/app-info` |
| `api.openLogDir()` | `POST /api/open-log-dir` |
| `api.openUrl(url)` | `POST /api/open-url {url}` |
| `api.importDir(dir)` | `POST /api/import?dir=` |
| `api.compLibraries()` | `GET /api/component-libraries` |
| `api.componentSymbolLibraries(id)` | `GET /api/component-libraries/{id}/symbol-libraries` |
| `api.saveCompLibrary(body)` | `POST /api/component-libraries {action, ...}` |

---

## 插件与库路径边界验收记录（2026-10-18）

本节记录 2026-10-18 插件与库路径边界修复在隔离副本和空数据便携包上的最终验收结果。数字来自本轮新构建与重新执行的完整集合，不与前轮定向结果或历史定向用例混用。

### 构建与静态检查

| 验收项 | 命令/方式 | 结果 |
|---|---|---|
| 前端锁与依赖许可 | `node scripts/frontend_lock.mjs --verify` | 通过 |
| 前端 lint | `npm run lint --prefix webui` | 0 错误；14 条警告 |
| SDK 与插件构建 | `node scripts/build_plugins.mjs` | SDK、3 个示例、Git/LCSC 前端全部构建成功 |
| 宿主前端构建 | `npm run build --prefix webui` | 成功 |
| 公共 SDK 类型夹具 | `node webui/node_modules/typescript/bin/tsc -p tests/types/tsconfig.json` | 通过 |
| 示例与官方插件类型 | 5 个目录 `npm run typecheck` | 全部通过 |
| Python 语法 | `python -m compileall -q plugins` | 通过 |
| Python 插件与构建助手 | `python -B -m unittest tests.python.test_plugins tests.python.test_lcsc_staging tests.python.test_lcsc_workflow tests.python.test_installer tests.python.test_runtime_assets -v` | 72 用例通过，3 个符号链接分支因本机权限跳过 |

### 测试

- `xmake build -y -j2`：通过，24.265 秒。
- `xmake test -v`：9 个已注册测试可执行文件全部通过。
- `test_data_integrity.exe --no-skip`：299/299 用例、19066/19066 断言通过，0 skipped。
- `node --test webui/tests/unit/*.test.js webui/tests/unit/*.test.mjs`：235/235 通过，0 失败、0 跳过。
- 真实 Edge 交互：LCSC 导入 70/70 逻辑场景、22/22 原生点击场景（1280x900、960x640）；类型页 72/72；类型匹配 44/44；插件原型页通过。

### 空数据便携包与 E2E

- `python scripts/package.py --binary build/mingw/x86_64/debug/KiCad_Forge.exe --name plugin-boundaries-windows-debug --output build/fix-check-dist` 生成 19,149,785 字节 zip，包内 `data/` 为空。
- `python scripts/smoke_test.py --binary build/fix-check-dist/plugin-boundaries-windows-debug/KiCad_Forge.exe`：通过；覆盖迁移后启动、组合筛选、单符号/绑定读取、路径边界（目录与单个 `.kicad_sym`、模型源必须为目录、拒绝后注册不变）、前端资源、插件加载、数据库与干净退出。
- 设置 `KFORGE_BUILD_ROOT` 指向该包、`KFORGE_STATE_E2E=1`、`KFORGE_GIT_WRITE_E2E=1`、`KFORGE_IMPORTS_E2E=1` 后，依次运行 `run-plugin-webview2.mjs`、`run-plugin-state-e2e.mjs`、`run-git-write-e2e.mjs`、`run-import-contract-e2e.mjs`：四项均 PASS。导入 E2E 覆盖新增的公共查询组合筛选、单符号 404/null 语义、单文件 Unicode 大写扩展名符号根等断言。

### 限制与未覆盖

- 本机创建文件/目录符号链接权限不足（WinError 1314）：Python 3 个符号链接分支跳过，`test_plugin_manager` 2 个用例跳过；`test_data_integrity --no-skip` 中的符号链接子分支输出 MESSAGE 而未实际执行。不能把 0 skipped 理解为这些分支已覆盖。
- 远程 GitHub Actions 未运行（本轮未推送）；桌面原生窗口与文件夹对话框仍需人工验收。
- 本轮未新增后台线程、轮询器或第三方依赖；测试结束后应用与测试进程均已退出。
