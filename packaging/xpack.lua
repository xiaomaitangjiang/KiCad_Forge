includes("@builtin/xpack")

local formats = is_plat("macosx") and {"dmg", "zip"}
    or (is_plat("mingw") and {"nsis", "zip"} or {"deb", "targz", "zip"})

for _, format in ipairs(formats) do
    xpack("KiCad_Forge_" .. format)
        set_formats(format)
        set_title("KiCad Forge")
        set_description("KiCad component library manager")
        set_inputkind("binary")
        set_version(os.getenv("KFORGE_PACKAGE_VERSION") or "1.1.1")
        set_basename("KiCad_Forge-$(plat)-$(arch)-$(version)")
        add_targets("KiCad_Forge")

        on_load(function (package)
            import("lib.detect.find_tool")
            local python = find_tool(is_host("windows") and "python" or "python3")
            assert(python, "Python is required to validate distribution contents")
            local root = os.projectdir()
            local target = package:target("KiCad_Forge")
            local args = {"-B", "-X", "utf8", path.join(root, "scripts/stage_distribution.py"),
                "--source", path.absolute(path.directory(target:targetfile()), root),
                "--output", path.absolute(path.join(package:builddir(), "payload"), root),
                "--platform", package:plat(), "--arch", package:arch(), "--version", package:version()}
            table.join2(args, {"--runtime", path.join(root, "build/packaging-deps", package:plat(), package:arch(), "python")})
            os.execv(python.program, args)
        end)

        on_installcmd(function (package, batchcmds)
            local payload = path.absolute(path.join(package:builddir(), "payload"))
            if package:is_plat("macosx") then
                batchcmds:cp(path.join(payload, "KiCad Forge.app"), package:installdir("KiCad Forge.app"))
            else
                -- Archives have one top-level directory. User data is created
                -- outside the installed payload, never copied from a developer.
                batchcmds:cp(payload, package:installdir(package:basename()))
            end
        end)

        if format == "dmg" then
            -- Finder's drag-install layout must be saved on a writable image
            -- before converting it to the final compressed read-only DMG.
            on_package(function (package)
                import("lib.detect.find_tool")
                local python = assert(find_tool("python3"))
                os.execv(python.program, {"-B", "-X", "utf8", path.join(os.projectdir(), "scripts/build_dmg.py"),
                    "--source", path.absolute(path.join(package:builddir(), "payload")),
                    "--output", path.absolute(package:outputdir()),
                    "--name", package:basename(), "--version", package:version()})
            end)
        elseif format == "deb" then
            -- Package the already-built payload with native dpkg tools. The
            -- default deb backend rebuilds sources with debuild instead.
            on_package(function (package)
                import("lib.detect.find_tool")
                local python = assert(find_tool("python3"))
                os.execv(python.program, {"-B", "-X", "utf8", path.join(os.projectdir(), "scripts/build_deb.py"),
                    "--source", path.absolute(path.join(package:builddir(), "payload")),
                    "--output", path.absolute(package:outputdir()),
                    "--name", package:basename(), "--version", package:version(), "--arch", package:arch()})
            end)
        elseif format == "nsis" then
            -- The built-in NSIS backend bootstraps an unlocked compiler/UAC
            -- plugin. Use the project's validated, explicitly prepared compiler.
            on_package(function (package)
                import("lib.detect.find_tool")
                local python = assert(find_tool("python"))
                os.execv(python.program, {"-B", "-X", "utf8", path.join(os.projectdir(), "scripts/build_installer.py"),
                    "--source", path.absolute(path.join(package:builddir(), "payload")),
                    "--output", path.absolute(package:outputdir()),
                    "--name", package:basename(), "--version", package:version()})
            end)
        end
    xpack_end()
end
