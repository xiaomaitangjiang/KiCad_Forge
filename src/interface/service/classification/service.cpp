#include "interface/service/classification/service.h"

#include "core/db/database.h"
#include "core/model/type_registry.h"
#include "core/repo/repositories.h"
#include "classifier/rule_engine.h"
#include "classifier/rule_loader.h"
#include "util/logger.h"

#include <unordered_set>

namespace kforge::services {

ClassificationService::ClassificationService(storage::Database* db) : db_(db) {}

util::Result<ClassificationService::Summary> ClassificationService::classify_all() {
    auto rules = classifier::RuleLoader::default_rules();
    classifier::RuleEngine engine(rules);

    // 读取一次快照后立刻释放数据库边界；分类计算不占锁
    std::vector<core::Symbol> syms;
    {
        auto session = db_->session();
        auto loaded = storage::SymbolRepository(session).find_all();
        if (!loaded) {
            return Summary{};
        }
        syms = std::move(*loaded);
    }

    // Classification only updates the DB metadata (component_type) and never
    // touches source files, so it is NOT blocked by locked libraries.
    Summary summary;
    summary.total = static_cast<int>(syms.size());

    // 写回按批提交：每批一个短事务，避免上万行更新长期占住数据库边界
    constexpr std::size_t kBatchSize = 500;
    std::vector<core::Symbol> pending;
    pending.reserve(kBatchSize);

    auto flush = [&] {
        if (pending.empty()) {
            return;
        }
        auto session = db_->session();
        auto begun = session.begin();
        if (!begun) {
            kforge::util::log_error{}("Classification batch: {}", begun.error().format_message());
            pending.clear();
            return;
        }
        storage::SymbolRepository repo(session);
        int updated = 0;
        for (auto& sym : pending) {
            if (repo.update(sym)) {
                updated++;
            }
        }
        if (auto committed = session.commit(); !committed) {
            kforge::util::log_error{}("Classification batch: {}",
                                      committed.error().format_message());
            return;
        }
        summary.type_updated += updated;
        pending.clear();
    };

    for (auto& sym : syms) {
        auto results = engine.evaluate_symbol(sym);
        if (results.empty()) {
            continue;
        }
        summary.matched++;
        auto& best = results[0];

        // 分类结果落到类型 ID 上；推断不出内置类型时不写，保持原值
        auto inferred = best.inferred_type;
        if (inferred == core::ComponentType::Unknown) {
            inferred = classifier::guess_type_from_name(sym.name());
        }
        auto type_id = std::string(core::builtin_component_id(inferred));
        if (inferred != core::ComponentType::Unknown) {
            sym.component_type_id = type_id;
            pending.push_back(sym);
            if (pending.size() >= kBatchSize) {
                flush();
            }
        }

        Result r;
        r.symbol_name = sym.name();
        r.target_library = best.target_library;
        r.type_id = type_id;
        r.type_name = core::TypeRegistry::instance().component_display(type_id);
        r.confidence = best.confidence;
        summary.results.push_back(r);
    }
    flush();

    return summary;
}

util::Result<std::vector<ClassificationService::Result>>
ClassificationService::classify_symbol(const std::string& symbol_id) {
    auto rules = classifier::RuleLoader::default_rules();
    classifier::RuleEngine engine(rules);
    auto session = db_->session();
    storage::SymbolRepository repo(session);

    auto sym_result = repo.find_by_id(symbol_id);
    if (!sym_result) return std::unexpected(sym_result.error());

    std::vector<Result> results;
    auto engine_results = engine.evaluate_symbol(*sym_result);
    for (auto& best : engine_results) {
        Result r;
        r.symbol_name = sym_result->name();
        r.target_library = best.target_library;
        r.type_id = core::builtin_component_id(best.inferred_type);
        r.type_name = core::TypeRegistry::instance().component_display(r.type_id);
        r.confidence = best.confidence;
        results.push_back(r);
    }
    return results;
}

}  // namespace kforge::services
