// Launcher 服务：Database 的启动包装。
// build 在 Logger 之后执行 —— DB open 失败时 logger 已就绪，可用 log_error。
#pragma once

#include "core/db/database.h"
#include "core/db/db_session.h"
#include "core/io/source_mutation.h"
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

    explicit DbService(std::string p) : path(std::move(p))
    {
    }
    DbService(DbService&&) = default;
    DbService& operator=(DbService&&) = delete;

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

        // 开放 HTTP 与导入之前先把上次未走完的源文件变更收尾（A05）
        kforge::io::SourceMutationService mutations(*self.db);
        (void) mutations.recover();
        return {};
    }

    /// 取一次独占数据库操作。所有仓储都要在 session 生命周期内构造。
    [[nodiscard]] DbSession session()
    {
        return db->session();
    }

    /// 连接持有者本身 —— 需要自己去开 session 的服务（导入、库服务）拿这个
    [[nodiscard]] Database& database() { return *db; }

    static kforge::util::Result<void> destroy()
    {
        auto& self = kforge::launcher::util::instance_store<DbService>();
        if (self)
            self->db.reset();
        return {};
    }
};

}  // namespace kforge::storage
