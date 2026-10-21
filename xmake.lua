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
target("KiCad_Forge")
    set_kind("binary")
    add_files("src/main.cpp")
    add_files("src/interface/**.cpp")
    add_files("src/core/io/**.cpp")
    add_files("src/core/repo/**.cpp")
    add_files("src/core/db/**.cpp")
    add_files("src/core/model/**.cpp")
    add_files("src/util/**.cpp")
    add_files("src/classifier/**.cpp")
    add_files("src/correspondence/**.cpp")
    add_files("src/plugin/plugin_manager.cpp")
    storage_dependencies()

    if is_plat("mingw") then
        add_sysincludedirs("src/third_party/webview", "src/third_party/webview2")
        add_defines("WEBVIEW_EDGE")
        add_files("resources/app.rc")
        if is_mode("release") then
            add_ldflags("-mwindows")
        end
        -- app.ico is tracked. Regenerate it manually when changing the artwork;
        -- normal application builds do not need CairoSVG or Pillow.
    end

    after_build(function (target)
        local root = path.directory(target:targetfile())
        for _, name in ipairs({"webui", "plugins", "config"}) do
            os.rm(path.join(root, name))
        end
        os.cp("$(projectdir)/webui/dist", path.join(root, "webui/dist"))
        os.cp("$(projectdir)/plugins", path.join(root, "plugins"))
        os.cp("$(projectdir)/config", path.join(root, "config"))
        os.cp("$(projectdir)/resources/licenses", path.join(root, "licenses"))
        for _, dir in ipairs(os.dirs(path.join(root, "plugins/**/__pycache__"))) do
            os.rm(dir)
        end
        os.mkdir(path.join(root, "data"))

        if target:is_plat("mingw") then
            local cxx = target:tool("cxx")
            local bindir = path.directory(cxx)
            -- MinGW's C++ runtime must also be available outside the MSYS2 shell.
            for _, dll in ipairs({"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll"}) do
                local source = path.join(bindir, dll)
                assert(os.isfile(source), "Missing MinGW runtime: " .. source)
                os.cp(source, path.join(root, dll))
            end
        end
    end)
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
            for _, f in ipairs(inputs) do
                if os.mtime(f) > built then stale = true break end
            end
        end
        if not stale then return end

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
    end)

target_end()


target("bench_import")
    set_kind("binary")
    set_default(false)
    add_files("tests/bench_import.cpp")
    add_files("src/core/model/**.cpp")
    add_files("src/core/io/**.cpp")
    add_files("src/core/repo/**.cpp")
    add_files("src/core/db/**.cpp")
    add_files("src/interface/service/library/service.cpp")
    add_files("src/interface/service/import/pipeline.cpp")
    add_files("src/classifier/**.cpp")
    add_files("src/correspondence/**.cpp")
    storage_dependencies()

target_end()
for _, name in ipairs({"test_logger", "test_plugin_manager", "test_launcher", "test_symbol_hash"}) do
    target(name)
        set_kind("binary")
        add_files("tests/" .. name .. ".cpp")
        if has_config("portable_deps") then add_packages("doctest") end
        set_rundir("$(projectdir)")
        add_tests("default", {run_timeout = 60000})
        if name == "test_plugin_manager" then
            add_files("src/plugin/plugin_manager.cpp")
        elseif name == "test_symbol_hash" then
            add_files("src/core/model/**.cpp")
        end
    target_end()
end
