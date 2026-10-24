# Cross-platform CI and releases / 跨平台构建与发布

## 自动检查

`.github/workflows/ci.yml` 在推送到 `master`、提交或更新 PR 时运行，也可在 Actions 页面手动运行。

当前配置为 **4 平台 × Debug/Release，共 8 组原生构建**；工作流配置存在不代表远程作业已执行。2026-10-18 续作尚无远程执行记录。历史本地证据与缺口保存在 `docs/AgentDocs/PLUGIN_SYSTEM_CONTINUATION_2026-10-18.md`，该目录不随仓库提交；远程作业结果需以实际 Actions 记录为准。

| 平台 | Runner | 编译器 | 模式 |
| --- | --- | --- | --- |
| Linux x64 | Ubuntu 24.04 | GCC 14 | Debug、Release |
| Windows x64 | Windows 2025 / MSYS2 MINGW64 | Clang / GNU C++ 标准库 | Debug、Release |
| macOS Apple Silicon | macOS 15 | Apple Clang / libc++ | Debug、Release |
| macOS Intel | macOS 15 Intel | Apple Clang / libc++ | Debug、Release |

xmake 安装步骤固定使用 3.1.1，关闭 setup-xmake 内置的包缓存。该缓存会在显式
`xmake f` 之前执行 `xmake repo --update` 和 `xmake l utils.ci.packageskey`，提前加载项目；
任务结束时还会重新计算一次键。平台、架构、模式及 `portable_deps` 尚未配置时计算的依赖
可能与后续构建不同。工具安装完成后，按下述步骤准备锁定依赖，再显式配置和编译。

若旧工作流在 `utils.ci.packageskey` 处以 255（Windows 为 4294967295）退出，
运行 `xmake l -c 'import("core.project.project"); print(project.requires_str())'` 查看项目加载错误；
缓存辅助模块会静默日志，不能据此认定是下载或缓存服务故障。

每个环境均构建 React/Vite 前端、C++ 应用，检查全部 Python 插件语法，并显式运行以下回归：

- C++：`test_logger`、`test_plugin_manager`、`test_task_service`、`test_launcher`（含 lifecycle）、`test_symbol_hash`、`test_app_window`、`test_service_startup`、`test_data_integrity`。窗口回归实际编译平台窗口与两种心跳回调调用，运行时只检查未打开的窗口；服务启动与数据完整性测试覆盖普通正则匹配和元件库工作流。
- Linux/Windows 另运行 `test_matching_regex_raii`，用 GNU `--wrap` 注入 PCRE2 编译上下文分配失败。Apple ld 不支持该选项，macOS 不注册这一项，不能据此声称 macOS 跑过同一故障注入测试。
- Python：`tests.python.test_plugins`、`tests.python.test_lcsc_staging`、`tests.python.test_lcsc_workflow`、`tests.python.test_installer`、`tests.python.test_runtime_assets`、`tests.python.test_repository_layout`、`tests.python.test_build_cleanup`。其中两份 LCSC 测试有 22 项离线 staged 协议用例，安装包输入及参数转发有 17 项；资源刷新测试检查缺少源文件、复制失败与路径重定向时保留原输出和源码。布局检查器自身用临时目录验证禁止路径、大小写碰撞和本地链接判断；清理测试验证显式类别、未知包保留、清单指纹和联接边界，并覆盖 POSIX `S_ISLNK` 与 Windows reparse 判断。总数以 unittest 输出为准。Windows 无文件符号链接权限时会跳过对应的一项。旧的 `fetch_and_merge`/插件直接写目标库协议已移除。
- 目录与本地链接：`python -B scripts/check_repository_layout.py` 检查已跟踪的 `__pycache__`、`.pyc`、编译数据库、构建输出回流，以及旧迁移路径、大小写碰撞和 Markdown/HTML/CSS 本地引用。脚本只用标准库和 `git ls-files`；`docs/AgentDocs` 是被忽略的本地资料，不纳入公开 CI 链接检查。
- Node：独立的 Ubuntu `frontend` 作业运行 `node --test webui/tests/unit/*.test.js webui/tests/unit/*.test.mjs`；四个平台的 Debug 作业也运行同一组测试，以覆盖平台差异。先生成 SDK 和插件产物，再运行读取产物的测试，不能依赖开发目录残留的 `dist`。
- Windows Debug：现有 Edge + CDP 浏览器脚本（`webui/tests/browser/`）默认流程、`--types` 及 `--type-matching`，实际运行 LCSC、设置导入、元件类型和匹配规则组件交互，并运行插件原型夹具。不额外下载浏览器或引入测试框架。
随后执行全部单元测试，把应用打包并复制到带空格的临时路径，以 `--headless --no-auto-import` 启动，跳过本机 KiCad 库的自动扫描，检查：

- `/api/status`、`/api/libraries`、`/api/plugins`。
- 空符号库返回、实际符号子库枚举，以及误把 `.kicad_sym` 填入封装路径时拒绝保存并说明字段。
- 匹配规则的读取、样本预览、保存、版本过期拒绝和空数据库应用。
- 临时副本中损坏的规则配置不能覆盖最后有效快照，不能继续保存或应用；恢复文件并重新加载后可用。
- 打包的前端 HTML 和静态资源可以通过 HTTP 读取。
- 打包的插件被正确发现，且 C++ 主程序可以通过子进程实际调用两个 Python 插件的 `info` 入口。
- SQLite 初始化、核心表和数据库完整性。
- 数据库路径不可用时，启动失败以退出码 1 结束，未启动的 HTTP 服务不会在日志服务销毁后再次写日志或崩溃。
- `/api/bye` 能让应用以退出码 0 正常退出。

Windows 冒烟测试会移除 PATH 中的构建工具链目录，避免遗漏的运行时 DLL 被 MSYS2 掩盖。
失败日志保留 7 天，通过检查的 Release ZIP 保留 14 天，均可在 Actions 运行页面下载。

Edge 组件交互检查与应用内 WebView2 是两个环境。Windows Debug 在打包后另运行真实 WebView2 插件加载、状态跨重启和 Git 写按钮测试；这些脚本只启动自己的临时应用副本。原生文件夹选择器仍需人工桌面验收。
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

需要 xmake 3.0.9+（CI 固定 3.1.1）、Node.js 24、Python 3.12，以及支持 C++23 显式对象参数及 `std::forward_like` 的编译器。Linux/macOS 另需已安装的 CMake，供原生 PCRE2 静态库构建使用。

PCRE2 固定为 10.47、8 位静态库，应用不调用 JIT。仓库保存官方源码归档及 MSYS2 `10.47-1` 包，SHA256、许可和静态 SDK 文件校验记录在 `dependencies/pcre2.lock.json`：

- Windows 开发构建默认保留 `MSYS2_DIR` 指向的已验证 SDK；缺省兼容原路径 `D:/msys64/mingw64`。CI 从仓库内锁定包提取到 `build/dependencies/`，通过 `PCRE2_SDK_DIR` 指定，不依赖当前 pacman 镜像中的 PCRE2 版本。
- Linux/macOS 的 `scripts/prepare_dependencies.py` 先核对源码归档，再调用该版本自带的官方 CMakeLists.txt，关闭共享库、16/32 位库、JIT、grep 和上游测试。生成 SDK 位于平台/架构/模式独立目录，并记录输出文件校验和。准备脚本不联网、不安装用户环境。
- SDK 头文件版本、归档、许可及 Windows SDK 三个原文件均严格校验；缺失或不符直接失败，不使用系统 PCRE2 代替。

其余依赖仍使用官方 xmake 包配方；固定快照来自 [xmake-repo 01dcc6e](https://github.com/xmake-io/xmake-repo/tree/01dcc6e2521785b4fee3a987008fe9b715fb51d5)。`dependencies/xmake-recipes.lock.json` 保存准确 commit、配方/补丁 SHA256 和所选源码 SHA256。五个配方移除了 Git tag 下载回退；其中 toml++ 的 header-only 分支和 fast_float 直接用 xmake 安装完整公开 include 目录。上游两者均为 INTERFACE 库，头文件不需生成；这些分支不再引入 CMake/Ninja/make，原配置、宏及需编译的库分支保持原行为。锁同时记录上游原文 hash、适配说明及适配后 hash；归档校验失败直接失败，不能退回可变 tag。预编译包下载关闭。选用 spdlog 1.17.0、nlohmann/json 3.12.0、toml++ 3.4.0、fast_float 8.3.0、SQLite 3.53.4、doctest 2.5.2。
CI 在明确的依赖准备/配置步骤下载并校验这些固定源码，随后编译/测试复用已准备的包。使用 `--portable_deps=y`，拒绝以任意系统库替代；CMake 仅供原生 PCRE2 的官方构建使用，不由 xmake 下载；纯头文件依赖不需要构建生成器。`package.requires_lock` 同时保留各环境生成的解析记录，上传到诊断产物；这里未伪造尚未实际配置的其它平台 lock。

```sh
cd webui
npm ci
npm run build
cd ..

# Linux：安装 GCC 14 和 CMake 后；先离线准备 PCRE2，再配置其它锁定依赖
python scripts/prepare_dependencies.py --plat linux --arch x86_64 --mode debug --cc gcc-14
xmake f -y -c -p linux -a x86_64 -m debug --portable_deps=y --cc=gcc-14 --cxx=g++-14 --ld=g++-14 --sh=g++-14
# macOS：Apple Silicon 示例，Intel 用 x86_64
# python scripts/prepare_dependencies.py --plat macosx --arch arm64 --mode debug --cc clang
# xmake f -y -c -p macosx -a arm64 -m debug --portable_deps=y

xmake build -y
xmake test -v
```

Windows 在 MSYS2 MINGW64 环境安装 `mingw-w64-x86_64-clang`、`mingw-w64-x86_64-lld`、
`mingw-w64-x86_64-gcc`，另安装标准 Python 后配置。Windows 使用锁定 PCRE2 SDK 包，纯头文件依赖不需要 CMake/Ninja/make。复现 CI 的依赖方式：

```sh
export MSYS2_DIR="$(cygpath -m "$MINGW_PREFIX")"
python scripts/prepare_dependencies.py --plat mingw --arch x86_64 --mode debug
export PCRE2_SDK_DIR="$(cygpath -m "$PWD")/build/dependencies/pcre2/mingw/x86_64/debug/install"
xmake f -y -c -p mingw -a x86_64 -m debug --portable_deps=y --mingw="$MSYS2_DIR"
xmake build -y
xmake test -v
```

正常构建直接使用仓库中已有的 `resources/icons/app.ico`，不安装 CairoSVG、Pillow 或 Cairo。
只有修改图标素材时才需要自行运行 `scripts/svg2ico.py`。导入 benchmark 可按需单独构建，不属于本工作流检查范围。

构建完成后通过 `scripts/sync_runtime_assets.py` 刷新前端、插件、配置种子和许可证。脚本先检查路径并暂存全部资源，再替换构建目录中的对应内容；源缺失、复制失败或路径存在重定向时停止，保留 `data/` 和其他输出文件。

以 Linux Debug 为例，可复现打包与冒烟检查：

```sh
python scripts/package.py --binary build/linux/x86_64/debug/KiCad_Forge --name KiCad_Forge-linux-x64-debug
python scripts/smoke_test.py --binary dist/KiCad_Forge-linux-x64-debug/KiCad_Forge
```

`--headless` 提供同一套本机 HTTP 服务，不打开浏览器或 WebView2 窗口。
`--no-auto-import` 跳过启动时自动导入，仍保留手动导入接口；默认启动行为不变。
主体通过 UTF-8 模式启动 Python 插件，避免 Windows 系统编码影响 JSON 通信。
仍需发送 `/api/status` 心跳维持运行，可通过 `POST /api/bye` 正常退出。
ZIP 包包含 `licenses/` 中的 PCRE2/sljit 许可原文和配置种子，`data/` 为空；不包含用户数据库、日志、插件下载缓存或 Python 字节码。Python 插件需要用户自行安装 Python 和对应的插件依赖。

## Windows 安装包

安装包使用本机已有的 Inno Setup 6.6+ 或 7（现有 `modern dynamic` 向导样式要求 6.6+）；辅助脚本不会下载或安装编译器，也不会执行生成的安装程序。
先完成 Windows 构建，再从仓库运行：

```powershell
# 默认读取 build/mingw/x86_64/release，输出到仓库的 dist/installers/。
# --dry-run 只校验并显示参数，不创建安装包或输出目录。
python scripts/build_installer.py --dry-run
python scripts/build_installer.py

# 也可使用已经打包的完整 Windows 便携目录；所有路径参数均支持空格。
python scripts/build_installer.py --source "build/ci-dist/KiCad_Forge-windows-x64-debug" --output "build/installer review" --name "KiCad Forge Debug Review"
```

`--iscc` 可指定 `ISCC.exe`；未指定时先查 PATH，再查标准 Program Files 下的 Inno Setup 7/6。
`--license`、`--icon` 分别覆盖仓库中的 LICENSE 和 resources/icons/app.ico，`--output`、`--name` 指定安装程序的目录和文件名（不含 `.exe`）。
默认路径都依据辅助脚本所在仓库解析；显式传入的相对路径依据调用时的目录解析。直接用 ISCC 编译 `.iss` 时，默认路径同样依据 `.iss` 所在位置，可通过 `/DAppSourceDir=...`、`/DAppLicenseFile=...`、`/DAppIconFile=...`、`/DAppOutputDir=...`、`/DAppOutputName=...` 覆盖。

脚本要求输入含主程序、运行 DLL、已构建前端、两个插件、三份配置种子和依赖许可；缺失时在调用编译器前失败。
输入中被打包的文件不能通过符号链接或目录联接跳到输入目录外，输出也不能位于输入目录中，文件名不能包含路径。
安装内容明确区分构建文件与运行数据：不打包 `data/`，插件排除 `fetched`、`staging`、`.git`、`__pycache__` 和 `.pyc`。
AppId、Program Files 安装目录和卸载时保留用户数据库、插件及日志的规则保持不变；实际安装、升级、卸载仍需单独的桌面验收。
离线输入校验与参数转发回归为 `python -B -m unittest tests.python.test_installer -v`；测试不会运行 Inno 安装程序。

## build 缓存与旧包清理

本机 build 目录可用标准库脚本精确清理，不引入新依赖：

```powershell
# 默认预览：列出可清理项和保留项，不删除任何文件。
python scripts/clean_build.py

# 预览指定类别（预览缺省为 all）。
python scripts/clean_build.py --categories cache,logs

# 明确应用；必须显式指定类别，--yes 跳过交互确认。
python scripts/clean_build.py --apply --yes --categories cache
```

`cache` 对应 `build/.build_cache`、`.objs`、`.deps`、`.gens`；`logs` 是 build 根下的日志；`packages` 和 `snapshot` 只处理 `scripts/clean_build_manifest.json` 中逐项核准的条目，未列入清单的包、zip 和 `commit-preparation-*` 快照只报告为 retained，不会被推断为可删。当前 build 中的交付包默认保持不动。

清单每个条目必须给出 `path`、`category`（`packages` 或 `snapshot`）、`purpose`、`data`（`none` 或 `archived`）、`recovery` 和 `tree_sha256`。可用以下命令生成指纹：

```powershell
python scripts/clean_build.py --fingerprint build/ci-dist/old-package
```

应用前会重新计算指纹；目标被替换、新增未审核文件、清单缺失或条目无效时拒绝删除。`build/ci-dist` 本身不能作为条目。`packages` 条目可指向 `ci-dist` 下的一个目录或 zip；`snapshot` 条目只能指向 `commit-preparation-*` 目录。

脚本拒绝清理 build 之外的路径、build 根、`build/ci-dist` 根、`mingw`/`dependencies` 及其祖先；build 根、候选根和候选祖先的重解析点直接拒绝，候选内部的符号链接或目录联接只摘除链接本身，不递归到目标。`--apply` 未显式提供 `--categories`、`all` 与其他类别混用、或出现未知类别时，均以非零退出且不删除任何候选。删除 cache 后重新运行 `xmake build` 会重建对象、依赖记录和 `compile_commands.json`。行为回归为 `python -B -m unittest tests.python.test_build_cleanup -v`。

## English

The workflow builds and tests Debug and Release on Linux x64, Windows x64, macOS ARM64 and macOS x64.
It runs seven C++ regression executables on all platforms, plus the GNU-linker-only PCRE2 RAII fault injection executable on Linux/Windows, the offline Python regressions (including installer input/argument checks, runtime asset staging, and build-cleanup manifest/boundary checks; file-symlink tests are skipped on Windows without the required privilege), all Node test files including matching-rule regressions, Windows Edge interaction suites, and a relocated-bundle HTTP/SQLite smoke test. The smoke test disables automatic library imports and verifies matching configuration recovery only in its temporary bundle copy.
PCRE2 10.47 archives and source checksums are committed; native static SDKs use upstream CMake. Other dependency recipes are vendored from a fixed official xmake-repo commit with source SHA256 values. Configuration explicitly prepares dependencies before the build. Linux/macOS CI has not been executed from this Windows workspace; the matrix is the verification path, not evidence that those jobs have already passed.
Desktop rendering and native dialogs are not covered by the headless test.

Pushes and pull requests never publish a release. To publish, manually run the workflow, enable
`publish_release` and enter a new stable version such as `v0.2.0`. All eight builds must pass before
the four Release ZIPs are published at the triggering commit. Existing tags are rejected.

## 插件化构建与检查（Phase 6）

当前 SDK 为 `1.1.0`、业务契约为 `1.0.0`；公共界面插槽已有七个。`sidebar.section` 用于工作空间内嵌分区，`sidebar.panel` 用于活动栏独占面板。类型门禁应同时覆盖两者及社区兼容入口；`TaskHandle.done/dispose` 是内部实现，不作为公共 SDK 类型要求。

`frontend` 作业先作为所有原生构建的门禁，按顺序执行：

1. `npm ci --prefix webui` 安装锁定依赖。
2. `node scripts/frontend_lock.mjs --verify` 校验框架版本、integrity 与许可证记录，随后运行 lint。
3. `node scripts/build_plugins.mjs` 构建 SDK、三个示例和两个首方插件，再构建宿主与共享运行时。
4. `tsc -p tests/types/tsconfig.json` 检查公共 SDK 类型正负例；五个插件目录各自按 lockfile 安装依赖并执行 `npm run typecheck`。Vite 转译不能替代源码类型检查。
5. 收集全部 `*.test.js` 和 `*.test.mjs`。`public-sdk-behavior.test.mjs` 使用公开 API 和内存夹具，验证服务等待/撤回/恢复、TaskHandle 与 imports 运行时契约；既有示例中的额外 `ctx.provide/inject` 不能代替这些检查。

前端门禁失败时不启动四平台、Debug/Release 共八组原生构建，也不能发布 Release。测试失败应修复实现，不将行为回归标为 skip、todo 或 `continue-on-error`。

Windows Debug 在打包与 headless smoke 之后，串行运行：

```sh
node webui/tests/e2e/run-plugin-webview2.mjs
KFORGE_STATE_E2E=1 node webui/tests/e2e/run-plugin-state-e2e.mjs
KFORGE_GIT_WRITE_E2E=1 node webui/tests/e2e/run-git-write-e2e.mjs
KFORGE_IMPORTS_E2E=1 node webui/tests/e2e/run-import-contract-e2e.mjs
```

工作流设置 `KFORGE_BUILD_ROOT` 指向刚打包、data 为空的应用目录，`KFORGE_TEST_RESULTS` 指向 `test-results/webview2`。脚本再创建各自的临时副本，禁止操作日常开发目录或复制用户数据库。CI 未设置 opt-in 时必须失败，不能把未执行算通过；整个集成步骤有 8 分钟上限，失败会上传 stdout/stderr 与诊断记录。

网络 LCSC 实测仍是单独的可选验收，不进入默认 PR 门禁；默认构建和离线导入测试不依赖立创服务。

公共导入集成使用临时 Python 提供者和前端消费者，验证非默认动作分派、指定符号子库、
新建与复用封装、完整暂存落库和操作重放，全部离线。WebView2 插槽用例实际渲染三层
DOM，并重复撤回/恢复父插件；不以注册计数代替显示结果。Node 门禁同时覆盖导入
快照稳定性、请求/结果字段转换和卸载时中止轮询；C++ 门禁覆盖累计短任务回收、
线程启动失败回滚、并发退出及旧同步取消语义。

六阶段复审与续作的历史记录仅保存在本地 `docs/AgentDocs/`，不随仓库提交。历史 G6 与前轮绿测均不能替代本轮源码新增门禁的实际结果；空 tsc 日志没有退出码时不能独立算通过。
