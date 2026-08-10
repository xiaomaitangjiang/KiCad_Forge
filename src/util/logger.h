// SPDX-License-Identifier: MIT
// Thin wrapper over spdlog with fmt-formatted convenience macros.
// Usage: KF_LOG_INFO("server started on port {}", port);
#pragma once

#define SPDLOG_HEADER_ONLY
#include <chrono>
#include <filesystem>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace kforge::util {

inline void init_logger(const std::string& log_path) {
    try {
        // Cleanup: delete log files older than 7 days
        auto log_dir = std::filesystem::path(log_path).parent_path();
        if (std::filesystem::exists(log_dir)) {
            auto now = std::filesystem::file_time_type::clock::now();
            for (auto& entry : std::filesystem::directory_iterator(log_dir)) {
                if (entry.path().extension() == ".log") {
                    auto ftime = std::filesystem::last_write_time(entry);
                    if (now - ftime > std::chrono::hours(24 * 7))
                        std::filesystem::remove(entry.path());
                }
            }
        }

        // Daily rotation with timestamp suffix: forge_2026-08-04.log
        auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            log_path, 0, 0);  // rotate at midnight
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>(
            "forge", spdlog::sinks_init_list{file_sink, console_sink});
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
    } catch (const spdlog::spdlog_ex&) {
        spdlog::set_default_logger(spdlog::stdout_color_mt("forge"));
        spdlog::set_level(spdlog::level::debug);
    }
}

inline void shutdown_logger() {
    spdlog::drop_all();
}

// ---- Fmt-formatted convenience functions ----
template <typename... Args>
void log_info(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::info(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void log_error(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::error(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void log_warn(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::warn(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void log_debug(fmt::format_string<Args...> fmt, Args&&... args) {
    spdlog::debug(fmt, std::forward<Args>(args)...);
}

}  // namespace kforge::util

// ---- File + line macros for source-location context ----
#define LOG_INFO(...)  ::kforge::util::log_info(__VA_ARGS__)
#define LOG_ERROR(...) ::kforge::util::log_error(__VA_ARGS__)
#define LOG_WARN(...)  ::kforge::util::log_warn(__VA_ARGS__)
#define LOG_DEBUG(...) ::kforge::util::log_debug(__VA_ARGS__)
