#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/error.h"
#include "core/model/symbol.h"
#include "core/model/footprint.h"
#include "core/model/types.h"

namespace kforge::storage { class Database; }
namespace kforge::classifier { class TypeMatchingStore; }

namespace kforge::services {

/// File-level library operations — delete, merge, patch, stats.
/// Import logic has been moved to ImportPipeline; bridge wrappers kept for compatibility.
class LibraryService {
public:
    explicit LibraryService(storage::Database* db,
                            classifier::TypeMatchingStore* matching_store = nullptr);

    // -- Import bridges (delegate to ImportPipeline) --
    // 旧整数形状保留：pipeline 报错（坏配置/健康失败/库写失败）一律返回 unexpected，
    // 只有确实完整成功的部分才折算成整数返回；无法用整数承载的告警走业务 warn 日志。
    util::Result<int> import_symbol_library(const std::filesystem::path& path,
                                             const std::string& component_library_id = "");
    util::Result<int> import_footprint(const std::filesystem::path& path);
    struct ImportStats
    {
        int symbols = 0, footprints = 0, models_3d = 0;
        int skipped = 0;
        int evaluation_limited = 0, evaluation_errors = 0;
        /// 真实错误事件数：完整 uint64 计数，不受 messages 的有限明细上限影响
        std::uint64_t errors = 0;
        /// 真实告警事件数
        std::uint64_t warnings = 0;
        /// 因失败而没有提交的来源文件数（与错误事件数不是一回事）
        std::uint64_t failed_files = 0;
        /// errors + warnings 的有限明细（错误在前，合计最多 ImportPipeline::kMaxReportMessages 条）；
        /// 完整数量看 errors / warnings，这里只是可读上下文
        std::vector<std::string> messages;
        bool ok() const { return errors == 0; }
    };
    util::Result<ImportStats> import_directory(const std::filesystem::path& dir,
                                               const std::string& component_library_id = "");
    util::Result<int> scan_3d_models(const std::filesystem::path& dir);
    // -- Core operations --
    util::Result<int> merge_into_library(const std::filesystem::path& source_sym,
                                          const std::filesystem::path& target_sym);
    struct DeleteLibResult { int symbols_removed; std::string deleted_file; };
    util::Result<DeleteLibResult> delete_library(const core::Uuid& lib_id);
    util::Result<void> delete_symbol(const core::Uuid& sym_id);
    int patch_kf_id_to_file(const std::filesystem::path& sym_file,
                            const std::unordered_map<std::string, std::string>& name_to_kf_id);
    int symbol_count() const;
    int footprint_count() const;
    int model_3d_count() const;

private:
    storage::Database* db_;
    classifier::TypeMatchingStore* matching_store_;
};

}  // namespace kforge::services
