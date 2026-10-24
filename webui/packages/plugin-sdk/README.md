# KiCad Forge Plugin SDK

`@kiforge/plugin-sdk` 是前端插件的公共接口包，当前公开稳定版本 **1.1.0**（契约 `1.0.0`）。

## 公共接口

- **七契约**：`symbols`、`libraries`、`footprints`、`imports`、`tasks`、`commands`、`workspace`。
  - `ctx.tasks.run(action,args)` 的插件身份由宿主注入，不能传 `pluginId`；返回 `TaskHandle`
    （快照 + `cancel()`）。
  - `ctx.workspace.getSnapshot()` 只读：`workspaceId/selectedSymbolId/libraryFilterId`；不提供切换。
  - `ctx.commands.run` 只接受 allowlist：`refreshSymbols|refreshLibraries|refreshStatus|refreshAll`。
- **七插槽**：`sidebar.section`（工作空间内嵌入分区）、`sidebar.panel`（活动栏独占面板）、
  `toolbar.action`、`card.accessory`、`inspector.tab`（keyed）、`contextmenu.item`、`settings.section`；
  每个插槽有独立 owner props 类型。`sidebar.panel` 要求完整 `counts`，旧 `sidebar.section` 的
  `counts` 保持可选。
- **视图**：`ctx.views.register({ id,title,icon?,order?,render,slots? })`；一次只激活一个。
- **任务与导入**：`ctx.tasks`/`ctx.imports` 由宿主按当前插件身份注入；它们不是可由
  `frontend.requires` 声明的服务契约，禁止把 `kiforge.tasks`/`kiforge.imports` 写进 requires
  （loader 会报 `contract/per-plugin-only`）。跨插件导入通过 `imports` 的 `provider` 字段路由。
- **配置**：`ctx.config.get/set/subscribe`；manifest `config.schema`；64 KiB 上限。
- **诊断/模型**：`ctx.diagnostics.report()`；`ctx.model.createStore(initial)` 复用宿主快照实现，
  提供 `getSnapshot/subscribe/set/update/dispose`；`ctx.model.useSnapshot(store)` 负责 React 订阅。
  快照引用在发布前保持稳定；相同引用不通知，订阅释放及 dispose 可重复执行。
  插件停用自动释放所属 store；业务模型可更早主动 dispose。社区模型无需导入宿主内部文件，
  也无需自行实现 store、增加轮询或依赖。
  独立测试和自建视图入口可从 SDK 导入 `createSnapshotStore(initial, { freeze?: boolean })`，
  复用相同实现并自行在生命周期结束时 dispose；`disposed` 是只读状态。`freeze` 默认 false，
  启用时只冻结快照顶层，保持已有宿主行为。
- **版本**：公开稳定版 `1.1.0`；使用 `sidebarPanel` 的首方插件声明 `engines.sdk: ^1.1.0`；
  旧插件可继续使用 `defineSlot` 的兼容路径。新代码使用 `SlotMap` 模块扩展 + `declareSlot()`，
  保留名称与 props 的关联；官方插槽还会检查固定 kind（`inspector.tab` 为 `keyed`，其它为
  `list`），省略 kind 时使用对应默认值。社区插槽可自行选择 kind，默认 `list`。

### 插槽类型检查边界

`declareSlot('sidebar.panel')` 返回保留名称字面量的 handle，注册回调与 `renderSlot` 的 props
从 `SlotMap` 推导。直接构造保留已知名称字面量的结构 handle，也会校验该名称的 kind/props。
侧栏 owner props 与其中的 `counts` 都是只读；旧 `sidebar.section` 的 `counts` 可选，
`sidebar.panel` 要求完整的 `symbols/issues/matches`。回调可以只使用 `workspaceId`，无需声明
或读取其余字段。

1.x 的 `defineSlot<P>(name: string, kind?)`、`SlotDefinition<P>` 结构 handle 和显式
`register<P>` / `renderSlot<P>` 保留宽名称兼容签名，不验证名称；通过这些入口重用官方名称、
主动将 handle 的名称扩宽成 `string`，仍可绕过名称与 props/kind 的关联。任意社区名称及
合法结构 handle 不要求额外品牌标记。编译检查不能代替运行时校验，也不承诺阻止类型断言。
`renderSlot` 继续允许省略整个 props 参数以兼容旧调用；一旦传入 props，就检查完整形状。

## 最小插件

`ctx.imports.analyze/commit` 的 `provider` 是提供暂存导入能力的插件清单 ID。
宿主读取其唯一的 `staged-import-v1` 动作与 `analyze_action`，请求仍经过完整的
宿主校验、暂存和提交；`lcsc`、`lcsc_import` 兼容别名指向首方 LCSC 插件。
插件调用时使用 SDK 的 camelCase 字段，适配器负责转换 HTTP/Python 字段。
返回的导入快照在状态未改变时保持同一个对象，可以直接用于 React 订阅。

`ctx.tasks.get()` 返回 `TaskSnapshot | null`，`list()` 返回快照数组，任务 ID 字段为
`id`。导入的候选封装 ID 为 `footprintId`，提交结果包含 `operationId/symbolId/footprintId`。
停用前端插件会释放其轮询和订阅；已在宿主提交的业务结果仍然保留。

```tsx
import * as React from 'react'
import { definePlugin, sidebarPanel } from '@kiforge/plugin-sdk'

export default definePlugin({
  name: 'my.plugin',
  apply(ctx) {
    ctx.slots.register(sidebarPanel, {
      id: 'panel',
      title: 'My panel',
      render: ({ counts }) => <div>{counts.symbols} symbols</div>,
    })
  },
})
```

## 构建

插件源码不进宿主构建。每个插件目录有自己的 `package.json`/`tsconfig.json`：

```sh
npm ci
node ../../webui/scripts/build-plugin.mjs .
```

产物为 `<plugin>/dist/index.js`（及可选 CSS）。`react`、`react-dom`、`react/jsx-runtime`、
`react-i18next`、`@kiforge/plugin-sdk` 由宿主运行时/import map 提供，不得打进插件包。

## 兼容性

- `manifestVersion` 缺省 1；v2 支持 `python`/`frontend`/`engines`/`config`/`contract` 声明。
- 旧 v1 manifest 与旧 CLI 继续兼容；旧的 `/api/plugins/execute` HTTP 入口已删除，插件调用统一走任务 API。
- 破坏性变更升 SDK/契约版本并重开冻结；内部 RC 冻结后只允许加法。
