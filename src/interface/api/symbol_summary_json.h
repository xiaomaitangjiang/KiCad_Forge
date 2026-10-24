#pragma once

#include "core/model/symbol.h"
#include "core/model/type_registry.h"
#include <nlohmann/json.hpp>

namespace kforge::api
{
inline nlohmann::json symbol_summary_json(const core::Symbol& symbol, bool locked)
{
    return {{"id", symbol.id()}, {"name", symbol.name()},
            {"library_id", symbol.library_id()}, {"value", symbol.default_value()},
            {"footprint", symbol.footprint()}, {"pins", symbol.pin_count()},
            {"mpn", symbol.mpn()}, {"type_id", symbol.component_type_id},
            {"type", core::TypeRegistry::instance().component_display(symbol.component_type_id)},
            {"locked", locked}};
}
}  // namespace kforge::api
