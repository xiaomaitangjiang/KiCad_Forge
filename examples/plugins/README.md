# Example Plugins

三个示例位于本目录，只用于验收/演示，不随产品启用：

| 目录 | 作用 |
| --- | --- |
| `demo_supplier` | 前端服务提供者：`ctx.contracts.provide('demo.supplier','1.0.0', service)`，同时 `ctx.provide` 给 Cordis inject |
| `demo_supply_page` | 视图提供者：注册 `demo.supply` 视图，声明子插槽 `demo.supply.tabs`，演示 config 默认值/样式 |
| `demo_supply_tab` | 第三方扩展：`inject: ['demo.supplier']`，向 `demo.supply.tabs` 注册页签 |

## 构建

```sh
cd examples/plugins/demo_supplier
npm ci
npm run build          # 产出 dist/index.js
```

或一次构建全部：

```sh
node scripts/build_plugins.mjs
```

宿主预构建会独立判断 SDK/示例/首方前端是否过期；插件源码不是 `webui/dist` 的输入。

## 验收时加载

把插件目录复制到构建输出插件目录（不要提交这些副本）：

```sh
cp -r examples/plugins/demo_* build/<plat>/<arch>/<mode>/plugins/
```

真实宿主会按 manifest 发现；阶段 1 的原型页 `/prototype/` 和阶段 5 的主窗口都可加载。
示例依赖 SDK/契约 `1.0.0`，engines 使用 `^1.0.0`。
