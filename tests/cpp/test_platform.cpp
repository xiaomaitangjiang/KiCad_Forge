#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "util/platform.h"
#include <chrono>
#include <fstream>

TEST_CASE("macOS bundle resolves immutable resources and writable user data separately")
{
    const std::filesystem::path exe = "/Applications/KiCad Forge.app/Contents/MacOS";
    CHECK(kforge::util::get_resource_dir(exe) ==
          std::filesystem::path("/Applications/KiCad Forge.app/Contents/Resources"));
    const char* home = std::getenv("HOME");
    if (home)
        CHECK(std::filesystem::path(kforge::util::get_data_dir(exe)) ==
              std::filesystem::path(home) / "Library/Application Support/KiCad_Forge/data");
    CHECK(kforge::util::get_python_executable(exe) ==
          std::filesystem::path("/Applications/KiCad Forge.app/Contents/Resources/python/bin/python3"));
}

TEST_CASE("ordinary and similarly named directories preserve portable resource layout")
{
    for (const auto* dir : {"/tmp/build", "/tmp/app/Contents/MacOS", "/tmp/app.app/MacOS"})
        CHECK(kforge::util::get_resource_dir(dir) == std::filesystem::path(dir));
    CHECK_FALSE(kforge::util::get_exe_dir().empty());
    CHECK_FALSE(kforge::util::get_data_dir().empty());
}

TEST_CASE("installed distributions resolve private Python even when its executable is missing")
{
    const auto root = std::filesystem::temp_directory_path() /
        ("kforge-private-python-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::ofstream(root / "kforge-distribution.json") << "{}";
#ifdef _WIN32
    CHECK(kforge::util::get_python_executable(root) == root / "runtime/python/python.exe");
#else
    CHECK(kforge::util::get_python_executable(root) == root / "runtime/python/bin/python3");
#endif
    std::filesystem::remove(root / "kforge-distribution.json");
    std::filesystem::create_directories(root / "runtime/python");
#ifdef _WIN32
    CHECK(kforge::util::get_python_executable(root) == root / "runtime/python/python.exe");
#else
    CHECK(kforge::util::get_python_executable(root) == root / "runtime/python/bin/python3");
#endif
}
