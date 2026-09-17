// SPDX-License-Identifier: MIT
// Thin wrapper over spdlog — std::source_location replaces macros
// Usage: kforge::util::log_info("server started on port {}", port);
#pragma once

#define SPDLOG_HEADER_ONLY
#include <chrono>
#include <filesystem>
#include <source_location>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "util/error.h"
#include "util/platform.h"

namespace kforge::util
{

inline void init_logger(const std::string& log_path)
{
    try
    {
        // Cleanup: delete log files older than 7 days
        auto log_dir = std::filesystem::path(log_path).parent_path();
        if (std::filesystem::exists(log_dir))
        {
            auto now = std::filesystem::file_time_type::clock::now();
            for (auto& entry : std::filesystem::directory_iterator(log_dir))
            {
                if (entry.path().extension() == ".log")
                {
                    auto ftime = std::filesystem::last_write_time(entry);
                    if (now - ftime > std::chrono::hours(24 * 7))
                        std::filesystem::remove(entry.path());
                }
            }
        }

        // Daily rotation with timestamp suffix: forge_2026-08-04.log
        auto file_sink =
            std::make_shared<spdlog::sinks::daily_file_sink_mt>(log_path, 0, 0);  // rotate at midnight
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>(
            "forge", spdlog::sinks_init_list{file_sink, console_sink});
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
    }
    catch (const spdlog::spdlog_ex&)
    {
        spdlog::set_default_logger(spdlog::stdout_color_mt("forge"));
        spdlog::set_level(spdlog::level::debug);
    }
}

inline void shutdown_logger()
{
    spdlog::drop_all();
}

// ---- Unified struct-based log — generic base + tag-driven specialization ----
//      log_X{}(fmt, args...) — uniform syntax across levels

struct with_location
{
};
struct without_location
{
};

// Base: no location
template <spdlog::level::level_enum Lvel, typename LocTag = without_location>
struct log_fn
{
    template <typename... Args>
    void operator()(fmt::format_string<Args...> fmt, Args&&... args)
    {
        spdlog::log(Lvel, fmt, std::forward<Args>(args)...);
    }
};

// Specialization: with location
template <spdlog::level::level_enum Lvel>
struct log_fn<Lvel, with_location>
{
    std::source_location loc;

    log_fn(std::source_location l = std::source_location::current()) : loc(l)
    {
    }

    template <typename... Args>
    void operator()(fmt::format_string<Args...> fmt, Args&&... args)
    {
        spdlog::log(spdlog::source_loc{loc.file_name(), static_cast<int>(loc.line()),
                                       loc.function_name()},
                    Lvel, fmt, std::forward<Args>(args)...);
    }
};

using log_info = log_fn<spdlog::level::info>;
using log_debug = log_fn<spdlog::level::debug>;
using log_warn = log_fn<spdlog::level::warn>;
using log_error = log_fn<spdlog::level::err, with_location>;

// ---- Launcher 服务：Logger ----
// 空标记类型，实例入 launcher 静态槽；build 初始化默认 logger（data 目录），destroy 释放。

struct Logger
{
    static inline kforge::util::Result<void> build()
    {
        kforge::util::init_logger(kforge::util::get_data_dir() + "/forge.log");
        return {};
    }
    static inline kforge::util::Result<void> destroy()
    {
        kforge::util::shutdown_logger();
        return {};
    }
};

}  // namespace kforge::util
