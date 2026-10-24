# Git Version Control Plugin

按元件库分组的 Git 面板：状态、历史、差异、初始化、提交。v2 manifest 同时保留 Python actions 与前端 bundle。

## Python

```sh
python plugin.py status '{"target_library":"D:\\libs\\main"}'
```

- `target_library` 是元件库目录（兼容旧版 `.kicad_sym` 文件路径）；插件在该目录或所在目录查找 Git 仓库根。
- `status/history/diff` 只读；`commit/init` 写动作，首轮不可取消。
- 旧 `/api/plugins/execute` 调用路径已删除；Git 前端使用任务 API。
- `commit/init` 提交前向宿主 `POST /api/library-manifest` 取组件库语义快照，写入仓库
  `.kforge/manifest.json` 并随本次提交入库。清单含符号 UUID、name、footprint 引用、
  KF ID、引脚、属性、封装焊盘和 3D 模型 SHA-256；不把整库 `properties_json` 原样塞进仓库。
- `diff` 优先用 HEAD 的 `.kforge/manifest.json` 与当前快照按 UUID 做语义差分：
  pin/pad 增删改、属性、封装引用、3D 哈希、库/符号/封装增删。没有基线清单时回退
  原始 `git diff --stat`，不会把「无法比较」伪装成「没有变化」。
- 清单是仓库数据，不是缓存；不要在 `.gitignore` 中排除 `.kforge/`。

## 前端

- 展示源码：`frontend/src/index.tsx`；业务模型：`frontend/src/model.ts`。
  独立构建：`cd frontend && npm ci && npm run build`。
- 注册 `sidebar.panel`：元件库下拉 + 按钮；通过 `ctx.tasks.run(action,{target_library,...})`
  走统一任务服务，订阅 `TaskHandle` 的快照/取消并显示错误/结果/`uncertain`。
- manifest 声明 `frontend { root:"frontend", entry:"dist/index.js", styles:["dist/style.css"] }`；
  宿主不再渲染旧 Git Commit 按钮；v1 清单和 Python CLI 仍兼容。
- 模型归属于插件生命周期，首次打开才加载元件库；普通面板切换只退订视图，进行中的任务继续。
  快速切库合并到最后一次选择，旧只读任务可请求取消，写任务不主动取消；全程复用 SDK 的同一任务轮询。
- 构建/校验由 `scripts/build_plugins.mjs`、`xmake.lua` 的 first-party frontend 过期判定覆盖。