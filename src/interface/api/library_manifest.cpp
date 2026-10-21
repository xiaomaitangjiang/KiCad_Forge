#include "interface/api/library_manifest.h"

#include "core/db/database.h"
#include "core/db/source_key.h"
#include "core/io/parser/footprint_parser.h"
#include "core/repo/repositories.h"
#include "interface/service/library/footprint_resolver.h"
#include "util/error.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kforge::api
{
namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

bool path_related(const fs::path& path, const fs::path& root)
{
    if (path.empty() || root.empty())
        return false;
    return storage::path_belongs_to(path, root);
}

bool path_related(const std::string& path, const fs::path& root)
{
    return !path.empty() && path_related(fs::path(path), root);
}

std::string relative_or_absolute(const fs::path& path, const fs::path& root)
{
    if (path.empty())
        return {};
    std::error_code canonical_error;
    auto canonical = fs::weakly_canonical(path, canonical_error);
    if (canonical_error)
        canonical = path;
    std::error_code base_error;
    const auto base = fs::weakly_canonical(root, base_error);
    if (!base_error)
    {
        auto relative = canonical.lexically_relative(base);
        const auto text = relative.generic_string();
        if (!text.empty() && !text.starts_with(".."))
            return text;
    }
    return canonical.generic_string();
}

json pins_json(const core::Symbol& symbol)
{
    json pins = json::array();
    for (const auto& pin : symbol.pins())
    {
        pins.push_back({{"number", pin.number},
                        {"name", pin.name},
                        {"electrical_type", pin.electrical_type}});
    }
    return pins;
}

json properties_json(const core::Symbol& symbol)
{
    json properties = json::object();
    for (const auto& [key, value] : symbol.properties())
    {
        // 仓储内部用于回读的私有列不是用户数据，清单里不列为属性。
        if (key.empty() || key.front() == '_')
            continue;
        properties[key] = value;
    }
    return properties;
}

json pads_json(const core::Footprint& footprint)
{
    json pads = json::array();
    for (const auto& pad : footprint.pads())
    {
        pads.push_back({{"number", pad.number},
                        {"type", pad.type},
                        {"shape", pad.shape},
                        {"x", pad.x},
                        {"y", pad.y},
                        {"width", pad.width},
                        {"height", pad.height},
                        {"drill", pad.drill}});
    }
    std::sort(pads.begin(), pads.end(), [](const json& left, const json& right) {
        const auto left_number = left.value("number", "");
        const auto right_number = right.value("number", "");
        if (left_number != right_number)
            return left_number < right_number;
        return left.value("x", 0.0) < right.value("x", 0.0);
    });
    return pads;
}

struct SymbolRow
{
    core::Symbol symbol;
    core::LibraryMeta library;
    std::string footprint_id;
    std::vector<std::string> model_ids;
};

struct FootprintRow
{
    core::Footprint footprint;
    json parsed_pads = json::array();
    std::string parse_error;
};

}  // namespace

json build_library_manifest(storage::Database& db,
                        const std::string& root_argument,
                        const std::string& target_argument)
{
    if (root_argument.empty())
        return {{"ok", false}, {"error", "Repository root is required"}};

    std::error_code error;
    auto root = fs::weakly_canonical(fs::path(root_argument), error);
    if (error)
        return {{"ok", false}, {"error", "Repository root does not exist"}};
    fs::path target;
    if (!target_argument.empty())
    {
        target = fs::weakly_canonical(fs::path(target_argument), error);
        if (error)
            target = fs::path(target_argument);
    }

    std::vector<SymbolRow> symbols;
    std::unordered_map<std::string, FootprintRow> footprints;
    std::unordered_map<std::string, std::string> models;
    json libraries = json::array();
    std::unordered_set<std::string> seen_libraries;

    {
        auto session = db.session();
        storage::ComponentLibraryRepository component_libraries(session);
        storage::LibraryRepository library_repository(session);
        storage::SymbolRepository symbol_repository(session);

        auto all_components = component_libraries.find_all();
        if (!all_components)
            return {{"ok", false}, {"error", all_components.error().format_message()}};
        std::unordered_set<std::string> selected_components;
        for (const auto& component : *all_components)
        {
            if (path_related(component.symbol_path, root) ||
                path_related(component.footprint_path, root) ||
                path_related(component.model_3d_path, root) ||
                (!target.empty() && (path_related(component.symbol_path, target) ||
                                     path_related(component.footprint_path, target) ||
                                     path_related(component.model_3d_path, target))))
            {
                selected_components.insert(component.id);
            }
        }

        auto all_libraries = library_repository.find_all();
        if (!all_libraries)
            return {{"ok", false}, {"error", all_libraries.error().format_message()}};
        std::vector<core::LibraryMeta> selected_libraries;
        for (const auto& library : *all_libraries)
        {
            const bool belongs =
                selected_components.contains(library.component_library_id) ||
                path_related(library.file_path, root) ||
                (!target.empty() && path_related(library.file_path, target));
            if (!belongs)
                continue;
            if (!seen_libraries.insert(library.id).second)
                continue;
            selected_libraries.push_back(library);
            libraries.push_back({{"id", library.id},
                                 {"name", library.name},
                                 {"source_key", library.source_key},
                                 {"component_library_id", library.component_library_id}});
        }

        auto loaded = services::load_snapshot_light(session);
        if (!loaded)
            return {{"ok", false}, {"error", loaded.error().format_message()}};
        const services::Snapshot& snapshot = *loaded;
        services::FileCheckContext files;

        for (const auto& library : selected_libraries)
        {
            auto rows = symbol_repository.find_by_library(library.id);
            if (!rows)
                return {{"ok", false}, {"error", rows.error().format_message()}};
            for (auto& symbol : *rows)
            {
                SymbolRow row;
                row.library = library;
                auto resolution = services::resolve_symbol(symbol, snapshot, files);
                row.footprint_id = resolution.footprint_id;
                if (!row.footprint_id.empty())
                {
                    const auto* footprint = snapshot.footprint(row.footprint_id);
                    if (footprint != nullptr)
                    {
                        auto found = footprints.find(row.footprint_id);
                        if (found == footprints.end())
                        {
                            FootprintRow footprint_row;
                            footprint_row.footprint = *footprint;
                            footprints.emplace(row.footprint_id, std::move(footprint_row));
                        }
                        if (auto links = snapshot.model_links.find(row.footprint_id);
                            links != snapshot.model_links.end())
                        {
                            for (const auto& model_id : links->second)
                            {
                                if (const auto* model = snapshot.model(model_id))
                                {
                                    models[model_id] = model->file_path().generic_string();
                                }
                            }
                        }
                    }
                }
                if (!resolution.model_id.empty())
                {
                    if (const auto* model = snapshot.model(resolution.model_id))
                        models[resolution.model_id] = model->file_path().generic_string();
                }
                row.symbol = std::move(symbol);
                symbols.push_back(std::move(row));
            }
        }
    }

    // 解析焊盘放在 session 之外：一个元件库可能有几千个封装，逐文件解析
    // 不应把数据库锁住。
    for (auto& [id, row] : footprints)
    {
        auto parsed = parser::FootprintParser::parse(row.footprint.library_path());
        if (parsed)
        {
            row.parsed_pads = pads_json(*parsed);
        }
        else
        {
            row.parse_error = parsed.error().format_message();
        }
    }

    json out;
    out["ok"] = true;
    out["root"] = root.generic_string();
    out["manifest_version"] = 1;
    out["libraries"] = std::move(libraries);

    json symbols_json = json::array();
    for (const auto& row : symbols)
    {
        const auto& symbol = row.symbol;
        json item;
        item["uuid"] = symbol.id();
        item["library_id"] = row.library.id;
        item["library_name"] = row.library.name;
        item["library_source_key"] = row.library.source_key;
        item["name"] = symbol.name();
        item["footprint_ref"] = symbol.footprint();
        item["footprint_uuid"] = row.footprint_id;
        item["kf_id"] = symbol.Kicad_Forge_ID();
        item["pre_kf_id"] = symbol.Pre_Kicad_Forge_ID();
        item["component_type_id"] = symbol.component_type_id;
        item["package_type_id"] = symbol.package_type_id;
        item["pins"] = pins_json(symbol);
        item["properties"] = properties_json(symbol);
        symbols_json.push_back(std::move(item));
    }
    std::sort(symbols_json.begin(), symbols_json.end(), [](const json& left, const json& right) {
        return left.value("uuid", "") < right.value("uuid", "");
    });
    out["symbols"] = std::move(symbols_json);

    json footprints_json = json::array();
    for (const auto& [id, row] : footprints)
    {
        json item;
        item["uuid"] = id;
        item["name"] = row.footprint.name();
        item["source_key"] = row.footprint.source_key();
        item["library"] = row.footprint.footprint_lib_name();
        item["pad_count"] = row.footprint.pad_count();
        item["pads"] = row.parsed_pads;
        if (auto links = row.footprint.properties(); !links.empty())
        {
            // 属性里可能有用户数据；封装属性与符号属性同样保留，跳过内部列。
            for (const auto& [key, value] : links)
            {
                if (key.empty() || key.front() == '_')
                    continue;
                item["properties"][key] = value;
            }
        }
        if (!row.parse_error.empty())
            item["parse_error"] = row.parse_error;
        footprints_json.push_back(std::move(item));
    }
    std::sort(footprints_json.begin(), footprints_json.end(), [](const json& left, const json& right) {
        return left.value("uuid", "") < right.value("uuid", "");
    });
    out["footprints"] = std::move(footprints_json);

    json models_json = json::array();
    for (const auto& [id, path] : models)
    {
        models_json.push_back({{"uuid", id}, {"path", relative_or_absolute(fs::path(path), root)}});
    }
    std::sort(models_json.begin(), models_json.end(), [](const json& left, const json& right) {
        return left.value("path", "") < right.value("path", "");
    });
    out["models"] = std::move(models_json);

    return out;
}

}  // namespace kforge::api