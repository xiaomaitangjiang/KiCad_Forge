# 导入核心规格

快照日期：2026-10-19。
状态：按当前源码整理的行为基线。范围为目录扫描和已登记元件库导入；插件暂存提交另见 [导入协议](IMPORT_PROTOCOL.md)。用例映射不代替每次变更的运行结果；本次派生符号修复的定向红绿日志位于 本地临时目录 `../../.tmp/derived-symbol-20261019/`（未随仓库提交，不作为可下载证据）。

## 入口和提交范围

| 入口 | 当前行为 | 边界 |
| --- | --- | --- |
| 启动自动导入 | `ImportManager.start_async()` → `ImportOrchestrator` | 后台处理已启用的元件库包 |
| 已登记库手动导入 | `POST /api/component-libraries`，`action: import` | 通过管理器导入；强制重导忽略本次 mtime 快照 |
| 指定目录 | `POST /api/import?dir=...` | HTTP 层要求目录；只装配符号导入步骤，不自动导入旁边的封装和 3D |
| 进度读取 | `GET /api/import-status` | 读取 orchestrator 内存状态；不能假定覆盖每个独立 pipeline 或插件任务 |

实现：[ImportManager](../../src/interface/api/import_manager.cpp)、[ImportOrchestrator](../../src/interface/api/import_orchestrator.cpp)、[HTTP 路由](../../src/interface/api/routes.cpp)。

## 来源与身份

| 编号 | 当前规则 |
| --- | --- |
| IC-001 | 已登记库导入以 `find_enabled()` 返回的包为来源；读取失败必须报告错误，不伪装成空库。头文件中“legacy settings fallback”注释不代表当前执行路径。 |
| IC-002 | 符号来源可为目录或一个 `.kicad_sym` 普通文件；扩展名按 ASCII 大小写无关匹配。直接配置单文件时只导入该文件。指定目录 HTTP 入口仍只接受目录。 |
| IC-003 | 封装来源必须为目录，可为 `.pretty` 或其父目录；`.kicad_sym`、单个 `.kicad_mod` 文件不能填入封装路径。3D 来源也必须为目录。 |
| IC-004 | 配置保存可以允许尚待创建的路径；实际导入要求来源存在。权限、扫描或文件消失错误必须报告，不能记为正常 skipped。 |
| IC-005 | 同名不同来源的符号库、封装保持独立身份；同一来源重导保持既有 UUID，并更新来源元数据、属性与引脚。包归属冲突不得靠重命名或覆盖另一包解决。 |
| IC-006 | 目录归属按规范路径组件检查；不得把 `libs2` 判为 `libs` 的子目录。来源校验不能只依赖前端传入的包名称。 |
| IC-007 | `.kicad_sym` 的 extends 在当前文件内解析，支持前向引用与多级继承，不依赖声明顺序。继承引脚、Reference、自定义属性以及 Value/Footprint/Datasheet/Description/MPN；子符号自有字段优先，显式空值覆盖父值。缺失父符号、循环及重复顶层名称返回整库 ParseError，不能将不完整解析结果作为成功导入。 |
| IC-008 | 子符号保持自己的名字、UUID 与 KF/Pre 身份；修改父符号后重导，子符号 UUID 保持，继承后的封装引用参与其自身 KF 指纹与关联更新。Reference 前缀在真实属性继承完成后统一生成；缺失时的名称兜底只作用当前符号，不传播给后代，也不伪造成真实属性。继承链使用迭代遍历及临时 presence 标记，不新增缓存或业务字段。 |

实现：[路径角色校验](../../src/interface/service/import/source_path.h)、[来源键](../../src/core/db/source_key.h)、[仓储](../../src/core/repo/repositories.cpp)、[pipeline](../../src/interface/service/import/pipeline.cpp)。

继承格式依据 [KiCad 官方符号定义](https://dev-docs.kicad.org/en/file-formats/sexpr-intro/#_symbols)。本规则针对完整库文件的解析和常规导入；暂存协议仍要求一个自包含符号，不承诺接收依赖目标库父符号的单节点产物。

## 执行与增量

| 编号 | 当前规则 |
| --- | --- |
| IC-010 | pipeline 在写入前核对各步骤的路径角色，并为需要匹配的操作领取已保存匹配配置的健康快照；健康门失败不得消费 mtime 快照或开始该操作的新写入。 |
| IC-011 | 未变化来源可跳过解析；`imported_files` 使用秒级 mtime。相同秒内变化可能无法由 mtime 区分，不能承诺内容变化都被自动检测。强制重导用于本次忽略快照，不删除全局来源快照表。 |
| IC-012 | 符号库的行、属性、引脚和来源快照服从其数据库事务；写入或 COMMIT 失败不得留下该事务的部分行，也不得发布成功计数和内存快照。 |
| IC-013 | 封装批及关联修正有各自提交边界。一次完整导入可以已经提交前面的文件，再在后续来源、关联或模型阶段失败；不承诺整个目录、全部包一次成功或一次回滚。 |
| IC-014 | 取消阻止后续工作及未提交批的发布；此前已经提交的内容保留。不得把 stop/cancel 描述成用户撤销整个导入。 |
| IC-015 | 自动关联只在引用可唯一解析时建立；同名歧义不自动选择，手工关系保留其规则边界。未变化且正确的关系不得无理由删除重写。 |
| IC-016 | 同来源更新保留既有已指定类型及未知 type ID；新增匹配使用本次健康快照。保存规则和对现有数据应用规则是两个操作。 |
| IC-017 | KF ID 源文件回写是可选能力；锁定、保护及官方源文件不得改写。符号数据库导入和后续 KF ID mutation 分开执行，不能把它们描述成一个全局事务。 |

实现：[pipeline](../../src/interface/service/import/pipeline.cpp)、[源文件 mutation](../../src/core/io/source_mutation.h)、[类型匹配仓储](../../src/classifier/type_matching.h)。

## 进度、报告和并发

| 编号 | 当前规则 |
| --- | --- |
| IC-020 | `progress.completed/total` 表示当前阶段处理的来源文件数；失败和跳过也可以推进进度。阶段或来源变化后可重置，`total:0` 不能当成百分比进度。 |
| IC-021 | 阶段包括 scanning、symbols_parse、symbols_import、footprints_parse、footprints、models、linking；终态为 done、failed、cancelled。进度快照读取不申请数据库 session。 |
| IC-022 | 旧 `symbols` / HTTP `imported_symbols` 计数表示已提交的符号库文件数，不能显示为导入元件条目数。发生部分失败时保留已提交计数。 |
| IC-023 | `error_count`、`warning_count`、`failed_files` 分别表示错误事件、警告事件及失败来源数；明细分别最多保留 100 条，不能以明细长度代替完整计数。 |
| IC-024 | `Result.ok()` 只判断 `error_count == 0`，不单独表达取消。最终状态必须结合取消标志与进度终态，不能仅凭 ok 声称“全部导入完成”。 |
| IC-025 | 同一 orchestrator 的手动导入使用 try-lock，正在执行另一个手动导入时返回忙态错误，不等待数据库锁；重复 start 不新增同类后台导入。直接目录入口不经过这一相同执行闸门，不能扩展成全系统只允许一个导入。 |
| IC-026 | 符号解析并发与封装解析 worker 数按 CPU 数及文件数约束，上限 16；不为每个来源无界创建线程。数据库发布仍使用现有 session/事务。 |
| IC-027 | 已提交数据发生变化时，即使整次结果包含错误，也应推进相应导入完成序号/数据版本，供后续读取刷新；这不意味着错误可被忽略。 |

实现：[报告结构](../../src/interface/service/import/pipeline.h)、[执行控制](../../src/interface/api/import_orchestrator.cpp)、[HTTP 报告映射](../../src/interface/api/routes.cpp)。

## 验收场景

下表为已有用例映射；实际执行范围和通过数以对应变更的日志为准。

| 场景 | 对应规则 | 用例位置 / 检查点 |
| --- | --- | --- |
| 单符号文件、大写后缀、Unicode 路径 | IC-002 | [test_import_contract.cpp](../../tests/cpp/test_import_contract.cpp)，`U08PATH`、`U08EDGE` |
| 封装路径误填符号文件，3D 来源为文件 | IC-003、010 | 同上；错误发生前文件与数据库均无新写入 |
| 来源扫描消失、解析失败及同 mtime 修复重试 | IC-004、011 | 同上 `U08EDGE`；失败不消费成功快照 |
| 同名不同来源、同来源多包归属冲突 | IC-005、006 | [test_library_workflows.cpp](../../tests/cpp/test_library_workflows.cpp)，`same-name`、`ownership conflict`、`FIX03` |
| 前向/多级继承、显式空值、6种声明顺序、2048级链、无效父关系 | IC-007 | [test_symbol_inheritance.cpp](../../tests/cpp/test_symbol_inheritance.cpp)，`DERIVED` 解析用例 |
| 父封装修改后子符号重导、独立身份、封装关联更新 | IC-008 | [test_library_workflows.cpp](../../tests/cpp/test_library_workflows.cpp)，`DERIVED import and reimport` |
| 更新或 snapshot 写入失败、COMMIT 失败 | IC-012、013 | 同上 `FIX01`；检查行、快照和已提交计数 |
| 关联歧义与手工关系保留 | IC-015 | 同上 `FIX01 ambiguous duplicates`、`FIX03 legacy footprint references` |
| 取消后重新启动、强制重导、KF ID 保护 | IC-011、014、017 | 同上 `manual force import`、`cancelled pipeline`、`KF ID opt-in` |
| 数据库会话被占用时仍读取进度 | IC-020、021 | 同上 `import progress` 用例组 |
| 并发手动导入拒绝 | IC-025 | [test_import_contract.cpp](../../tests/cpp/test_import_contract.cpp)，`U08ORCH concurrent manual import` |
| 性能回归 | IC-026 | [bench_import.cpp](../../tests/benchmarks/bench_import.cpp)；同机器、同构建和同合成数据比较首次/未变化重导，不设未经测量的绝对毫秒门槛 |

## 不作保证的范围

秒级 mtime 不是内容校验和；外部进程改文件仍需在写入时复核。数据库和文件的 mutation 恢复不等于用户可撤销的历史。大规模导入、硬链接别名、跨进程并发和真实原生文件选择器需要独立验收，不能从小型离线夹具推导。
