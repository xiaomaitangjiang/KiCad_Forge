#include "util/config_store.h"
#include "util/file_write.h"

#include <fstream>
#include <mutex>

namespace kforge::util
{

ConfigStore::ConfigStore(std::filesystem::path file) : path_(std::move(file))
{
    load();
}

void ConfigStore::load()
{
    std::unique_lock lock(mtx_);
    data_ = nlohmann::json::object();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec))
    {
        return;  // First run — empty config; flush() will create the file.
    }
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open())
    {
        return;
    }
    try
    {
        in >> data_;
        if (!data_.is_object())
        {
            data_ = nlohmann::json::object();
        }
    }
    catch (const nlohmann::json::parse_error&)
    {
        data_ = nlohmann::json::object();  // corrupt file — start fresh
    }
}

void ConfigStore::flush(const nlohmann::json& data) const
{
    auto parent = path_.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
    replace_file(path_, data.dump(2));
}

std::string ConfigStore::get(const std::string& key) const
{
    std::shared_lock lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end() || !it->is_string())
    {
        return {};
    }
    return it->get<std::string>();
}

bool ConfigStore::get_bool(const std::string& key, bool default_val) const
{
    std::shared_lock lock(mtx_);
    auto it = data_.find(key);
    if (it == data_.end())
    {
        return default_val;
    }
    if (it->is_boolean())
    {
        return it->get<bool>();
    }
    if (it->is_string())
    {
        auto s = it->get<std::string>();
        if (s == "true" || s == "1") return true;
        if (s == "false" || s == "0") return false;
    }
    return default_val;
}

void ConfigStore::set(const std::string& key, std::string value)
{
    std::unique_lock lock(mtx_);
    auto next = data_;
    next[key] = std::move(value);
    flush(next);
    data_ = std::move(next);
}

void ConfigStore::set_bool(const std::string& key, bool val)
{
    std::unique_lock lock(mtx_);
    auto next = data_;
    next[key] = val;
    flush(next);
    data_ = std::move(next);
}

nlohmann::json ConfigStore::all() const
{
    std::shared_lock lock(mtx_);
    return data_;
}

}  // namespace kforge::util
