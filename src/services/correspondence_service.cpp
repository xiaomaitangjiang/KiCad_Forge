#include "services/correspondence_service.h"

#include "storage/database.h"
#include "storage/repositories.h"
#include "correspondence/checker.h"

namespace kforge::services {

CorrespondenceService::CorrespondenceService(storage::Database* db) : db_(db) {}

util::Result<std::vector<correspondence::Issue>> CorrespondenceService::check_all() {
    storage::SymbolRepository sr(db_->handle());
    storage::FootprintRepository fr(db_->handle());
    storage::RelationshipRepository rr(db_->handle());

    auto syms = sr.find_all();
    auto fps = fr.find_all();
    if (!syms || !fps) return std::vector<correspondence::Issue>{};

    // Build relationship maps from relationship tables
    std::unordered_map<core::Uuid, core::Uuid> sym_to_fp;
    std::unordered_map<core::Uuid, std::vector<core::Uuid>> fp_to_models;

    // For each symbol, look up its linked footprint
    for (auto& sym : *syms) {
        auto fp_id = rr.find_footprint_for_symbol(sym.id());
        if (fp_id && *fp_id) {
            sym_to_fp[sym.id()] = **fp_id;
        }
    }

    // For each footprint, look up linked 3D models
    for (auto& fp : *fps) {
        auto models = rr.find_models_for_footprint(fp.id());
        if (models) {
            fp_to_models[fp.id()] = *models;
        }
    }

    std::vector<core::Model3D> models;  // Models come from relationship tables
    return correspondence::CorrespondenceChecker::check(
        *syms, *fps, models, sym_to_fp, fp_to_models);
}

util::Result<std::vector<correspondence::MatchSuggestion>>
CorrespondenceService::suggest_matches() {
    storage::SymbolRepository sr(db_->handle());
    storage::FootprintRepository fr(db_->handle());

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

util::Result<int> CorrespondenceService::auto_link() {
    storage::RelationshipRepository rr(db_->handle());
    auto suggestions = suggest_matches();
    if (!suggestions) return 0;

    int linked = 0;
    const double MIN_SCORE = 0.7;
    for (auto& sug : *suggestions) {
        if (sug.score >= MIN_SCORE) {
            auto result = rr.link_symbol_to_footprint(
                sug.symbol_id, sug.footprint_id, "heuristic", sug.score);
            if (result) linked++;
        }
    }
    return linked;
}

}  // namespace kforge::services
