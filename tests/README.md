# 测试

使用 C++23 和 doctest。C++ 用例在 `tests/cpp/`，Python 用例在 `tests/python/`，共享头文件在 `tests/support/`，性能基准在 `tests/benchmarks/`，SDK 类型夹具在 `tests/types/`。目标名保持不变。平台工具链及锁定依赖准备步骤见 [CI 构建说明](../docs/development/ci.md)。

在仓库根目录用 PowerShell 执行：

```powershell
xmake build -y
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
xmake test -v
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
```

- `test_launcher`：依赖注入、约束、错误定位、失败清理、移动和析构。
- `test_service_startup`：临时 SQLite 数据库、日志先于 DB 初始化、DB 失败后跳过后续服务、未启动的导入管理器安全停止、依赖先释放而 DB 后关闭。API 使用测试桩，不启动 HTTP/WebView。
- `test_data_integrity`：JSON 持久化与失败保留、属性值精确修改、封装绑定与事务回滚、增量/强制导入、取消后重启、KF ID 写回保护。仅使用临时数据库和合成 KiCad 文件。
- `test_logger`：级别、格式化和调用点位置；退出用例时恢复原默认 logger。
- `test_plugin_manager`：每用例独立插件目录；两个依赖真实应用安装路径的 smoke 用例默认跳过，可用 `--no-skip` 显式运行。
- `test_symbol_hash`：现有符号哈希回归。
- `test_app_window`：实际编译当前平台窗口的构造和左值/右值心跳回调，运行时只构造、关闭未打开的窗口，不启动 GUI。
- `test_matching_regex_raii`：Linux/Windows 的 PCRE2 编译上下文分配失败及释放检查；macOS 不注册这项 GNU 链接器专用测试。
- `bench_import`：性能基准，不属于本轮单元测试。

派生符号回归已纳入 `test_data_integrity`，可用 `test_data_integrity.exe --test-case="DERIVED*"` 定向运行。解析用例位于 `test_symbol_inheritance.cpp`，覆盖前向多级继承、属性覆盖与显式空值、缺失/循环/重复父关系及深链；`test_library_workflows.cpp` 另验证继承字段进入真实导入、分类、封装关联与重导 KF/Pre 链。行为约束见 [IC-007/IC-008](../docs/specs/IMPORT_CORE.md#来源与身份)。

检查顺序依赖时可运行 `xmake run test_launcher --order-by=rand --rand-seed=42`，其他测试目标同样支持。

生命周期约定：

- 调用过 build（包括返回错误或抛异常）的服务需要清理；未调用 build 的服务跳过。
- 只有 destroy 的服务从 Launcher 构造起参与清理。
- 每轮启动只尝试一次 destroy；显式销毁失败后，析构继续清理尚未处理的服务，不重试失败项。
- 移动转移清理责任；移出对象析构不再关闭服务。
- 静态实例存储仍为每类型一份，不支持多个 Launcher 同时持有同类型右值服务；移动赋值测试使用引用槽避免覆盖。

P1 的文件与数据库一致性测试覆盖可捕获的 I/O、SQL 和提交失败。未覆盖进程断电恢复、外部程序同时修改源文件，也没有对文件系统和 SQLite 作跨系统原子提交保证。

插件进程参数、超时、取消和输出边界由 `test_plugin_manager` 覆盖；Python 插件、打包和前端浏览器回归命令见 [CI 构建说明](../docs/development/ci.md)。浏览器测试使用本机 Edge 和隔离夹具，不代表应用内 WebView2 与原生文件选择器已通过桌面验收。

`scripts/smoke_test.py` 在临时复制的便携包中检查真实 HTTP 启停、匹配配置恢复和 SQLite 完整性，并将数据库路径设为目录，验证启动失败严格以退出码 1 结束。它使用 `--no-auto-import`，跳过本机元件库自动扫描。

`python -B -m unittest tests.python.test_runtime_assets -v` 检查构建资源刷新：缺少源文件、复制失败或输出存在链接/目录联接时，在清理前拒绝操作，保留源码、旧输出和 `data/`。用例只在临时目录中创建资源与重定向。

`python -B -m unittest tests.python.test_repository_layout -v` 检查目录规则和本地链接检查器本身；`python -B scripts/check_repository_layout.py` 是 CI 入口，拦截生成物回流、旧迁移路径、大小写碰撞和失效的 Markdown/HTML/CSS 本地引用。AgentDocs 属于本地忽略资料，不进入公开 CI 的链接检查。

`python -B -m unittest tests.python.test_build_cleanup -v` 检查 `scripts/clean_build.py` 的预览/应用边界：`--apply` 必须显式指定类别，`all` 不能与其他类别混用，未知类别不删除；`packages`/`snapshot` 只接受 `scripts/clean_build_manifest.json` 中逐项核准且指纹匹配的条目，未知包/zip/快照保留；拒绝 build 之外、build 根、`ci-dist` 根、`mingw`、依赖及其祖先，build 根、候选根和祖先的符号链接/目录联接拒绝，候选内部链接只摘除本身。链接判断同时覆盖 POSIX `S_ISLNK` 和 Windows reparse 属性；POSIX 用真实符号链接，Windows 用 junction 模拟 POSIX stat。默认执行 `python scripts/clean_build.py` 只打印清单；`--apply --yes --categories cache` 才会删除。

`test_data_integrity --test-case="import progress*"` 检查解析、写入、跳过、回滚和取消时的内存任务进度；持有数据库会话时仍能读到快照。`node webui/tests/browser/run-lcsc-browser.mjs --import-progress` 定向运行真实导入弹窗，检查串行轮询、失败重试、阶段切换和卸载保护；这些场景也包含在完整 `--types` 检查中。

`test_data_integrity --test-case="U08*"` 检查符号/封装路径类型、单符号文件、扫描错误、封装解析取消、轻量关联查询、正确自动关系不重写和并发手动导入拒绝。`node webui/tests/browser/run-lcsc-browser.mjs --settings-import` 检查设置导入的忙态、防重复、错误恢复、路径说明以及关闭窗口后的回包；同样包含在 `--types` 中。

性能比较使用临时合成来源，不读取用户元件库：

```powershell
xmake build -y bench_import
.\build\mingw\x86_64\debug\bench_import.exe --synthetic --files 512 --pins 16
.\build\mingw\x86_64\debug\bench_import.exe --synthetic --files 2048 --pins 16
```

基准分别输出空数据库首次导入、同库未变化重导入的阶段耗时，校验符号、封装、引脚和关联数量，并记录关联阶段的插删及属性查询次数。`cold` 指空数据库，不代表清空系统文件缓存。没有绝对毫秒通过门槛；请在同一机器、相同构建和数据规模下比较。无参只打印用法，读取已安装 KiCad 的旧基准必须显式指定 `--local-kicad`。
