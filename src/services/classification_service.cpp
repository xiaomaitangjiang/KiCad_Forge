#include "services/classification_service.h"

#include "storage/database.h"
#include "storage/repositories.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"

namespace kforge::services {

ClassificationService::ClassificationService(storage::Database* db) : db_(db) {}

util::Result<ClassificationService::Summary> ClassificationService::classify_all() {
    auto rules = classifier::RuleLoader::default_rules();
    classifier::RuleEngine engine(rules);
    storage::SymbolRepository repo(db_->handle());

    auto syms = repo.find_all();
    Summary summary;
    if (!syms) {
        summary.total = 0;
        return summary;
    }

    summary.total = (int)syms->size();

    for (auto& sym : *syms) {
        auto results = engine.evaluate_symbol(sym);
        if (!results.empty()) {
            summary.matched++;
            auto& best = results[0];

            auto inferred = best.inferred_type;
            if (inferred == core::ComponentType::Unknown) {
                inferred = classifier::guess_type_from_name(sym.name());
            }
            if (inferred != core::ComponentType::Unknown) {
                sym.component_type = inferred;
                if (repo.update(sym)) summary.type_updated++;
            }

            Result r;
            r.symbol_name = sym.name();
            r.target_library = best.target_library;
            r.type_name = classifier::component_type_to_string(inferred);
            r.confidence = best.confidence;
            summary.results.push_back(r);
        }
    }

    return summary;
}

util::Result<std::vector<ClassificationService::Result>>
ClassificationService::classify_symbol(const std::string& symbol_id) {
    auto rules = classifier::RuleLoader::default_rules();
    classifier::RuleEngine engine(rules);
    storage::SymbolRepository repo(db_->handle());

    auto sym_result = repo.find_by_id(symbol_id);
    if (!sym_result) return std::unexpected(sym_result.error());

    std::vector<Result> results;
    auto engine_results = engine.evaluate_symbol(*sym_result);
    for (auto& best : engine_results) {
        Result r;
        r.symbol_name = sym_result->name();
        r.target_library = best.target_library;
        r.type_name = classifier::component_type_to_string(best.inferred_type);
        r.confidence = best.confidence;
        results.push_back(r);
    }
    return results;
}

}  // namespace kforge::services
