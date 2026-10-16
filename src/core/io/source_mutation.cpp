#include "core/io/source_mutation.h"

#include "core/db/database.h"
#include "core/db/db_session.h"
#include "core/db/source_key.h"
#include "core/repo/repositories.h"
#define XXH_INLINE_ALL  // 与 symbol.cpp 同一用法：头文件内联实现，不额外加链接单元
#include "third_party/xxhash.h"
#include "util/logger.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sqlite3.h>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kforge::io
{

namespace fs = std::filesystem;

namespace
{

// ---- 进程内文件锁：按规范路径共享同一把锁，取锁顺序固定（小 → 大） ----

std::mutex g_registry_mutex;
std::map<std::string, std::weak_ptr<std::mutex>>& lock_registry()
{
    static std::map<std::string, std::weak_ptr<std::mutex>> registry;
    return registry;
}

std::vector<std::shared_ptr<std::mutex>> acquire_locks(const std::vector<std::string>& keys)
{
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    std::vector<std::shared_ptr<std::mutex>> locks;
    {
        // 只在建表/查找时持登记表锁；真正的等待在登记表之外做，
        // 避免一次慢操作把所有文件操作都堵在登记表上
        std::lock_guard registry_lock(g_registry_mutex);
        for (const auto& key : sorted)
        {
            auto& slot = lock_registry()[key];
            auto lock = slot.lock();
            if (!lock)
            {
                lock = std::make_shared<std::mutex>();
                slot = lock;
            }
            locks.push_back(std::move(lock));
        }
    }
    // 已按规范路径排序 → 全局取锁顺序一致，不会互相等待
    for (auto& lock : locks)
        lock->lock();
    return locks;
}

void release_locks(std::vector<std::shared_ptr<std::mutex>>& locks)
{
    for (auto it = locks.rbegin(); it != locks.rend(); ++it)
        (*it)->unlock();
}

std::string make_operation_id()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i)
        id += hex[dist(gen)];
    return id;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return in.good() || in.eof();
}

/// 同目录唯一临时文件；调用方负责清理
bool write_temp(const fs::path& temp, const std::string& content, std::string& error)
{
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot create temp file: " + temp.string();
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out)
    {
        error = "cannot write temp file: " + temp.string();
        return false;
    }
    return true;
}

/// 平台替换：Windows 走 ReplaceFileW（保留属性、可带备份），其余回落到同卷改名
bool replace_target(const fs::path& target, const fs::path& replacement, std::string& error)
{
#ifdef _WIN32
    if (ReplaceFileW(target.wstring().c_str(), replacement.wstring().c_str(), nullptr,
                     REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
    {
        return true;
    }
    // 目标不存在或 ReplaceFileW 不支持该卷时回落到改名
#endif
    std::error_code ec;
    fs::rename(replacement, target, ec);
    if (ec)
    {
        error = "replace failed: " + ec.message();
        return false;
    }
    return true;
}

struct PlannedFile
{
    fs::path path;
    std::string key;      // 规范路径
    fs::path temp;
    fs::path backup;
    bool existed = false;
    bool remove = false;
    std::string old_digest;
    std::string new_digest;
};

std::string plan_to_json(const std::string& operation, const std::vector<PlannedFile>& files)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& file : files)
    {
        nlohmann::json item;
        item["path"] = file.path.string();
        item["temp"] = file.temp.string();
        item["backup"] = file.backup.string();
        item["existed"] = file.existed;
        item["remove"] = file.remove;
        item["old_digest"] = file.old_digest;
        item["new_digest"] = file.new_digest;
        array.push_back(std::move(item));
    }
    nlohmann::json root;
    root["operation"] = operation;
    root["files"] = std::move(array);
    return root.dump();
}

void remove_quietly(const fs::path& path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

/// 候选文件的结构自检：括号深度不能变负、收尾必须归零（跳过字符串与转义）。
/// 只对 KiCad S-expression 文件生效 —— 写坏源文件比写不进去严重得多。
bool structurally_sane(const fs::path& path, const std::string& text)
{
    auto extension = path.extension().string();
    if (extension.rfind(".kicad", 0) != 0)
        return true;
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (in_string)
        {
            if (c == '\\')
            {
                ++i;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '(')
            ++depth;
        else if (c == ')')
        {
            --depth;
            if (depth < 0)
                return false;
        }
    }
    return depth == 0 && !in_string;
}

}  // namespace

std::string content_digest(const std::string& bytes)
{
    XXH64_hash_t hash = XXH64(bytes.data(), bytes.size(), 0);
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(hash));
    return buffer;
}

util::Result<void> SourceMutationService::apply(
    const std::vector<FileChange>& changes,
    const std::function<util::Result<void>(storage::DbSession&)>& business)
{
    using Kind = util::Error::Kind;
    if (changes.empty())
        return {};

    // ---- 1. 取锁（排序，避免互相等待）----
    std::vector<std::string> keys;
    keys.reserve(changes.size());
    for (const auto& change : changes)
    {
        auto key = storage::source_key(change.path);
        if (key.empty())
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Cannot normalize source path: " + change.path.string()));
        keys.push_back(key);
    }
    auto locks = acquire_locks(keys);
    auto unlock = [&]
    {
        release_locks(locks);
    };

    const std::string operation = make_operation_id();
    std::vector<PlannedFile> planned;
    planned.reserve(changes.size());

    // ---- 2. 锁内复核：已登记来源、存在性、摘要 ----
    {
        auto session = db_->session();
        storage::LibraryRepository libraries(session);
        storage::FootprintRepository footprints(session);
        storage::ComponentLibraryRepository components(session);
        for (std::size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            PlannedFile file;
            file.path = change.path;
            file.key = keys[i];
            file.remove = change.remove;
            file.temp = change.path;
            file.temp += "." + operation + ".tmp";
            file.backup = change.path;
            file.backup += "." + operation + ".bak";

            std::error_code ec;
            file.existed = fs::is_regular_file(change.path, ec) && !ec;
            if (file.existed)
            {
                std::string original;
                if (!read_file(change.path, original))
                {
                    unlock();
                    return std::unexpected(util::Error::make<Kind::IoError>(
                        "Cannot read source: " + change.path.string()));
                }
                file.old_digest = content_digest(original);
                if (!change.expected_digest.empty() &&
                    change.expected_digest != file.old_digest)
                {
                    unlock();
                    return std::unexpected(util::Error::make<Kind::ValidationError>(
                        "Source changed since it was read: " + change.path.string()));
                }
            }
            else if (!change.allow_missing)
            {
                unlock();
                return std::unexpected(
                    util::Error::make<Kind::NotFound>("Source missing: " + change.path.string()));
            }

            // 已登记来源（模块库/封装库都在册）+ 未锁定
            bool registered = false;
            if (auto lib = libraries.find_by_source_key(file.key); lib)
            {
                registered = true;
                if (!lib->component_library_id.empty())
                {
                    if (auto all = components.find_all())
                    {
                        for (const auto& entry : *all)
                        {
                            if (entry.id == lib->component_library_id && (entry.locked || entry.is_protected))
                            {
                                unlock();
                                return std::unexpected(util::Error::make<Kind::ValidationError>(
                                    "Source belongs to a locked library: " +
                                    change.path.string()));
                            }
                        }
                    }
                }
            }
            if (!registered)
            {
                if (auto fp = footprints.find_by_source_key(file.key); fp)
                    registered = true;
            }
            if (!registered && file.existed && !change.allow_missing)
            {
                unlock();
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Source is not a registered writable source: " + change.path.string()));
            }

            planned.push_back(std::move(file));
        }
    }

    // ---- 3. 写临时文件与备份 ----
    auto cleanup_temp_backups = [&]
    {
        for (auto& file : planned)
        {
            remove_quietly(file.temp);
            remove_quietly(file.backup);
        }
    };

    std::string error;
    for (std::size_t i = 0; i < planned.size(); ++i)
    {
        auto& file = planned[i];
        if (!file.remove)
        {
            if (!file.existed)
            {
                // 新文件的目录（如某个 .pretty）由本次操作按需建立
                std::error_code error;
                fs::create_directories(file.path.parent_path(), error);
            }
            if (!structurally_sane(file.path, changes[i].content))
            {
                error = "candidate content is malformed: " + file.path.string();
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::ValidationError>(error));
            }
            if (!write_temp(file.temp, changes[i].content, error))
            {
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(error));
            }
            file.new_digest = content_digest(changes[i].content);
        }
        if (file.existed)
        {
            std::error_code ec;
            fs::copy_file(file.path, file.backup, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(
                    "Cannot back up source: " + file.path.string() + ": " + ec.message()));
            }
        }
    }

    // ---- 4. 短事务落 prepared 日志 ----
    {
        auto session = db_->session();
        if (auto begun = session.begin(); !begun)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(begun.error());
        }
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO file_mutations(id,state,plan_json) VALUES(?1,'prepared',?2)";
        if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(session.handle())));
        }
        std::string plan = plan_to_json(operation, planned);
        sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, plan.c_str(), -1, SQLITE_TRANSIENT);
        bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!inserted)
        {
            (void) session.rollback();
            cleanup_temp_backups();
            unlock();
            return std::unexpected(util::Error::make<Kind::DbError>("Cannot write mutation log"));
        }
        if (auto committed = session.commit(); !committed)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(committed.error());
        }
    }

    // ---- 5. 替换目标文件 ----
    std::vector<PlannedFile*> replaced;
    for (auto& file : planned)
    {
        if (file.remove)
        {
            if (file.existed)
            {
                std::error_code ec;
                fs::remove(file.path, ec);
                if (ec)
                {
                    error = "delete failed: " + ec.message();
                    break;
                }
                replaced.push_back(&file);
            }
            continue;
        }
        if (!replace_target(file.path, file.temp, error))
            break;
        replaced.push_back(&file);
    }

    // ---- 6. 业务事务 + applied，同一事务提交；失败则回滚 DB 并还原文件 ----
    auto restore_files = [&]
    {
        for (auto* file : replaced)
        {
            if (file->existed)
            {
                std::error_code ec;
                fs::copy_file(file->backup, file->path, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    kforge::util::log_error{}("Restore failed for {}: {}", file->path.string(),
                                              ec.message());
            }
            else
            {
                remove_quietly(file->path);
            }
        }
    };
    // 用调用方已有的 session —— 同一线程不能再开第二个（不可重入）
    auto drop_log = [&](storage::DbSession& session)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(), "DELETE FROM file_mutations WHERE id = ?1", -1,
                               &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    };
    auto drop_log_short = [&]
    {
        auto session = db_->session();
        drop_log(session);
    };

    if (!error.empty())
    {
        restore_files();
        drop_log_short();
        cleanup_temp_backups();
        unlock();
        return std::unexpected(util::Error::make<Kind::IoError>(error));
    }

    {
        auto session = db_->session();
        if (auto begun = session.begin(); !begun)
        {
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(begun.error());
        }
        if (auto done = business(session); !done)
        {
            (void) session.rollback();  // 先撤业务写入，再在自动提交下清日志
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(done.error());
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(),
                               "UPDATE file_mutations SET state='applied' WHERE id=?1", -1, &stmt,
                               nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (auto committed = session.commit(); !committed)
        {
            (void) session.rollback();
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(committed.error());
        }
    }

    cleanup_temp_backups();
    // 数据变了：让按版本失效的缓存（绑定索引等）跟着刷新
    db_->bump_data_version();
    unlock();
    return {};
}

RecoveryReport SourceMutationService::recover()
{
    RecoveryReport report;
    auto session = db_->session();

    struct Row
    {
        std::string id;
        std::string state;
        std::string plan;
    };
    std::vector<Row> rows;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(session.handle(),
                           "SELECT id, state, plan_json FROM file_mutations ORDER BY created_at",
                           -1, &stmt, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* plan = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            rows.push_back({id ? id : "", state ? state : "", plan ? plan : ""});
        }
    }
    sqlite3_finalize(stmt);

    for (const auto& row : rows)
    {
        nlohmann::json plan;
        try
        {
            plan = nlohmann::json::parse(row.plan);
        }
        catch (const std::exception& e)
        {
            report.conflicts++;
            report.messages.push_back("mutation " + row.id + ": unreadable plan (" + e.what() + ")");
            continue;
        }

        for (auto& item : plan.value("files", nlohmann::json::array()))
        {
            fs::path path = item.value("path", "");
            fs::path backup = item.value("backup", "");
            fs::path temp = item.value("temp", "");
            bool existed = item.value("existed", false);
            bool remove = item.value("remove", false);
            std::string new_digest = item.value("new_digest", "");

            std::string current;
            bool present = read_file(path, current);
            std::string current_digest = present ? content_digest(current) : std::string();

            if (row.state == "prepared")
            {
                // 还没走完业务事务：把文件还原回原状
                if (remove)
                {
                    if (existed && fs::exists(backup))
                    {
                        std::error_code ec;
                        fs::copy_file(backup, path, fs::copy_options::overwrite_existing, ec);
                        if (ec)
                        {
                            report.conflicts++;
                            report.messages.push_back("mutation " + row.id + ": restore failed for " +
                                                      path.string() + ": " + ec.message());
                            continue;
                        }
                        report.restored++;
                    }
                }
                else if (existed && fs::exists(backup))
                {
                    if (present && !new_digest.empty() && current_digest != new_digest)
                    {
                        report.conflicts++;
                        report.messages.push_back("mutation " + row.id + ": " + path.string() +
                                                  " was edited externally; backup kept at " +
                                                  backup.string());
                        continue;
                    }
                    std::error_code ec;
                    fs::copy_file(backup, path, fs::copy_options::overwrite_existing, ec);
                    if (ec)
                    {
                        report.conflicts++;
                        report.messages.push_back("mutation " + row.id + ": restore failed for " +
                                                  path.string() + ": " + ec.message());
                    }
                    else
                    {
                        report.restored++;
                    }
                }
                else if (!existed)
                {
                    remove_quietly(path);  // 新建到一半的文件
                    report.restored++;
                }
            }
            else if (row.state == "applied")
            {
                if (present && !new_digest.empty() && current_digest != new_digest)
                {
                    report.conflicts++;
                    report.messages.push_back("mutation " + row.id + ": " + path.string() +
                                              " differs from the recorded result; backup kept at " +
                                              backup.string());
                    continue;
                }
                report.finalized++;
            }
            remove_quietly(backup);
            remove_quietly(temp);
        }

        sqlite3_stmt* drop = nullptr;
        if (sqlite3_prepare_v2(session.handle(), "DELETE FROM file_mutations WHERE id = ?1", -1,
                               &drop, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(drop, 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(drop);
            sqlite3_finalize(drop);
        }
    }

    for (const auto& message : report.messages)
        kforge::util::log_error{}("Source recovery: {}", message);
    if (report.restored > 0 || report.finalized > 0 || report.conflicts > 0)
        kforge::util::log_info{}("Source recovery: {} restored, {} finalized, {} conflicts",
                                 report.restored, report.finalized, report.conflicts);
    return report;
}

}  // namespace kforge::io
