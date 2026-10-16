#pragma once

#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace test_support
{
class TempDirectory
{
public:
    TempDirectory()
    {
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt)
        {
            auto candidate = std::filesystem::temp_directory_path() /
                ("kforge-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(candidate))
            {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot create isolated test directory");
    }
    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class RestoreDefaultLogger
{
public:
    RestoreDefaultLogger() : previous_(spdlog::default_logger()) {}
    ~RestoreDefaultLogger() { spdlog::set_default_logger(previous_); }
    RestoreDefaultLogger(const RestoreDefaultLogger&) = delete;
    RestoreDefaultLogger& operator=(const RestoreDefaultLogger&) = delete;

private:
    std::shared_ptr<spdlog::logger> previous_;
};
}
