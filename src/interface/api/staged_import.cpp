#include "interface/api/staged_import.h"

#include "core/db/database.h"
#include "core/db/source_key.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include "interface/service/library/source_policy.h"
#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <sqlite3.h>

namespace kforge::api
{

namespace fs = std::filesystem;
using Kind = util::Error::Kind;

namespace
{

std::string make_identifier()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<int> dist(0, 15);
    static const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i) id += hex[dist(gen)];
    return id;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

util::Error invalid(std::string message)
{
    return util::Error::make<Kind::ValidationError>(std::move(message));
}

/// 清单里的相对路径必须真的落在暂存目录内（拒绝路径穿越、链接、junction）
util::Result<fs::path> resolve_staged_path(const fs::path& staging, const std::string& relative)
{
    if (relative.empty())
        return std::unexpected(invalid("Empty file path in manifest"));
    if (relative.find("..") != std::string::npos)
        return std::unexpected(invalid("Manifest path escapes staging dir: " + relative));
    fs::path candidate = staging / relative;
    std::error_code error;
    if (!fs::is_regular_file(candidate, error) || error)
        return std::unexpected(invalid("Staged file is not a regular file: " + relative));
    auto canonical = fs::weakly_canonical(candidate, error);
    auto root = fs::weakly_canonical(staging, error);
    if (error || !storage::path_belongs_to(canonical, root))
        return std::unexpected(invalid("Staged file resolves outside staging dir: " + relative));
    return canonical;
}

/// 合并：把插件产出的 (symbol ...) 节点原样并进目标库，保留双方全部图形。
/// 同名符号**替换**而不是追加 —— 同源更新重跑一次不能堆出重复节点。
std::string merge_symbol_nodes(const std::string& target_text,
                               const std::vector<std::pair<std::string, std::string>>& nodes,
                               bool& created_new)
{
    std::string merged = target_text;
    if (merged.empty())
    {
        merged = "(kicad_symbol_lib\n  (version 20231120)\n  (generator KiCad_Forge)\n)\n";
        created_new = true;
    }

    for (const auto& [name, node] : nodes)
    {
        if (auto start = sexpr::find_node(merged, "symbol", name))
        {
            auto end = sexpr::find_matching_paren(merged, *start);
            if (end == std::string_view::npos)
                return {};
            merged = sexpr::remove_node(merged, *start, end + 1);
        }
    }

    auto last_paren = merged.find_last_of(')');
    if (last_paren == std::string::npos)
        return {};
    std::string insertion;
    for (const auto& [name, node] : nodes)
    {
        insertion += "\n";
        insertion += node;
        insertion += "\n";
    }
    merged.insert(last_paren, insertion);
    return merged;
}

}  // namespace

util::Result<nlohmann::json> StagedImporter::run(const Request& request)
{
    if (request.external_id.empty())
        return std::unexpected(invalid("Missing external id"));

    // ---- 1. 解析目标库：必须是已登记、可写的用户库 ----
    std::string target_symbol_path;
    std::string target_footprint_root;
    std::string target_library_id;
    {
        auto session = db_->session();
        storage::LibraryRepository libraries(session);
        core::LibraryMeta target;
        bool found = false;
        if (!request.target_library_id.empty())
        {
            if (auto row = libraries.find_all())
            {
                for (const auto& candidate : *row)
                {
                    if (candidate.id == request.target_library_id)
                    {
                        target = candidate;
                        found = true;
                        break;
                    }
                }
            }
        }
        else if (!request.target_library_path.empty())
        {
            // 旧协议只给路径：必须唯一解析到已登记库
            auto key = storage::source_key(request.target_library_path);
            if (auto row = libraries.find_by_source_key(key))
            {
                target = *row;
                found = true;
            }
        }
        if (!found)
            return std::unexpected(invalid("Target library is not registered"));

        auto groups = storage::ComponentLibraryRepository(session).find_all();
        if (!groups)
            return std::unexpected(groups.error());
        bool writable = false;
        for (const auto& group : *groups)
        {
            if (group.id != target.component_library_id)
                continue;
            // 正向确认：目录已登记且未被锁定/保护
            writable = !group.locked && !group.is_protected && !group.symbol_path.empty() &&
                       !group.footprint_path.empty();
            target_footprint_root = group.footprint_path;
            break;
        }
        if (!writable)
            return std::unexpected(invalid("Target library is locked, protected or incomplete"));

        target_symbol_path = target.file_path.string();
        target_library_id = target.id;
        auto normalized = services::normalized_source_path(target_symbol_path).generic_string();
        if (normalized.contains("/share/kicad"))
            return std::unexpected(invalid("Target library lives in an official KiCad directory"));
    }

    // reuse 只接受已登记的封装 UUID
    core::Footprint reused;
    if (request.footprint_mode == "reuse")
    {
        if (request.footprint_id.empty())
            return std::unexpected(invalid("reuse requires a footprint id"));
        auto session = db_->session();
        auto found = storage::FootprintRepository(session).find_by_id(request.footprint_id);
        if (!found)
            return std::unexpected(invalid("Footprint not found: " + request.footprint_id));
        reused = *found;
    }

    // ---- 2. 幂等：同 ID 同内容返回原结果，同 ID 不同内容拒绝 ----
    const std::string request_digest = io::content_digest(
        request.plugin_id + "|" + request.action + "|" + request.provider + "|" +
        request.external_id + "|" + target_library_id + "|" + request.footprint_mode + "|" +
        request.footprint_id);
    const std::string operation_id =
        request.operation_id.empty() ? make_identifier() : request.operation_id;
    {
        auto session = db_->session();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(),
                               "SELECT request_digest, result_json FROM import_operations "
                               "WHERE id = ?1",
                               -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* digest = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string stored_digest = digest ? digest : "";
                std::string stored_result = result ? result : "{}";
                sqlite3_finalize(stmt);
                if (stored_digest != request_digest)
                    return std::unexpected(invalid("Operation id reused with different request"));
                auto json = nlohmann::json::parse(stored_result);
                json["replayed"] = true;
                return json;
            }
        }
        sqlite3_finalize(stmt);
    }

    // ---- 3. 私有暂存目录 + 插件执行（不持有数据库会话与源文件锁）----
    // 暂存目录放在数据库旁边：与数据同一卷，测试里也跟着临时库走。
    // 无论成功失败都用 RAII 清掉，不留一次性目录
    fs::path staging = db_->path().parent_path() / "staging" / operation_id;
    struct StagingScope
    {
        fs::path path;
        ~StagingScope()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } staging_scope{staging};
    {
        std::error_code error;
        fs::remove_all(staging, error);
        fs::create_directories(staging, error);
        if (error)
            return std::unexpected(util::Error::make<Kind::IoError>(
                "Cannot create staging dir: " + error.message()));
    }

    nlohmann::json args;
    args["lcsc_id"] = request.external_id;
    args["source"] = request.external_id;
    args["staging_dir"] = staging.string();
    args["protocol"] = "staged-import-v1";
    args["footprint_choice"] = request.footprint_mode == "reuse"
                                   ? nlohmann::json{{"mode", "reuse"}}
                                   : nlohmann::json{{"mode", "new"}};

    auto executed = plugins_->execute(request.plugin_id, request.action, args.dump());
    if (!executed)
        return std::unexpected(executed.error());
    nlohmann::json plugin_result;
    try
    {
        plugin_result = nlohmann::json::parse(*executed);
    }
    catch (const std::exception& error)
    {
        return std::unexpected(invalid(std::string("Plugin output is not JSON: ") + error.what()));
    }
    if (!plugin_result.value("ok", false))
    {
        return std::unexpected(invalid("Plugin failed: " +
                                       plugin_result.value("error", std::string("unknown"))));
    }

    // ---- 4. 校验清单与产物 ----
    std::string manifest_text;
    if (!read_file(staging / "manifest.json", manifest_text))
        return std::unexpected(invalid("Plugin produced no manifest.json"));
    nlohmann::json manifest;
    try
    {
        manifest = nlohmann::json::parse(manifest_text);
    }
    catch (const std::exception& error)
    {
        return std::unexpected(invalid(std::string("Manifest is not JSON: ") + error.what()));
    }
    if (manifest.value("protocol", "") != "staged-import-v1")
        return std::unexpected(invalid("Manifest protocol mismatch"));

    struct StagedSymbol
    {
        std::string name;
        std::vector<std::string> nodes;
    };
    std::vector<StagedSymbol> symbols;
    std::string staged_digest_source;
    auto entries = manifest.value("symbols", nlohmann::json::array());
    if (!entries.is_array() || entries.empty() || entries.size() > 20)
        return std::unexpected(invalid("Manifest must list 1..20 symbols"));
    for (const auto& entry : entries)
    {
        auto name = entry.value("name", "");
        auto file = entry.value("file", "");
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos)
            return std::unexpected(invalid("Invalid symbol name in manifest"));
        auto path = resolve_staged_path(staging, file);
        if (!path)
            return std::unexpected(path.error());
        if (path->extension() != ".kicad_sym")
            return std::unexpected(invalid("Symbol file must be .kicad_sym"));
        auto parsed = parser::SymbolLibParser::parse(*path);
        if (!parsed)
            return std::unexpected(invalid("Staged symbol library does not parse: " + file));
        std::string text;
        read_file(*path, text);
        staged_digest_source += text;
        auto nodes = sexpr::top_level_nodes(text, "symbol");
        // 一个清单项对应文件里唯一一个符号节点，且名字必须对得上：
        // 后面的 Footprint 属性改写就是按这个名字定位的
        if (nodes.size() != 1 || !sexpr::find_node(text, "symbol", name))
            return std::unexpected(invalid("Staged file must contain exactly the declared symbol: " +
                                           file));
        StagedSymbol staged;
        staged.name = name;
        staged.nodes.emplace_back(nodes.front());
        symbols.push_back(std::move(staged));
    }

    std::string staged_footprint_content;
    std::string staged_footprint_name;
    if (manifest.contains("footprint") && manifest["footprint"].is_object())
    {
        auto footprint = manifest["footprint"];
        staged_footprint_name = footprint.value("name", "");
        auto path = resolve_staged_path(staging, footprint.value("file", ""));
        if (!path)
            return std::unexpected(path.error());
        if (path->extension() != ".kicad_mod")
            return std::unexpected(invalid("Footprint file must be .kicad_mod"));
        auto parsed = parser::FootprintParser::parse(*path);
        if (!parsed)
            return std::unexpected(invalid("Staged footprint does not parse"));
        read_file(*path, staged_footprint_content);
        staged_digest_source += staged_footprint_content;
    }
    if (request.footprint_mode == "reuse" && !staged_footprint_content.empty())
        return std::unexpected(invalid("reuse must not produce a new footprint"));
    if (request.footprint_mode == "new" && staged_footprint_content.empty())
        return std::unexpected(invalid("new footprint mode requires a .kicad_mod"));

    const std::string content_digest = io::content_digest(staged_digest_source);

    // ---- 5. 先看来源映射：同源同内容 = no-op ----
    {
        auto session = db_->session();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(),
                               "SELECT symbol_id, symbol_name, content_digest FROM import_sources "
                               "WHERE target_library_id=?1 AND provider=?2 AND external_id=?3",
                               -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, target_library_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, request.provider.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, request.external_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                const char* digest = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                std::string previous_digest = digest ? digest : "";
                if (previous_digest == content_digest)
                {
                    nlohmann::json result;
                    result["ok"] = true;
                    result["noop"] = true;
                    result["symbol_id"] = id ? id : "";
                    result["symbol_name"] = name ? name : "";
                    result["lcsc_id"] = request.external_id;
                    sqlite3_finalize(stmt);
                    return result;
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    // ---- 6. 目标文件内容：合并符号 + 决定封装落点（宿主说了算）----
    std::string target_text;
    std::error_code exists_error;
    bool target_exists = fs::is_regular_file(target_symbol_path, exists_error) && !exists_error;
    if (target_exists)
        read_file(target_symbol_path, target_text);

    std::string library_stem = fs::path(target_symbol_path).stem().string();
    std::string footprint_reference;
    fs::path final_footprint_path;
    if (request.footprint_mode == "reuse")
    {
        auto library_name = fs::path(reused.library_path()).parent_path().stem().string();
        footprint_reference = library_name + ":" + reused.name();
    }
    else
    {
        fs::path pretty = fs::path(target_footprint_root) / (library_stem + ".pretty");
        final_footprint_path = pretty / (staged_footprint_name + ".kicad_mod");
        footprint_reference = library_stem + ":" + staged_footprint_name;
    }

    std::vector<std::pair<std::string, std::string>> nodes;
    for (auto& staged : symbols)
    {
        for (auto& node : staged.nodes)
        {
            std::string text = node;
            sexpr::replace_property_value(text, "symbol", staged.name, "Footprint",
                                          footprint_reference);
            nodes.emplace_back(staged.name, std::move(text));
        }
    }
    bool created_new = false;
    std::string merged = merge_symbol_nodes(target_text, nodes, created_new);
    if (merged.empty())
        return std::unexpected(invalid("Target library file is malformed"));

    // ---- 7. 一次变更提交全部文件与数据库改动 ----
    std::vector<io::FileChange> changes;
    changes.push_back(io::FileChange{.path = target_symbol_path,
                                     .content = merged,
                                     .expected_digest = target_exists ? io::content_digest(target_text)
                                                                      : std::string(),
                                     .allow_missing = !target_exists,
                                     .owner = ""});
    if (!final_footprint_path.empty())
    {
        changes.push_back(io::FileChange{.path = final_footprint_path,
                                         .content = staged_footprint_content,
                                         .allow_missing = true});
    }

    std::string symbol_id;
    std::string footprint_id = request.footprint_mode == "reuse" ? request.footprint_id : "";
    std::string symbol_name = symbols.front().name;

    io::SourceMutationService mutations(*db_);
    auto applied = mutations.apply(
        changes, [&](storage::DbSession& session) -> util::Result<void>
        {
            // 目标库行（新建的 .kicad_sym 在这里登记）
            core::LibraryMeta meta;
            meta.name = library_stem;
            meta.file_path = target_symbol_path;
            meta.component_library_id = [&]
            {
                auto session_libs = storage::LibraryRepository(session).find_all();
                if (session_libs)
                    for (const auto& row : *session_libs)
                        if (row.id == target_library_id) return row.component_library_id;
                return std::string{};
            }();
            meta.description = "Imported from " + request.provider;
            auto library = storage::LibraryRepository(session).upsert_by_source_key(meta);
            if (!library) return std::unexpected(library.error());

            if (!final_footprint_path.empty())
            {
                // 身份是来源路径：同一个 .kicad_mod 二次导入要复用原行，
                // 不能靠 INSERT OR IGNORE（它被忽略后会返回一个并不存在的 id）
                storage::FootprintRepository footprints_repo(session);
                auto key = storage::existing_source_key(final_footprint_path);
                core::Footprint footprint;
                footprint.set_name(staged_footprint_name);
                footprint.set_library_path(final_footprint_path.string());
                footprint.set_source_key(key);
                footprint.set_footprint_lib_name(library_stem);
                footprint.set_description("Imported from " + request.provider);
                if (auto existing = footprints_repo.find_by_source_key(key))
                {
                    footprint.set_id(existing->id());
                    if (auto updated = footprints_repo.update(footprint); !updated)
                        return std::unexpected(updated.error());
                    footprint_id = existing->id();
                }
                else
                {
                    auto inserted = footprints_repo.insert(footprint);
                    if (!inserted) return std::unexpected(inserted.error());
                    footprint_id = inserted->id();
                }
            }
            if (footprint_id.empty())
                return std::unexpected(invalid("No footprint resolved for the symbol"));

            storage::SymbolRepository symbols_repo(session);
            core::Symbol row;
            bool existing_symbol = false;
            if (auto rows = symbols_repo.find_by_library(library->id))
            {
                for (const auto& candidate : *rows)
                {
                    if (candidate.name() == symbol_name)
                    {
                        row = candidate;
                        existing_symbol = true;
                        break;
                    }
                }
            }
            row.set_name(symbol_name);
            row.set_library_id(library->id);
            row.set_footprint(footprint_reference);
            if (row.component_type_id.empty())
                row.component_type_id = "Unknown";
            if (!row.Kicad_Forge_ID().empty())
                row.set_Kicad_Forge_ID(core::Symbol::compute_hash(row));
            if (existing_symbol)
            {
                // 同名符号若已属于别的来源，不能悄悄改写 —— 报冲突让用户决定
                sqlite3_stmt* owner_stmt = nullptr;
                if (sqlite3_prepare_v2(session.handle(),
                                       "SELECT external_id FROM import_sources "
                                       "WHERE target_library_id=?1 AND provider=?2 AND symbol_id=?3",
                                       -1, &owner_stmt, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text(owner_stmt, 1, target_library_id.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(owner_stmt, 2, request.provider.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(owner_stmt, 3, row.id().c_str(), -1, SQLITE_TRANSIENT);
                    bool conflict = false;
                    if (sqlite3_step(owner_stmt) == SQLITE_ROW)
                    {
                        const char* owner =
                            reinterpret_cast<const char*>(sqlite3_column_text(owner_stmt, 0));
                        conflict = owner && request.external_id != owner;
                    }
                    sqlite3_finalize(owner_stmt);
                    if (conflict)
                        return std::unexpected(invalid(
                            "Symbol name already imported from a different source: " + symbol_name));
                }
                if (auto updated = symbols_repo.update(row); !updated)
                    return std::unexpected(updated.error());
            }
            else
            {
                auto inserted = symbols_repo.insert(row);
                if (!inserted) return std::unexpected(inserted.error());
                row = *inserted;
            }
            symbol_id = row.id();

            storage::RelationshipRepository links(session);
            if (auto old = links.find_footprint_for_symbol(symbol_id); old && *old)
            {
                if (auto unlinked = links.unlink_symbol_footprint(symbol_id, **old); !unlinked)
                    return std::unexpected(unlinked.error());
            }
            if (auto linked = links.link_symbol_to_footprint(symbol_id, footprint_id, "imported");
                !linked)
                return std::unexpected(linked.error());

            // 来源映射：同源后续更新认这一条
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "INSERT INTO import_sources(id,target_library_id,provider,external_id,symbol_id,"
                "symbol_name,content_digest) VALUES(?1,?2,?3,?4,?5,?6,?7) "
                "ON CONFLICT(target_library_id,provider,external_id) DO UPDATE SET "
                "symbol_id=excluded.symbol_id, symbol_name=excluded.symbol_name, "
                "content_digest=excluded.content_digest, updated_at=datetime('now')";
            if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
                return std::unexpected(
                    util::Error::make<Kind::DbError>(sqlite3_errmsg(session.handle())));
            std::string mapping_id = make_identifier();
            sqlite3_bind_text(stmt, 1, mapping_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, target_library_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, request.provider.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, request.external_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, symbol_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, symbol_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, content_digest.c_str(), -1, SQLITE_TRANSIENT);
            bool mapped = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
            if (!mapped)
                return std::unexpected(
                    util::Error::make<Kind::DbError>("Cannot record import source"));
            return {};
        });
    if (!applied)
        return std::unexpected(applied.error());

    // ---- 8. 记录幂等结果并清理暂存 ----
    nlohmann::json result;
    result["ok"] = true;
    result["operation_id"] = operation_id;
    result["lcsc_id"] = request.external_id;
    result["symbol_id"] = symbol_id;
    result["symbol_name"] = symbol_name;
    result["footprint_id"] = footprint_id;
    result["footprint_reference"] = footprint_reference;
    result["mode"] = request.footprint_mode;
    result["created_library"] = created_new;
    {
        auto session = db_->session();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(),
                               "INSERT INTO import_operations(id,request_digest,result_json) "
                               "VALUES(?1,?2,?3)",
                               -1, &stmt, nullptr) == SQLITE_OK)
        {
            std::string payload = result.dump();
            sqlite3_bind_text(stmt, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, request_digest.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return result;  // 暂存目录由 StagingScope 清理
}

}  // namespace kforge::api
