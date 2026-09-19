// JSON-file config store — replaces the old DB `settings` table.
// Atomic write (tmp + rename). Thread-safe via shared_mutex.
#pragma once

#include <filesystem>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace kforge::util
{

class ConfigStore
{
public:
    explicit ConfigStore(std::filesystem::path file);

    std::string get(const std::string& key) const;
    bool get_bool(const std::string& key, bool default_val = false) const;
    // I/O failure throws; the in-memory value is committed only after saving.
    void set(const std::string& key, std::string value);
    void set_bool(const std::string& key, bool val);

    nlohmann::json all() const;

private:
    std::filesystem::path path_;
    mutable std::shared_mutex mtx_;
    nlohmann::json data_;

    void load();
    void flush(const nlohmann::json& data) const;
};

}  // namespace kforge::util
