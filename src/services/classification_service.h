#pragma once

#include <string>
#include <vector>

#include "util/result.h"
#include "classifier/rule.h"
#include "core/symbol.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// Runs classification rules against database symbols and writes results back.
class ClassificationService {
public:
    explicit ClassificationService(storage::Database* db);

    struct Result {
        std::string symbol_name;
        std::string target_library;
        std::string type_name;
        int confidence;
    };

    struct Summary {
        int total;
        int matched;
        int type_updated;
        std::vector<Result> results;
    };

    /// Classify all symbols in the database using default rules.
    /// Writes inferred types back to the database.
    util::Result<Summary> classify_all();

    /// Classify a single symbol by ID.
    util::Result<std::vector<Result>> classify_symbol(const std::string& symbol_id);

private:
    storage::Database* db_;
};

}  // namespace kforge::services
