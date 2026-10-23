# WebUI 测试

- `unit/`：Node 单元与纯函数回归，`node --test webui/tests/unit/*.test.js webui/tests/unit/*.test.mjs`。
- `browser/`：本机 Edge + Vite + CDP 的真实组件页面与 runner。
- `e2e/`：真实 WebView2 应用夹具验收，需要 `KFORGE_BUILD_ROOT` 指向空 data 的便携包。
- `support/`：应用夹具、CDP 客户端和 WebView2 CI 策略。
- `tools/`：截图等辅助工具，不属于有断言的回归。
- `fixtures/`：测试数据。

浏览器与 WebView2 脚本只操作自己创建的临时应用副本和进程，不读取开发目录的真实 `data/`。