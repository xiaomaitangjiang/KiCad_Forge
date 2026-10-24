// 数据库操作边界：一次完整数据库操作内的独占访问 + 显式事务。
// 仓储只接受 session 提供的连接，服务不得跨请求缓存裸 sqlite3*。
#pragma once

#include <mutex>
#include <string>
#include <utility>

#include <sqlite3.h>

#include "util/error.h"

namespace kforge::storage
{

/// 一次完整的数据库操作。构造时取得进程内独占锁，析构时释放；
/// 期间对连接的访问一律经 handle()。事务由 begin()/commit() 显式管理，
/// 析构时仍有未提交事务则回滚 —— 未提交状态不会留给下一个 session。
class DbSession
{
public:
    DbSession(sqlite3* db, std::timed_mutex& mutex) : lock_(mutex), db_(db)
    {
    }

    /// 已持锁的会话：由 Database::try_session 在限时取锁成功后移交
    DbSession(sqlite3* db, std::unique_lock<std::timed_mutex> lock)
        : lock_(std::move(lock)), db_(db)
    {
    }

    DbSession(DbSession&& other) noexcept
        : lock_(std::move(other.lock_)), db_(std::exchange(other.db_, nullptr)),
          in_transaction_(std::exchange(other.in_transaction_, false))
    {
    }
    DbSession(const DbSession&) = delete;
    DbSession& operator=(const DbSession&) = delete;
    DbSession& operator=(DbSession&&) = delete;

    ~DbSession()
    {
        if (in_transaction_ && db_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    [[nodiscard]] sqlite3* handle() const { return db_; }
    [[nodiscard]] bool in_transaction() const { return in_transaction_; }

    /// BEGIN IMMEDIATE —— 立即取得写锁，避免读事务升级失败才发现冲突
    util::Result<void> begin()
    {
        if (in_transaction_)
            return {};
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
            return fail("BEGIN IMMEDIATE");
        in_transaction_ = true;
        return {};
    }

    /// COMMIT 失败时事务仍然打开 —— 调用方可重试提交或回滚，
    /// 直接把失败当成功会把未提交的写入当作已生效。
    util::Result<void> commit()
    {
        if (!in_transaction_)
            return util::Result<void>(std::unexpected(
                util::Error::make<util::Error::Kind::DbError>("commit without active transaction")));
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            return fail("COMMIT");
        in_transaction_ = false;
        return {};
    }

    util::Result<void> rollback()
    {
        if (!in_transaction_)
            return {};
        if (sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) != SQLITE_OK)
            return fail("ROLLBACK");
        in_transaction_ = false;
        return {};
    }

private:
    util::Result<void> fail(const char* statement) const
    {
        std::string message = std::string(statement) + ": " +
                              (db_ ? sqlite3_errmsg(db_) : "no connection");
        if (db_ && retryable_rc(sqlite3_extended_errcode(db_)))
            message = "SQLITE_BUSY " + message;
        return util::Result<void>(std::unexpected(
            util::Error::make<util::Error::Kind::DbError>(std::move(message))));
    }

    /// 外部进程占用（SQLITE_BUSY / SQLITE_LOCKED 及其扩展码）
    static bool retryable_rc(int rc)
    {
        return rc == SQLITE_BUSY || rc == SQLITE_LOCKED || (rc & 0xff) == SQLITE_BUSY ||
               (rc & 0xff) == SQLITE_LOCKED;
    }

    std::unique_lock<std::timed_mutex> lock_;
    sqlite3* db_{nullptr};
    bool in_transaction_{false};
};

/// 外部进程占用导致的可重试错误 —— HTTP 层据此报 503 而不是 500
inline bool retryable_db_error(const util::Error& error)
{
    return error.message().find("SQLITE_BUSY") != std::string::npos;
}

}  // namespace kforge::storage
