set_toolchains("clang")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")
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
    add_files("src/core/type_registry.cpp")

    add_includedirs("src", "src/third_party", sys_inc)
    add_linkdirs(sys_lib)
    add_defines("_WIN32_WINNT=0x0A00")

    add_syslinks("pthread", "ws2_32", "ole32", "oleaut32", "uuid")
    add_links(sys_lib .. "/libsqlite3.a")

    if is_mode("release") then
        add_ldflags("-mwindows")  -- no console window in release
    end

    after_build(function(target)
        local root = path.directory(target:targetfile())
        -- Web UI
        local dest = path.join(root, "webui", "dist")
        os.rm(path.join(root, "webui", "*"))
        os.cp("$(projectdir)/webui/dist", dest)
        -- Plugins
        os.cp("$(projectdir)/plugins", path.join(root, "plugins"))
        -- Portable data directory (DB, settings go here)
        os.mkdir(path.join(root, "data"))
    end)
    add_runenvs("PATH", msys2 .. "/bin")
