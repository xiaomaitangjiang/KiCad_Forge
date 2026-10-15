set_xmakever("3.1.1")
set_project("KiCad_Forge")
set_languages("cxx23")
set_defaultmode("debug")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate")

-- Keep the existing Windows toolchain while letting Unix hosts build natively.
if is_host("windows") then
    set_plat("mingw")
end
if is_plat("mingw") then
    set_toolchains("clang", {target = "x86_64-w64-windows-gnu"})
    add_cxflags("--target=x86_64-w64-windows-gnu")
    add_ldflags("-fuse-ld=lld")
    add_defines("_WIN32_WINNT=0x0A00")
    add_syslinks("ws2_32", "ole32", "oleaut32", "shell32", "uuid", "imm32", "shlwapi", "version")
end
if is_plat("linux", "mingw") then
    add_syslinks("pthread")
end
if is_plat("linux") then
    add_syslinks("dl")
end

-- Header-only logging avoids a dependency on a machine-specific fmt DLL.
-- Versions are explicit so every platform uses the same dependencies.
add_requires("spdlog 1.17.0", {configs = {header_only = true}})
add_requires("nlohmann_json 3.12.0", "toml++ 3.4.0", "doctest 2.5.3")
add_requires("sqlite3 3.53.0+400", {configs = {shared = false}})
add_includedirs("src", "src/third_party")
add_packages("spdlog", "nlohmann_json", "toml++")
set_targetdir("build/$(plat)/$(arch)/$(mode)")

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
    add_packages("sqlite3")

    if is_plat("mingw") then
        add_sysincludedirs("src/third_party/webview", "src/third_party/webview2")
        add_defines("WEBVIEW_EDGE")
        add_files("resources/app.rc")
        if is_mode("release") then
            add_ldflags("-mwindows")
        end
        before_build(function (target)
            local svg = path.join(os.projectdir(), "webui/public/favicon.svg")
            local ico = path.join(os.projectdir(), "resources/app.ico")
            if not os.isfile(ico) or os.mtime(svg) > os.mtime(ico) then
                import("lib.detect.find_tool")
                local python = assert(find_tool("python") or find_tool("python3"), "Python is required to generate app.ico")
                os.vrunv(python.program, {"scripts/svg2ico.py"})
                os.touch(path.join(os.projectdir(), "resources/app.rc"))
            end
        end)
    end

    after_build(function (target)
        local root = path.directory(target:targetfile())
        for _, name in ipairs({"webui", "plugins", "config"}) do
            os.rm(path.join(root, name))
        end
        os.cp("$(projectdir)/webui/dist", path.join(root, "webui/dist"))
        os.cp("$(projectdir)/plugins", path.join(root, "plugins"))
        os.cp("$(projectdir)/config", path.join(root, "config"))
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

    add_files("resources/app.rc")  -- Windows exe icon

    add_includedirs("src", "src/third_party", sys_inc)
    -- Third-party headers as system includes — suppress their internal warnings
    add_sysincludedirs("src/third_party/webview", "src/third_party/webview2")
    add_linkdirs(sys_lib)
    add_defines("_WIN32_WINNT=0x0A00", "WEBVIEW_EDGE")
    -- Force MinGW target so clangd doesn't pick up MSVC headers
    add_cxflags("--target=x86_64-w64-windows-gnu")

    add_syslinks("pthread", "ws2_32", "ole32", "oleaut32", "shell32", "uuid", "imm32", "shlwapi", "version")
    add_links(sys_lib .. "/libsqlite3.a", sys_lib .. "/libfmt.dll.a")

    if is_mode("release") then
        add_ldflags("-mwindows")  -- no console window in release
    end

    after_build(function(target)
        local root = path.directory(target:targetfile())
        -- Web UI
        local dest = path.join(root, "webui", "dist")
        os.rm(path.join(root, "webui", "*"))
        os.cp("$(projectdir)/webui/dist", dest)
        -- Plugins (remove existing dir to ensure new files like icons sync)
        os.rm(path.join(root, "plugins"))
        os.cp("$(projectdir)/plugins", path.join(root, "plugins"))
        -- Type config seeds：运行期可改的配置住在数据目录，这里只放首次运行的种子。
        -- 先删再拷：目标已存在时 os.cp 会往里再套一层
        os.rm(path.join(root, "config"))
        os.cp("$(projectdir)/config", path.join(root, "config"))
        -- Portable data directory (DB, settings go here)
        os.mkdir(path.join(root, "data"))
        -- Copy required MinGW DLLs so exe runs standalone (no MSYS2 PATH needed)
        os.cp(path.join(msys2, "bin", "libstdc++-6.dll"), path.join(root, "libstdc++-6.dll"))
        os.cp(path.join(msys2, "bin", "libgcc_s_seh-1.dll"), path.join(root, "libgcc_s_seh-1.dll"))
        os.cp(path.join(msys2, "bin", "libwinpthread-1.dll"), path.join(root, "libwinpthread-1.dll"))
        os.cp(path.join(msys2, "bin", "libfmt-12.dll"), path.join(root, "libfmt-12.dll"))
    end)
    add_runenvs("PATH", msys2 .. "/bin")

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
    add_packages("sqlite3")

target_end()
for _, name in ipairs({"test_logger", "test_plugin_manager", "test_launcher", "test_symbol_hash"}) do
    target(name)
        set_kind("binary")
        add_files("tests/" .. name .. ".cpp")
        add_packages("doctest")
        set_rundir("$(projectdir)")
        add_tests("default", {run_timeout = 60000})
        if name == "test_plugin_manager" then
            add_files("src/plugin/plugin_manager.cpp")
        elseif name == "test_symbol_hash" then
            add_files("src/core/model/**.cpp")
        end
    target_end()
end
