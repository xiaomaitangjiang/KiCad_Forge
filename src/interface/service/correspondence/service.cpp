#include "interface/service/correspondence/service.h"

#include "core/db/database.h"
#include "core/repo/repositories.h"
#include "correspondence/checker.h"

namespace kforge::services {

CorrespondenceService::CorrespondenceService(storage::Database* db) : db_(db) {}

util::Result<std::vector<correspondence::Issue>> CorrespondenceService::check_all() {
    auto session = db_->session();  // 三次查询读同一快照，不被同进程写入穿插
    storage::SymbolRepository sr(session);
    storage::FootprintRepository fr(session);
    storage::RelationshipRepository rr(session);

    auto syms = sr.find_all();
    auto fps = fr.find_all();
    // 批量取关联（替代每行点查），一次会话内读完，锁不跨整个检查过程
    auto sym_to_fp = rr.find_all_symbol_links();
    auto fp_to_models = rr.find_all_model_links();
    if (!syms || !fps || !sym_to_fp || !fp_to_models)
        return std::vector<correspondence::Issue>{};

    std::vector<core::Model3D> models;  // Models come from relationship tables
    return correspondence::CorrespondenceChecker::check(
        *syms, *fps, models, *sym_to_fp, *fp_to_models);
}

util::Result<std::vector<correspondence::MatchSuggestion>>
CorrespondenceService::suggest_matches() {
    auto session = db_->session();
    storage::SymbolRepository sr(session);
    storage::FootprintRepository fr(session);

    auto syms = sr.find_all();
    auto fps = fr.find_all();
    if (!syms || !fps) return std::vector<correspondence::MatchSuggestion>{};

    // Cap to avoid O(n*m) explosion (22K syms × 2K fps = 44M comparisons)
    const int MAX_SYMS = 2000;
    const int MAX_FPS = 1000;
    std::vector<core::Symbol> capped_syms;
    std::vector<core::Footprint> capped_fps;
    for (int i = 0; i < static_cast<int>(syms->size()) && i < MAX_SYMS; i++)
        capped_syms.push_back((*syms)[i]);
    for (int i = 0; i < static_cast<int>(fps->size()) && i < MAX_FPS; i++)
        capped_fps.push_back((*fps)[i]);
    return correspondence::CorrespondenceChecker::suggest_matches(capped_syms, capped_fps);
}

util::Result<std::vector<correspondence::MatchSuggestion>>
CorrespondenceService::suggest_matches_for(const core::Uuid& symbol_id, size_t top_n) {
    auto session = db_->session();
    storage::SymbolRepository sr(session);
    storage::FootprintRepository fr(session);

    auto sym = sr.find_by_id(symbol_id);
    auto fps = fr.find_all();
    if (!sym || !fps) return std::vector<correspondence::MatchSuggestion>{};

    // Single symbol × all footprints is O(F) — no cap, so footprints beyond
    // the batch cap can still be suggested
    return correspondence::CorrespondenceChecker::suggest_for_symbol(*sym, *fps, top_n);
}

}  // namespace kforge::services
