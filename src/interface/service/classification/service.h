#pragma once

#include <string>
#include <vector>

#include "util/error.h"
#include "classifier/rule.h"
#include "core/model/symbol.h"

namespace kforge::storage { class Database; }

namespace kforge::services {

/// Runs classification rules against database symbols and writes results back.
class ClassificationService {
public:
    explicit ClassificationService(storage::Database* db);

    struct Result {
        std::string symbol_name;
        std::string target_library;
        std::string type_id;    // 类型身份
        std::string type_name;  // 展示名
        int confidence = 0;
    };

    struct Summary {
        int total = 0;
        int matched = 0;
        int type_updated = 0;
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
