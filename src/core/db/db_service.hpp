// Launcher 服务：Database 的启动包装。
// build 在 Logger 之后执行 —— DB open 失败时 logger 已就绪，可用 log_error。
#pragma once

#include "core/db/database.h"
#include "util/logger.h"
#include "util/service_runtime.hpp"

#include <memory>
#include <string>

namespace kforge::storage
{

struct DbService
{
    std::unique_ptr<Database> db;
    std::string path;

    explicit DbService(std::string p) : path(std::move(p)) {}

    // Launcher build：打开 DB；失败时 logger 已 init，可 log_error
    static kforge::util::Result<void> build()
    {
        auto& self = kforge::launcher::util::instance_store<DbService>().value();
        auto r = Database::open(self.path);
        if (!r)
        {
            kforge::util::log_error{}("Database open failed: {}", r.error().format_message());
            return std::unexpected(r.error());
        }
        self.db = std::move(*r);
        kforge::util::log_info{}("Database opened: {}", self.path);
        return {};
    }

    // 提供 sqlite3* 给其他服务 build 注入
    sqlite3* handle() const { return db ? db->handle() : nullptr; }

    static kforge::util::Result<void> destroy()
    {
        auto& self = kforge::launcher::util::instance_store<DbService>();
        if (self)
            self->db.reset();
        return {};
    }
};

}  // namespace kforge::storage
