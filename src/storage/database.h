#pragma once

#include <filesystem>
#include <memory>
#include <string>

struct sqlite3;

#include "../util/result.h"

namespace kforge::storage {

/// SQLite database wrapper — public-domain sqlite3 C API.
/// No Qt or any heavy framework dependency.
class Database {
public:
    ~Database();

    static util::Result<std::unique_ptr<Database>> open(
        const std::filesystem::path& path);

    sqlite3* handle() { return db_; }
    util::Result<void> execute(const std::string& sql);
    int schema_version() const { return schema_version_; }
    const std::filesystem::path& path() const { return path_; }

private:
    Database() = default;
    util::Result<void> run_migrations();

    sqlite3* db_{nullptr};
    std::filesystem::path path_;
    int schema_version_{0};
};

}  // namespace kforge::storage
