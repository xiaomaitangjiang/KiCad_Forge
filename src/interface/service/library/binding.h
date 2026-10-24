// Symbol-Footprint-Model binding manager
// OOP interface for callers, DOD internals (contiguous storage + hash indices)
#pragma once

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "interface/service/library/footprint_resolver.h"
#include "util/error.h"

namespace kforge::storage
{
class Database;
}

namespace kforge::core
{
struct Footprint;
struct Model3D;
struct Symbol;
}  // namespace kforge::core

namespace kforge::services
{

class SymbolBindingManager
{
public:
    explicit SymbolBindingManager(storage::Database& db);

    // ---- OOP interface ----

    // 绑定状态 = 统一 resolver 的解析结果（身份 + 存在性一起给）：
    // 名称非空、UUID 非空都不代表封装真的存在，前端只按 status 显示。
    using Binding = Resolution;

    // Query current binding state for a symbol.
    // DB 读失败 / 符号不存在都返回 Error —— 绝不能被当成「没有绑定」的正常响应。
    util::Result<Binding> get(const std::string& symbol_id) const;

    // Update the source reference and DB links; roll back DB changes if the
    // source cannot be replaced. Models are read from the selected footprint.
    util::Result<void> assign_footprint(const std::string& symbol_id,
                                        const std::string& footprint_id);

    // Assign 3D model to footprint (replaces existing link). Validates both
    // objects first and swaps the links inside a transaction — a failure
    // leaves the previous links untouched.
    util::Result<void> assign_model(const std::string& footprint_id,
                                    const std::string& model_id);

    // Search from in-memory index, no DB queries.
    // 索引构建失败必须报错：返回空数组会被前端当成「没有候选」。
    struct FootprintRef
    {
        std::string id;
        std::string name;
        std::string library;  // .pretty library name (for "Lib:Name" footprint refs)
        double score;         // similarity score (0-1) when anchor is used, else 1.0
        int pad_count;
    };
    // `query` filters by substring; when query is empty and `anchor` is given,
    // returns the Top-N most relevant candidates for that value instead of
    // the first N rows (which would otherwise be a fixed, useless batch)
    // (param named `anchor` because `near` is a Windows legacy macro)
    util::Result<std::vector<FootprintRef>> search_footprints(const std::string& query,
                                                              int limit = 20,
                                                              const std::string& anchor = "") const;

    struct ModelRef
    {
        std::string id;
        std::string name;
        std::string format;
        double score;  // similarity score (0-1) when anchor is used, else 1.0
    };
    util::Result<std::vector<ModelRef>> search_models(const std::string& query, int limit = 20,
                                                      const std::string& anchor = "") const;

    struct SymbolRef
    {
        std::string id;
        std::string name;
        double score;  // similarity score (0-1) when anchor is used, else 1.0
    };
    util::Result<std::vector<SymbolRef>> search_symbols(const std::string& query, int limit = 20,
                                                        const std::string& anchor = "") const;

private:
    // ---- DOD internals ----
    // 只借连接持有者，不缓存裸 sqlite3*：每次操作自取 session
    storage::Database* db_;

    // Contiguous storage。搜索是 const 方法、故障恢复要能重读，所以索引部分
    // 是 mutable：所有读写都在 index_mutex_ 下进行（读共享、重建独占）。
    mutable std::vector<core::Footprint> footprints_;
    mutable std::vector<core::Model3D> models_;
    mutable std::vector<core::Symbol> symbols_;
    // 读索引失败的记录：搜索时原样上报，不静默当空目录。
    // 失败不是永久状态 —— 下次搜索会再试一次（数据库没变也要能恢复）。
    mutable std::optional<util::Error> index_error_;

    // Hash indices: name/stem → offset into the vector（同受 index_mutex_ 保护）
    mutable std::unordered_map<std::string, size_t> fp_by_name_;
    mutable std::unordered_map<std::string, size_t> model_by_stem_;

    // 索引读写锁：重建要独占，搜索共享。HTTP 请求线程之间不能无锁改写这些向量。
    mutable std::shared_mutex index_mutex_;

    /// 索引读失败后重新加载（没有失败时直接返回，健康索引照样复用）
    util::Result<void> ensure_index() const;
    /// 在持有 index_mutex_ 写锁的前提下整份重建；任一表读失败都不改动现有索引
    util::Result<void> load_index_locked() const;
};

}  // namespace kforge::services
