#include "interface/service/correspondence/service.h"

#include <optional>

#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "correspondence/checker.h"

namespace kforge::services {

CorrespondenceService::CorrespondenceService(storage::Database* db) : db_(db) {}

util::Result<std::vector<correspondence::Issue>> CorrespondenceService::check_all() {
    std::vector<core::Symbol> syms;
    std::vector<core::Footprint> fps;
    std::unordered_map<core::Uuid, core::Uuid> sym_to_fp;
    std::unordered_map<core::Uuid, std::vector<core::Uuid>> fp_to_models;

    {
        auto session = db_->session();  // 四次查询读同一快照，不被同进程写入穿插
        storage::SymbolRepository sr(session);
        storage::FootprintRepository fr(session);
        storage::RelationshipRepository rr(session);

        auto loaded_syms = sr.find_all_for_check();
        auto loaded_fps = fr.find_all_for_index();
        // 批量取关联（替代每行点查）
        auto loaded_sym_to_fp = rr.find_all_symbol_links();
        auto loaded_fp_to_models = rr.find_all_model_links();
        if (!loaded_syms || !loaded_fps || !loaded_sym_to_fp || !loaded_fp_to_models)
            return std::vector<correspondence::Issue>{};

        syms = std::move(*loaded_syms);
        fps = std::move(*loaded_fps);
        sym_to_fp = std::move(*loaded_sym_to_fp);
        fp_to_models = std::move(*loaded_fp_to_models);
    }  // 检查是纯 CPU 计算，先放掉 DB 会话，别让启动导入等它算完

    std::vector<core::Model3D> models;  // Models come from relationship tables
    return correspondence::CorrespondenceChecker::check(
        syms, fps, models, sym_to_fp, fp_to_models);
}

util::Result<std::vector<correspondence::MatchSuggestion>>
CorrespondenceService::suggest_matches() {
    // Cap to avoid O(n*m) explosion. 截断放在 SQL 里：旧实现先 find_all()
    // 把 22K 符号 + 15K 封装（含属性 JSON / 焊盘）全部解析，再取前 2000/1000，
    // 既慢又长时间占着 DB 会话。判分只需要身份、名字、封装引用、引脚/焊盘数。
    constexpr size_t MAX_SYMS = 2000;
    constexpr size_t MAX_FPS = 1000;

    std::vector<core::Symbol> syms;
    std::vector<core::Footprint> fps;
    {
        auto session = db_->session();
        storage::SymbolRepository sr(session);
        storage::FootprintRepository fr(session);

        auto loaded_syms = sr.find_for_matching(MAX_SYMS);
        auto loaded_fps = fr.find_for_matching(MAX_FPS);
        if (!loaded_syms || !loaded_fps)
            return std::vector<correspondence::MatchSuggestion>{};
        syms = std::move(*loaded_syms);
        fps = std::move(*loaded_fps);
    }  // 判分是纯 CPU，先放掉 DB 会话

    return correspondence::CorrespondenceChecker::suggest_matches(syms, fps);
}

util::Result<std::vector<correspondence::MatchSuggestion>>
CorrespondenceService::suggest_matches_for(const core::Uuid& symbol_id, size_t top_n) {
    std::optional<core::Symbol> sym;
    std::vector<core::Footprint> fps;
    {
        auto session = db_->session();
        storage::SymbolRepository sr(session);
        storage::FootprintRepository fr(session);

        auto loaded_sym = sr.find_by_id(symbol_id);
        if (!loaded_sym) return std::unexpected(loaded_sym.error());
        // Single symbol × all footprints is O(F) — no cap, so footprints beyond
        // the batch cap can still be suggested. 判分仍只需要轻量字段。
        auto loaded_fps = fr.find_for_matching(0);
        if (!loaded_fps) return std::unexpected(loaded_fps.error());

        sym = std::move(*loaded_sym);
        fps = std::move(*loaded_fps);
    }  // 纯 CPU 之前放掉 DB 会话

    return correspondence::CorrespondenceChecker::suggest_for_symbol(*sym, fps, top_n);
}

}  // namespace kforge::services
