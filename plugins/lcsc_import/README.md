# LCSC Import Plugin

从立创（LCSC / easyeda2kicad）按编号取符号与封装，导入到宿主管理的**元件库包**
（符号库 + 封装库）。插件只负责下载、转换与暂存产物；目标文件、封装落点、
数据库登记全部由宿主完成。

## 流程

1. **analyze**（不需要目标库）：`{"lcsc_id":"C37593"}`
   - 规范化编号 → 下载并转换到独立临时目录（不复用缓存，失败不回传上次结果）→ 删除临时目录；
   - 返回 `lcsc_id`、`symbol_names`、`footprint_name`、`model_name` 与 `candidates`：
     宿主封装搜索 API 的**完整**结果（`id`/`name`/`library`/`score`/`pad_count`），
     同名不同 UUID 全部保留，不在这里做筛选或排序；
   - 查询不可用/失败时返回 `warning`，`candidates` 为空 —— 明确区分「查询失败」与
     「确定没有匹配」，用户仍可选择新建封装继续。
2. **用户选择**：先选元件库包（`target_component_library_id`），再选具体 `.kicad_sym`
   符号子库，最后从自动匹配候选中选一个封装或新建封装。符号子库写法是
   `"symbol_library_choice":{"mode":"existing","path":"nested/Device.kicad_sym"}`，
   或 `"symbol_library_choice":{"mode":"new","name":"Switches"}`；
   封装复用写法是
   （复用 `{"mode":"reuse","footprint_id":"<UUID>"}`，`footprint_choice.id` 与顶层
   `footprint_id` 是等价写法）或新建（`{"mode":"new"}`，不允许携带旧 UUID；
   三处 UUID 同时给出且不一致的请求会被宿主拒绝）。
3. **import**（协议 `staged-import-v1`）：宿主给出私有暂存目录，插件把符号与封装
   写进去并产出 `manifest.json`，再由宿主校验、落盘、登记数据库。

## 界面流程

客户端通过 `sidebar.panel` 在侧栏内直接渲染 `plugins/lcsc_import/frontend/src/LcscImportModal.jsx`（inline 模式，不再弹窗）；
宿主不再按 `action.template` 分派。`analyze`/`import` 经插件适配器转成 `ctx.tasks` 的
`TaskHandle`；`frontend/src/model.js` 管理查找、选择、提交、任务与刷新状态，视图只订阅快照和调用操作。
模型订阅任务的 `getSnapshot/subscribe` 显示 `state/phase/cancelable`，staging 期间
可请求取消，进入 committing 后取消由后端拒绝。普通面板切换只退订视图，不撤销已提交导入；
插件停用时释放模型和适配器。刷新失败保留已提交意图，只允许重试刷新。
`analyze` 的组件行为由 `webui/tests/browser/lcsc-browser.jsx` 直接挂载插件 modal 验证。
`analyze` 是内部步骤，不会作为可点动作出现在插件面板里。

1. **只输编号并查找**：目标元件库、候选、导入在此之前都不出现，也不能操作。
   支持 Enter；空编号、格式错误的编号立即给提示且不发请求。编号规范化与插件
   一致（去首尾空白、大写、纯数字补 `C`），同一个未完成的查找不会被重复触发。
2. **选择目标元件库**：列表来自 `/api/component-libraries`，每项显示包名与
   符号/封装路径。锁定、受保护、停用、缺符号或封装路径的包不可选并显示原因；
   空的新包可以选（不依赖「是否已使用」的过滤）。没有可用的包时提示到
   「设置 → 库路径」里配置。不预选任何包（包括 activeWorkspace 与第一个包）。
3. **选择符号子库**：列表来自 `/api/component-libraries/{id}/symbol-libraries`，
   按符号根目录下的真实 `.kicad_sym` 文件列出，包含空文件和尚未登记到数据库的文件。
   显示目录相对路径，避免把不同目录中的同名文件混为一项；没有任何默认选择。
   可选已有文件，或输入新子库名（如 `Switches`；界面接受一次 `.kicad_sym` 后缀并移除）。
   新建文件只在最终导入提交时创建，选择和预览不创建文件；新建不覆盖同名文件。
   包的符号路径直接配置为单个 `.kicad_sym` 文件时，只能选择该文件，不能新建子库。
4. **明确选择封装**：自动匹配的候选**全部**展示（封装名、所属库、相似度、焊盘数；
   UUID 只作为身份，不显示给用户），默认没有任何选择，也不会自动选最相似的一个；
   空候选时同样必须显式点「新建封装」。缺少 UUID 的候选项不可选。确认区显示
   「立创编号 / 器件 / 目标元件库 / 符号子库 / 复用哪个封装或新建」的摘要。
5. **导入**：发送 `target_component_library_id`、`symbol_library_choice` 与显式 `footprint_id`
   （新建则不带任何旧 UUID），并为本次确认意图生成 `operation_id`；失败后保留
   表单与同一个 `operation_id` 以便重试，编号/目标/选择变化会生成新的操作 ID。
   切换元件库或符号子库会清除封装选择，需重新确认。
   导入期间冻结输入与关闭（Escape、遮罩、关闭按钮都不生效）。
6. **提交前复核**：先重新拉取元件库列表，包被删除/锁定/保护/缺路径时阻断并说明；
   包的名称或路径与已显示的摘要不一致时停下、更新显示并要求用户重新确认。
   查找阶段的旧响应、旧 `finally`、关闭后返回的响应都不会覆盖新状态。
7. **成功后**：刷新符号、库列表与统计，刷新完成后才关闭；写入已成功但刷新失败时
   弹窗保持打开，只提供「重试刷新」，不会重复执行导入。

`symbol_library_choice` 仅适用于元件库包模式。旧客户端省略该字段时，宿主仍按旧规则
写入目录下的 `LCSC.kicad_sym`，或写入包配置指定的单个符号库文件；新界面必须明确选择。
已有子库沿用其 UUID 和来源路径身份，宿主通过同一套文件变更事务写入。成功的新建请求
用相同 `operation_id` 重试时先返回历史结果，不会因为文件已创建而报重名。

## 编号规范化

`normalize_lcsc_id`：去首尾空白、统一大写、纯数字补 `C`；只接受 `C` + 数字。
空字符串、路径字符（`/ \ :`）、混杂编号（`C12A3`）、带扩展名等一律 `ValueError`，
并且在校验通过之前不会构造任何路径、不会发起任何下载。

宿主侧同规则、同时机：元件库包模式下，宿主在算来源键/幂等摘要之前就把请求编号
规范化成同一个 `C` + 数字形式再交给插件，所以 `" c123 "`、`"C123"`、`"123"`
落在同一条来源上（同 operation 重放、同源重导 no-op），非法编号根本不会执行插件；
清单里的 `lcsc_id` 必须与宿主规范化后的编号一致。其它第三方插件的外部编号不受影响。

## 暂存目录产物

| 文件 | 说明 |
| --- | --- |
| `symbol-N.kicad_sym` | 固定编号的安全文件名，上游元件名只写在 manifest 与文件内容里 |
| `footprint-0.kicad_mod` | 仅新建模式；根节点规范为 `(footprint "封装名" ...)`，其余内容原样保留 |
| `manifest.json` | `protocol` / `lcsc_id` / `symbols` / `footprint` / `footprint_choice` |

复用模式不写封装文件：转换器产生的 `.kicad_mod` 与 `.pretty` 中间产物会被清掉，
最终关联由宿主按候选 UUID 建立（不复制既有封装）。

## CLI

```
python plugin.py analyze '{"lcsc_id":"C37593"}'
python plugin.py import  '{"source":"C37593","staging_dir":"<dir>","protocol":"staged-import-v1",
                           "footprint_choice":{"mode":"new"}}'
python plugin.py info
```

**已移除**：旧的 `import` 回退分支（插件在缺 `staging_dir`/`protocol` 时自行把符号
merge 进任意 `target_library` 路径的 .kicad_sym、并顺手复制封装的旧流程）不再存在 ——
UI 操作不再由插件直接改用户库文件。缺 `staging_dir` 或协议不符时直接返回
`Missing staged-import-v1 staging_dir` 并退出码 1。`info` 与下载/转换（`prepare`）不变。

## 测试

```
python -B -m unittest tests.python.test_lcsc_staging tests.python.test_lcsc_workflow
```

不联网：测试用桩替换 `prepare` 与宿主搜索 API。
