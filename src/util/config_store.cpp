#include "util/config_store.h"

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

void ConfigStore::flush() const
{
    auto parent = path_.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    auto tmp = path_;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }
    out << data_.dump(2);
    out.flush();
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);  // atomic on POSIX & Win32
    if (ec)
    {
        std::filesystem::remove(tmp, ec);
    }
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
        return s == "true" || s == "1";
    }
    return default_val;
}

void ConfigStore::set(const std::string& key, std::string value)
{
    std::unique_lock lock(mtx_);
    data_[key] = std::move(value);
    flush();
}

void ConfigStore::set_bool(const std::string& key, bool val)
{
    std::unique_lock lock(mtx_);
    data_[key] = val;
    flush();
}

nlohmann::json ConfigStore::all() const
{
    std::shared_lock lock(mtx_);
    return data_;
}

}  // namespace kforge::util
