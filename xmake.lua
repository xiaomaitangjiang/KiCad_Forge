set_xmakever("3.0.9")
set_project("KiCad_Forge")
set_languages("cxx23")
set_defaultmode("debug")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate")
set_targetdir("build/$(plat)/$(arch)/$(mode)")

if is_host("windows") then
    set_defaultplat("mingw")
end
local msys2 = os.getenv("MSYS2_DIR") or os.getenv("MINGW_PREFIX") or "D:/msys64/mingw64"
option("portable_deps")
    set_default(not is_host("windows"))
    set_showmenu(true)
    set_description("Use the vendored, SHA256-locked official xmake dependency recipes")
option_end()

if is_plat("mingw") then
    set_toolchains("clang", {target = "x86_64-w64-windows-gnu"})
    add_cxflags("--target=x86_64-w64-windows-gnu")
    add_ldflags("-fuse-ld=lld")
    add_defines("_WIN32_WINNT=0x0A00")
    add_syslinks("ws2_32", "ole32", "oleaut32", "shell32", "uuid", "imm32", "shlwapi", "version")
end
if is_plat("linux", "mingw") then add_syslinks("pthread") end
if is_plat("linux") then add_syslinks("dl") end
add_includedirs("src", "src/third_party")

if has_config("portable_deps") then
    -- Official recipe install logic and selected archive checksums are committed
    -- together; URL-only adapters remove unverified Git-tag fallbacks.
    -- Resolve/install in the explicit CI dependency preparation step, before build.
    add_repositories("locked dependencies/xmake-repo", {rootdir = os.scriptdir()})
    set_policy("package.requires_lock", true)
    set_policy("package.precompiled", false)
    set_policy("package.cmake_generator.ninja", false)
    add_requires("locked@spdlog 1.17.0", {alias = "spdlog", system = false, configs = {header_only = true}})
    add_requires("locked@nlohmann_json 3.12.0", {alias = "nlohmann_json", system = false})
    add_requires("locked@toml++ 3.4.0", {alias = "toml++", system = false})
    add_requires("locked@doctest 2.5.2", {alias = "doctest", system = false, configs = {cmake = false}})
    if is_plat("macosx") then
        add_requires("locked@fast_float 8.3.0", {alias = "fast_float", system = false})
        add_packages("fast_float")
    end
    add_requires("locked@sqlite3 3.53.0+400", {alias = "sqlite3", system = false, configs = {shared = false}})
    -- Reuse runner/local CMake; do not bootstrap an unlocked tool dependency.
    add_requireconfs("**.cmake", {system = true, override = true})
    add_packages("spdlog", "nlohmann_json", "toml++")
else
    -- Preserve the existing, locally verified MSYS2 SDK for developer builds.
    add_includedirs(path.join(msys2, "include"))
    add_linkdirs(path.join(msys2, "lib"))
    add_links(path.join(msys2, "lib/libfmt.dll.a"))
    add_runenvs("PATH", path.join(msys2, "bin"))
end

rule("pcre2.locked")
    on_load(function (target)
        local json = import("core.base.json")
        local config = import("core.project.config")
        local root = os.projectdir()
        local lock = json.loadfile(path.join(root, "dependencies/pcre2.lock.json"))
        local function verify(file, expected)
            assert(os.isfile(file), "Locked dependency missing: " .. file .. "; run scripts/prepare_dependencies.py")
            assert(hash.sha256(file):lower() == expected:lower(), "Locked dependency SHA256 mismatch: " .. file)
        end
        for _, entry in pairs(lock.licenses) do verify(path.join(root, entry.vendored), entry.sha256) end
        local sdk = os.getenv("PCRE2_SDK_DIR")
        if target:is_plat("mingw") then
            sdk = sdk or msys2
            for _, entry in ipairs(lock.sdk.files) do verify(path.join(sdk, entry.path), entry.sha256) end
        else
            sdk = sdk or path.join(root, "build/dependencies/pcre2", target:plat(), target:arch(), config.mode(), "install")
            verify(path.join(root, lock.source.archive), lock.source.sha256)
            local manifest = json.loadfile(path.join(sdk, "kforge-sdk.json"))
            assert(manifest.source_sha256 == lock.source.sha256, "PCRE2 SDK was built from another source archive")
            assert(manifest.plat == target:plat() and manifest.arch == target:arch() and manifest.mode == config.mode(),
                   "PCRE2 SDK platform/architecture/mode mismatch")
            for _, entry in ipairs(manifest.files) do verify(path.join(sdk, entry.path), entry.sha256) end
        end
        local header = io.readfile(path.join(sdk, lock.sdk.version_header)) or ""
        local major = tonumber(header:match("#define%s+PCRE2_MAJOR%s+(%d+)"))
        local minor = tonumber(header:match("#define%s+PCRE2_MINOR%s+(%d+)"))
        assert(major == lock.sdk.version_macros.PCRE2_MAJOR and minor == lock.sdk.version_macros.PCRE2_MINOR,
               "Expected locked PCRE2 10.47 headers")
        target:add("includedirs", path.join(sdk, lock.link.include_dir))
        target:add("defines", lock.link.define)
        target:add("links", path.join(sdk, lock.link.library))
    end)
rule_end()

local function storage_dependencies()
    add_rules("pcre2.locked")
    if has_config("portable_deps") then add_packages("sqlite3")
    else add_links(path.join(msys2, "lib/libsqlite3.a")) end
end
local function storage_test_sources()
    add_files("src/core/db/**.cpp", "src/core/repo/**.cpp", "src/core/io/**.cpp")
    add_files("src/core/model/**.cpp")
    add_files("src/interface/api/import_manager.cpp", "src/interface/api/import_orchestrator.cpp")
    add_files("src/interface/service/import/pipeline.cpp", "src/interface/service/library/service.cpp")
    -- service.cpp 和 import_orchestrator.cpp 在成功编辑后调用 note_successful_edit()；
    -- 所有复用这两份源码的测试目标都必须同时链接编辑历史实现。
    add_files("src/interface/service/editing/history.cpp")
    add_files("src/util/config_store.cpp")
    add_files("src/classifier/**.cpp", "src/correspondence/**.cpp")
    storage_dependencies()
end
local function register_test()
    if has_config("portable_deps") then add_packages("doctest") end
    set_rundir("$(projectdir)")
    add_tests("default", {run_timeout = 120000})
end

target("KiCad_Forge")
    set_kind("binary")
    add_files("src/main.cpp", "src/interface/**.cpp", "src/core/io/**.cpp", "src/core/repo/**.cpp")
    add_files("src/core/db/**.cpp", "src/core/model/**.cpp", "src/util/**.cpp")
    add_files("src/classifier/**.cpp", "src/correspondence/**.cpp", "src/plugin/plugin_manager.cpp",
              "src/plugin/task_service.cpp")
    storage_dependencies()
    if is_plat("mingw") then
        add_sysincludedirs("src/third_party/webview", "src/third_party/webview2")
        add_defines("WEBVIEW_EDGE")
        add_files("resources/app.rc")
        if is_mode("release") then add_ldflags("-mwindows") end
    end
    -- 前端：dist 落后于源码就重建；缺依赖时按 lockfile 安装（npm ci）。
    -- 不做这一步，发布构建会直接打包上一次留下的 dist。
    before_build(function(target)
        local root = os.projectdir()
        local webui = path.join(root, "webui")
        local index = path.join(webui, "dist", "index.html")
        local stale = not os.isfile(index)
        if not stale then
            local built = os.mtime(index)
            local inputs = os.files(path.join(webui, "src", "**"))
            table.join2(inputs, os.files(path.join(webui, "index.html")))
            table.join2(inputs, os.files(path.join(webui, "package.json")))
            table.join2(inputs, os.files(path.join(webui, "package-lock.json")))
            -- The shared runtime imports SDK source; plugin source is NOT an
            -- input here. Changing one plugin must not rebuild the host.
            table.join2(inputs, os.files(path.join(webui, "packages", "plugin-sdk", "src", "**")))
            for _, f in ipairs(inputs) do
                if os.mtime(f) > built then stale = true break end
            end
        end
        if stale then
            local npm = is_host("windows") and "npm.cmd" or "npm"
            if not os.isdir(path.join(webui, "node_modules")) then
                print("  -> npm ci (webui)")
                if not os.execv(npm, {"ci"}, {curdir = webui}) then
                    raise("webui: npm ci failed — 前端依赖无法按 lockfile 安装")
                end
            end
            print("  -> npm run build (webui)")
            if not os.execv(npm, {"run", "build"}, {curdir = webui}) then
                raise("webui: npm run build failed — 界面产物缺失或过期")
            end
        end

        -- Plugin/SDK outputs are tracked independently. Rebuild them when an
        -- input is newer, but never put plugin sources into the webui inputs.
        local sdk_dist = path.join(webui, "packages", "plugin-sdk", "dist", "index.js")
        local plugins_stale = not os.isfile(sdk_dist)
        if not plugins_stale then
            local sdk_built = os.mtime(sdk_dist)
            for _, f in ipairs(os.files(path.join(webui, "packages", "plugin-sdk", "src", "**"))) do
                if os.mtime(f) > sdk_built then plugins_stale = true break end
            end
        end
        local examples = path.join(root, "examples", "plugins")
        if not plugins_stale and os.isdir(examples) then
            for _, manifest in ipairs(os.files(path.join(examples, "*", "manifest.json"))) do
                local dir = path.directory(manifest)
                local out = path.join(dir, "dist", "index.js")
                if not os.isfile(out) then plugins_stale = true break end
                local built = os.mtime(out)
                local inputs = os.files(path.join(dir, "src", "**"))
                table.join2(inputs, {manifest, path.join(dir, "package.json"), path.join(dir, "tsconfig.json")})
                for _, f in ipairs(inputs) do
                    if os.mtime(f) > built then plugins_stale = true break end
                end
                if plugins_stale then break end
            end
        end
        -- First-party plugin frontends live in plugins/<id>/frontend and are
        -- built independently from the host webui.
        local first_party_frontends = {
            path.join(root, "plugins", "git_version", "frontend"),
            path.join(root, "plugins", "lcsc_import", "frontend"),
        }
        if not plugins_stale then
            for _, frontend in ipairs(first_party_frontends) do
                local out = path.join(frontend, "dist", "index.js")
                if not os.isfile(out) then plugins_stale = true break end
                local built = os.mtime(out)
                local inputs = os.files(path.join(frontend, "src", "**"))
                table.join2(inputs, {path.join(frontend, "package.json"), path.join(frontend, "tsconfig.json")})
                for _, f in ipairs(inputs) do
                    if os.mtime(f) > built then plugins_stale = true break end
                end
                if plugins_stale then break end
            end
        end
        if plugins_stale then
            print("  -> build plugins (SDK + examples)")
            if not os.execv("node", {path.join(root, "scripts", "build_plugins.mjs")}, {curdir = root}) then
                raise("plugins: SDK/example build failed — 插件产物缺失或过期")
            end
        end
    end)

    after_build(function (target)
        local root = path.absolute(path.directory(target:targetfile()), os.projectdir())
        local find_tool = import("lib.detect.find_tool")
        local python = find_tool(is_host("windows") and "python" or "python3") or find_tool("python")
        assert(python, "Python is required to validate and stage runtime assets")
        os.execv(python.program, {"-B", "-X", "utf8", path.join(os.projectdir(), "scripts/sync_runtime_assets.py"),
                                "--project", os.projectdir(), "--output", root})
        os.mkdir(path.join(root, "data"))
        if target:is_plat("mingw") then
            local cxx = target:tool("cxx")
            local bindir = path.directory(cxx)
            for _, name in ipairs({"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"}) do
                local source = path.join(bindir, name)
                if not os.isfile(source) then source = path.join(msys2, "bin", name) end
                assert(os.isfile(source), "Missing MinGW runtime: " .. source)
                os.cp(source, path.join(root, name))
            end
            if not has_config("portable_deps") then
                os.cp(path.join(msys2, "bin/libfmt-12.dll"), path.join(root, "libfmt-12.dll"))
            end
        end
    end)
target_end()

target("bench_import")
    set_kind("binary")
    set_default(false)
    add_files("tests/bench_import.cpp")
    storage_test_sources()
target_end()

for _, name in ipairs({"test_logger", "test_plugin_manager", "test_launcher", "test_symbol_hash"}) do
    target(name)
        set_kind("binary")
        add_files("tests/" .. name .. ".cpp")
        register_test()
        if name == "test_plugin_manager" then add_files("src/plugin/plugin_manager.cpp")
        elseif name == "test_launcher" then add_files("tests/test_launcher_lifecycle.cpp")
        elseif name == "test_symbol_hash" then add_files("src/core/model/**.cpp") end
    target_end()
end

target("test_task_service")
    set_kind("binary")
    add_files("tests/test_task_service.cpp")
    add_files("src/plugin/task_service.cpp", "src/plugin/plugin_manager.cpp")
    add_files("src/util/config_store.cpp")
    register_test()
target_end()

target("test_app_window")
    set_kind("binary")
    add_files("tests/test_app_window.cpp")
    register_test()
    if is_plat("mingw") then
        add_sysincludedirs("src/third_party/webview", "src/third_party/webview2")
        add_defines("WEBVIEW_EDGE")
    end
target_end()

target("test_service_startup")
    set_kind("binary")
    add_files("tests/test_service_startup.cpp")
    storage_test_sources()
    register_test()
target_end()

target("test_data_integrity")
    set_kind("binary")
    add_defines('KFORGE_TEST_PROJECT_ROOT="' .. string.gsub(os.projectdir(), "\\", "/") .. '"')
    add_files("tests/test_config_store.cpp", "tests/test_text_util.cpp", "tests/test_library_workflows.cpp")
    add_files("tests/test_import_contract.cpp")
    add_files("tests/test_symbol_inheritance.cpp")
    add_files("tests/test_public_queries.cpp", "tests/test_library_manifest.cpp", "src/interface/service/correspondence/service.cpp")
    add_files("src/interface/service/library/binding.cpp", "src/interface/service/classification/service.cpp")
    add_files("src/interface/service/library/footprint_resolver.cpp")
    add_files("src/interface/service/editing/export.cpp", "src/interface/service/editing/paste.cpp")
    add_files("src/interface/service/editing/move.cpp")
    add_files("tests/test_editing_commands.cpp")
    add_files("src/interface/api/staged_import.cpp", "src/interface/api/library_manifest.cpp", "src/plugin/plugin_manager.cpp")
    storage_test_sources()
    register_test()
target_end()

-- GNU --wrap is supported by ELF ld and MinGW lld. Apple ld has no equivalent;
-- macOS still runs the ordinary matcher and library workflow regressions above.
if is_plat("linux", "mingw") then
    target("test_matching_regex_raii")
        set_kind("binary")
        add_files("tests/test_matching_regex_raii.cpp", "src/classifier/matching_regex.cpp")
        add_rules("pcre2.locked")
        add_ldflags("-Wl,--wrap=pcre2_compile_context_create_8", "-Wl,--wrap=pcre2_compile_context_free_8")
        register_test()
    target_end()
end
