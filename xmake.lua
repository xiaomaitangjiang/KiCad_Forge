set_toolchains("clang", {target = "x86_64-w64-windows-gnu"})
set_plat("mingw")
add_ldflags("-fuse-ld=lld")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate")
local mode="DEBUG"
set_defaultmode(mode)
set_languages("cxx23")

local msys2 = os.getenv("MSYS2_DIR") or "D:/msys64/mingw64"
local sys_inc = msys2 .. "/include"
local sys_lib = msys2 .. "/lib"

-- Run with:  $env:PATH="D:\msys64\mingw64\bin;$env:PATH"
--            xmake f -p mingw -c
--            xmake build
-- Or just:   .\make.ps1

target("KiCad_Forge")
    set_kind("binary")

    add_files("src/main.cpp")
    add_files("src/api/**.cpp")
    add_files("src/services/**.cpp")
    add_files("src/sexpr/**.cpp")
    add_files("src/parser/**.cpp")
    add_files("src/storage/**.cpp")
    add_files("src/classifier/**.cpp")
    add_files("src/correspondence/**.cpp")
    add_files("src/plugin/plugin_manager.cpp")
    -- Auto-generate app.ico from favicon.svg before building
    before_build(function(target)
        local svg = path.join(os.projectdir(), "webui", "public", "favicon.svg")
        local ico = path.join(os.projectdir(), "resources", "app.ico")
        local script = path.join(os.projectdir(), "scripts", "svg2ico.py")
        if os.isfile(svg) and (not os.isfile(ico) or os.mtime(svg) > os.mtime(ico)) then
            -- Try Windows Python first, then system python
            local python = os.getenv("LOCALAPPDATA") .. "\\Programs\\Python\\Python313\\python.exe"
            if not os.isfile(python) then python = "python" end
            local ok = os.execv(python, {script})
            if ok then
                print("  ✓ icon regenerated from favicon.svg")
                os.touch(path.join(os.projectdir(), "resources", "app.rc"))
            end
        end
    end)
    add_files("resources/app.rc")  -- Windows exe icon
    add_files("src/core/symbol.cpp")
    add_files("src/core/type_registry.cpp")

    add_includedirs("src", "src/third_party", "src/third_party/webview", "src/third_party/webview2", sys_inc)
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
    add_files("tests/bench_import.cpp")
    add_files("src/core/symbol.cpp")
    add_files("src/core/type_registry.cpp")
    add_files("src/sexpr/**.cpp")
    add_files("src/parser/**.cpp")
    add_files("src/storage/**.cpp")
    add_files("src/services/library_service.cpp")
    add_files("src/services/import_pipeline.cpp")
    add_files("src/classifier/**.cpp")
    add_files("src/correspondence/**.cpp")
    add_includedirs("src", "src/third_party")
    add_linkdirs(sys_lib)
    add_defines("_WIN32_WINNT=0x0A00")
    add_cxflags("--gcc-toolchain=" .. msys2)
    add_syslinks("pthread", "ws2_32", "ole32", "oleaut32", "shell32", "uuid")
    add_links(sys_lib .. "/libsqlite3.a", sys_lib .. "/libfmt.dll.a")
