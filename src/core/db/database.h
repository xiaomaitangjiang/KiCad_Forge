#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

#include "core/db/db_session.h"
#include "util/error.h"

namespace kforge::storage {

/// SQLite database wrapper — public-domain sqlite3 C API.
/// No Qt or any heavy framework dependency.
/// 连接由 mutex_ 保护：一次完整操作对应一个 DbSession，
/// 仓储只接受 session 提供的连接，服务不得缓存裸 sqlite3*。
class Database {
public:
    ~Database();

    static util::Result<std::unique_ptr<Database>> open(
        const std::filesystem::path& path);

    /// 取一次独占数据库操作（不可重入：同一线程不可嵌套两个 session）
    [[nodiscard]] DbSession session() { return DbSession(db_, mutex_); }

    /// 限时取会话；被长操作占住时返回 nullopt。
    /// 心跳/状态这类不能被拖住的调用用它 —— 超时即降级，不排队。
    [[nodiscard]] std::optional<DbSession> try_session(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_, std::defer_lock);
        if (!lock.try_lock_for(timeout))
            return std::nullopt;
        return DbSession(db_, std::move(lock));
    }

    util::Result<void> execute(const std::string& sql);

    /// 一致性快照（SQLite backup API）——迁移前留存原始证据，
    /// 不能直接复制文件：WAL 里可能还有未检查点的页。
    util::Result<void> backup_to(const std::filesystem::path& path);

    /// 统一数据版本：任何写入（导入、源文件变更、删除）成功后自增。
    /// 只观察导入序号会让绑定索引在别的写入后继续用旧快照。
    [[nodiscard]] int64_t data_version() const { return data_version_.load(std::memory_order_relaxed); }
    void bump_data_version() { data_version_.fetch_add(1, std::memory_order_relaxed); }

    int schema_version() const { return schema_version_; }
    const std::filesystem::path& path() const { return path_; }

private:
    Database() = default;
    util::Result<void> run_migrations();

    sqlite3* db_{nullptr};
    std::timed_mutex mutex_;
    std::atomic<int64_t> data_version_{0};
    std::filesystem::path path_;
    int schema_version_{0};
};

}  // namespace kforge::storage
