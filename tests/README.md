# 测试

使用 C++23、doctest 和项目现有 MinGW 工具链，无新增第三方依赖。

在仓库根目录用 PowerShell 执行：

```powershell
$env:PATH = "D:\msys64\mingw64\bin;$env:PATH"
$targets = @('test_launcher', 'test_service_startup', 'test_data_integrity', 'test_logger', 'test_plugin_manager', 'test_symbol_hash')
foreach ($target in $targets) {
    xmake build $target
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $target" }
    xmake run $target
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $target" }
}
```

- `test_launcher`：依赖注入、约束、错误定位、失败清理、移动和析构。
- `test_service_startup`：临时 SQLite 数据库、日志先于 DB 初始化、DB 失败后跳过后续服务、未启动的导入管理器安全停止、依赖先释放而 DB 后关闭。API 使用测试桩，不启动 HTTP/WebView。
- `test_data_integrity`：JSON 持久化与失败保留、属性值精确修改、封装绑定与事务回滚、增量/强制导入、取消后重启、KF ID 写回保护。仅使用临时数据库和合成 KiCad 文件。
- `test_logger`：级别、格式化和调用点位置；退出用例时恢复原默认 logger。
- `test_plugin_manager`：每用例独立插件目录；两个依赖真实应用安装路径的 smoke 用例默认跳过，可用 `--no-skip` 显式运行。
- `test_symbol_hash`：现有符号哈希回归。
- `bench_import`：性能基准，不属于本轮单元测试。

检查顺序依赖时可运行 `xmake run test_launcher --order-by=rand --rand-seed=42`，其他测试目标同样支持。

生命周期约定：

- 调用过 build（包括返回错误或抛异常）的服务需要清理；未调用 build 的服务跳过。
- 只有 destroy 的服务从 Launcher 构造起参与清理。
- 每轮启动只尝试一次 destroy；显式销毁失败后，析构继续清理尚未处理的服务，不重试失败项。
- 移动转移清理责任；移出对象析构不再关闭服务。
- 静态实例存储仍为每类型一份，不支持多个 Launcher 同时持有同类型右值服务；移动赋值测试使用引用槽避免覆盖。

P1 的文件与数据库一致性测试覆盖可捕获的 I/O、SQL 和提交失败。未覆盖进程断电恢复、外部程序同时修改源文件，也没有对文件系统和 SQLite 作跨系统原子提交保证。

插件执行参数（P2）、手动修改模型的完整文件同步和 GUI 交互尚未覆盖。
