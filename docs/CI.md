# Cross-platform CI and releases / 跨平台构建与发布

## 自动检查

`.github/workflows/ci.yml` 在推送到 `master`、提交或更新 PR 时运行，也可在 Actions 页面手动运行。

| 平台 | Runner | 编译器 | 模式 |
| --- | --- | --- | --- |
| Linux x64 | Ubuntu 24.04 | GCC 14 | Debug、Release |
| Windows x64 | Windows 2025 / MSYS2 MINGW64 | Clang / GNU C++ 标准库 | Debug、Release |
| macOS Apple Silicon | macOS 15 | Apple Clang / libc++ | Debug、Release |
| macOS Intel | macOS 15 Intel | Apple Clang / libc++ | Debug、Release |

每个环境均构建 React/Vite 前端、C++ 应用、四个 doctest 测试程序以及导入 benchmark。
随后执行全部单元测试，把应用打包并复制到带空格的临时路径，以 `--headless` 启动，检查：

- `/api/status`、`/api/libraries`、`/api/plugins`。
- 打包的前端 HTML 和静态资源可以通过 HTTP 读取。
- 打包的插件被正确发现。
- SQLite 初始化、核心表和数据库完整性。
- `/api/bye` 能让应用以退出码 0 正常退出。

Windows 冒烟测试会移除 PATH 中的构建工具链目录，避免遗漏的运行时 DLL 被 MSYS2 掩盖。
失败日志保留 7 天，通过检查的 Release ZIP 保留 14 天，均可在 Actions 运行页面下载。

这些是编译、单元测试和无窗口运行检查。Windows 的 WebView2 实际渲染、交互和原生文件夹选择器仍需桌面测试；
Linux/macOS 当前使用系统浏览器，启动图形界面需要相应的桌面会话。

## 明确要求后发布

1. 工作流文件合入默认分支后，进入 **Actions → Cross-platform build and test → Run workflow**。
2. 选择要发布的分支。
3. 勾选 `publish_release`，填写尚未使用的 `release_tag`，例如 `v0.2.0`。
4. 点击运行。八组构建和测试全部通过后，自动创建 tag、生成更新说明并发布四个平台的 Release ZIP。

普通 push、PR，以及未勾选 `publish_release` 的手动运行都只执行检查，不发布。
发布使用本次运行的 commit SHA；不会在构建结束后重新选择分支上的最新提交。
已有 tag 会被拒绝，不会被覆盖。本工作流接受 `v主版本.次版本.修订号` 格式的正式版本。

## 本地构建

需要 xmake 3.1.1+、Node.js 24 和支持 C++23 显式对象参数及 `std::forward_like` 的编译器。
spdlog、nlohmann/json、toml++、SQLite 和 doctest 由 xmake 安装，版本固定在 `xmake.lua` 中。

```sh
cd webui
npm ci
npm run build
cd ..

# Linux：安装 GCC 14 后
xmake f -y -c -p linux -m debug --cc=gcc-14 --cxx=g++-14 --ld=g++-14 --sh=g++-14
# macOS：使用 Xcode 16.4+ 命令行工具
# xmake f -y -c -p macosx -m debug

xmake build -y --all
xmake test -v
```

Windows 在 MSYS2 MINGW64 环境安装 `mingw-w64-x86_64-clang`、`mingw-w64-x86_64-lld`、
`mingw-w64-x86_64-gcc` 和 `mingw-w64-x86_64-cairo`，另安装标准 Python 后配置：

```sh
python -m pip install cairosvg Pillow
export CAIROCFFI_DLL_DIRECTORIES="$(cygpath -w "$MINGW_PREFIX/bin")"
python scripts/svg2ico.py
xmake f -y -c -p mingw -a x86_64 -m debug --mingw="$(cygpath -m "$MINGW_PREFIX")"
xmake build -y --all
xmake test -v
```

以 Linux Debug 为例，可复现打包与冒烟检查：

```sh
python scripts/package.py --binary build/linux/x86_64/debug/KiCad_Forge --name KiCad_Forge-linux-x64-debug
python scripts/smoke_test.py --binary dist/KiCad_Forge-linux-x64-debug/KiCad_Forge
```

`--headless` 提供同一套本机 HTTP 服务，不打开浏览器或 WebView2 窗口。
仍需发送 `/api/status` 心跳维持运行，可通过 `POST /api/bye` 正常退出。
ZIP 包不包含用户数据库或日志；Python 插件需要用户自行安装 Python 和对应的插件依赖。

## English

The workflow builds and tests Debug and Release on Linux x64, Windows x64, macOS ARM64 and macOS x64.
It runs the four existing doctest executables and a relocated-bundle HTTP/SQLite smoke test.
Desktop rendering and native dialogs are not covered by the headless test.

Pushes and pull requests never publish a release. To publish, manually run the workflow, enable
`publish_release` and enter a new stable version such as `v0.2.0`. All eight builds must pass before
the four Release ZIPs are published at the triggering commit. Existing tags are rejected.
