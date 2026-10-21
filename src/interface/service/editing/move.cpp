#include "move.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/db/db_session.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include "interface/service/editing/history.h"
#include "interface/service/editing/node_util.h"
#include "interface/service/editing/paste.h"

namespace kforge::services
{
namespace
{
using util::Error;
using util::Result;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 稳定错误码：消息统一为 "code: detail"，HTTP 层据此映射，不解析自由文本。
// ---------------------------------------------------------------------------

Error invalid_request(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("invalid_request: " + std::move(detail));
}

Error target_not_found(std::string detail)
{
    return Error::make<Error::Kind::NotFound>("target_not_found: " + std::move(detail));
}

Error target_not_writable(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("target_not_writable: " +
                                                     std::move(detail));
}

Error symbol_invalid(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("symbol_invalid: " + std::move(detail));
}

Error conflict(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("conflict: " + std::move(detail));
}

Error source_changed(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("source_changed: " + std::move(detail));
}

Error operation_conflict(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("operation_conflict: " +
                                                     std::move(detail));
}

Error commit_failed(std::string detail)
{
    return Error::make<Error::Kind::ServiceError>("commit_failed: " + std::move(detail));
}

Error invalid_token(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("invalid_token: " + std::move(detail));
}

const std::vector<std::string_view>& known_codes()
{
    static const std::vector<std::string_view> codes{
        "invalid_request",   "target_not_found", "target_not_writable", "symbol_invalid",
        "conflict",          "source_changed",   "operation_conflict",  "commit_failed",
        "invalid_token",      "cut_failed",       "payload_too_large",
        "history_empty",      "history_conflict", "history_full",        "history_failed",
    };
    return codes;
}

std::string code_of(const Error& error)
{
    const std::string& message = error.message();
    const auto colon = message.find(':');
    if (colon == std::string::npos)
        return {};
    const std::string_view head(message.data(), colon);
    for (const auto code : known_codes())
    {
        if (head == code)
            return std::string(head);
    }
    return {};
}

std::string fallback_code(const Error& error, std::string_view generic)
{
    switch (error.kind())
    {
        case Error::Kind::NotFound:
            return "target_not_found";
        case Error::Kind::ValidationError:
            return "invalid_request";
        case Error::Kind::ParseError:
        case Error::Kind::ServiceError:
            return "symbol_invalid";
        case Error::Kind::DbError:
        case Error::Kind::IoError:
        case Error::Kind::InternalError:
            return "commit_failed";
        default:
            return std::string(generic);
    }
}

// ---------------------------------------------------------------------------
// 进程内剪切意图与移动幂等缓存。两者都只保证单进程生命周期。
// ---------------------------------------------------------------------------

struct CutSource
{
    std::string uuid;
    std::string name;
    std::string library_id;
    std::string source_path;
    std::string source_digest;
};

struct CutIntent
{
    std::vector<CutSource> sources;
    std::vector<ExportedSymbol> items;
    std::chrono::steady_clock::time_point created;
};

std::mutex g_cut_mutex;
std::map<std::string, CutIntent, std::less<>> g_cut_intents;
std::deque<std::string> g_cut_order;
std::chrono::steady_clock::duration g_cut_clock_offset{};

struct MoveIdempotencyEntry
{
    std::string digest;
    MoveOutcome outcome;
};

std::map<std::string, MoveIdempotencyEntry, std::less<>> g_move_idempotency;
std::deque<std::string> g_move_order;

std::chrono::steady_clock::time_point cut_now()
{
    return std::chrono::steady_clock::now() + g_cut_clock_offset;
}

void prune_cut_order_locked()
{
    while (!g_cut_order.empty() &&
           g_cut_intents.find(g_cut_order.front()) == g_cut_intents.end())
        g_cut_order.pop_front();
}

void purge_expired_cut_locked()
{
    const auto now = cut_now();
    for (auto it = g_cut_intents.begin(); it != g_cut_intents.end();)
    {
        if (now - it->second.created > kCutTokenTtl)
            it = g_cut_intents.erase(it);
        else
            ++it;
    }
    prune_cut_order_locked();
}

std::string make_cut_token()
{
    std::random_device device;
    std::uint64_t seed = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    seed ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::uint64_t> distribution;
    char buffer[33];
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                  static_cast<unsigned long long>(distribution(rng)),
                  static_cast<unsigned long long>(distribution(rng)));
    return std::string(buffer, 32);
}

std::string register_cut_intent(std::vector<CutSource> sources,
                                const std::vector<ExportedSymbol>& items)
{
    std::lock_guard lock(g_cut_mutex);
    purge_expired_cut_locked();
    prune_cut_order_locked();
    while (g_cut_intents.size() >= kMaxCutIntents)
    {
        if (g_cut_order.empty())
            break;
        g_cut_intents.erase(g_cut_order.front());
        g_cut_order.pop_front();
        prune_cut_order_locked();
    }

    std::string token = make_cut_token();
    while (g_cut_intents.find(token) != g_cut_intents.end())
        token = make_cut_token();

    CutIntent intent;
    intent.sources = std::move(sources);
    intent.items = items;
    intent.created = cut_now();
    g_cut_intents.emplace(token, std::move(intent));
    g_cut_order.push_back(token);
    return token;
}

std::optional<CutIntent> load_cut_intent(std::string_view token)
{
    std::lock_guard lock(g_cut_mutex);
    const auto it = g_cut_intents.find(token);
    if (it == g_cut_intents.end())
        return std::nullopt;
    if (cut_now() - it->second.created > kCutTokenTtl)
    {
        g_cut_intents.erase(it);
        prune_cut_order_locked();
        return std::nullopt;
    }
    return it->second;
}

bool consume_cut_token(std::string_view token)
{
    std::lock_guard lock(g_cut_mutex);
    const auto it = g_cut_intents.find(token);
    if (it == g_cut_intents.end())
        return false;
    if (cut_now() - it->second.created > kCutTokenTtl)
    {
        g_cut_intents.erase(it);
        prune_cut_order_locked();
        return false;
    }
    g_cut_intents.erase(it);
    prune_cut_order_locked();
    return true;
}

std::string canonical_move_digest(const MoveRequest& request)
{
    nlohmann::json payload;
    payload["token"] = request.token;
    payload["target_library_id"] = request.target_library_id;
    payload["renames"] = request.renames;
    return kforge::io::content_digest(payload.dump());
}

void remember_move_idempotent(const std::string& operation_id, std::string digest,
                              const MoveOutcome& outcome)
{
    while (g_move_idempotency.size() >= kMaxMoveIdempotencyEntries)
    {
        if (g_move_order.empty())
            break;
        g_move_idempotency.erase(g_move_order.front());
        g_move_order.pop_front();
    }
    MoveIdempotencyEntry entry;
    entry.digest = std::move(digest);
    entry.outcome = outcome;
    g_move_idempotency.emplace(operation_id, std::move(entry));
    g_move_order.push_back(operation_id);
}

Result<void> validate_move_request(const MoveRequest& request)
{
    if (request.token.empty() || request.token.size() > 128)
        return std::unexpected(invalid_request("token must be 1..128 characters"));
    if (request.target_library_id.empty() || request.target_library_id.size() > 64)
        return std::unexpected(invalid_request("target_library_id must be 1..64 characters"));
    if (request.operation_id.empty() ||
        request.operation_id.size() > kMaxMoveOperationIdLength)
        return std::unexpected(invalid_request(
            "operation_id must be 1.." + std::to_string(kMaxMoveOperationIdLength) +
            " characters"));
    if (!request.target_digest.empty() && !is_digest_text(request.target_digest))
        return std::unexpected(invalid_request("target_digest must be 16 hex characters or empty"));
    for (const auto& [source_name, new_name] : request.renames)
    {
        std::string why;
        if (!is_plain_name(source_name, why))
            return std::unexpected(invalid_request("rename source is invalid: " + why));
        if (!is_plain_name(new_name, why))
            return std::unexpected(invalid_request("rename target is invalid: " + why));
    }
    return {};
}

// ---------------------------------------------------------------------------
// 库 / 源对象只读校验
// ---------------------------------------------------------------------------

std::optional<core::LibraryMeta> find_library(const std::vector<core::LibraryMeta>& libraries,
                                              std::string_view id)
{
    for (const auto& library : libraries)
    {
        if (library.id == id)
            return library;
    }
    return std::nullopt;
}

Result<void> ensure_library_writable(const storage::DbSession& session,
                                     const core::LibraryMeta& library)
{
    if (library.file_path.empty())
        return std::unexpected(target_not_writable("Library \"" + library.name +
                                                   "\" has no symbol file path"));
    if (lower_copy(to_utf8(library.file_path.extension())) != ".kicad_sym")
        return std::unexpected(target_not_writable("Library \"" + library.name +
                                                   "\" does not point to a .kicad_sym file"));

    if (!library.component_library_id.empty())
    {
        storage::ComponentLibraryRepository owners(session);
        auto all = owners.find_all();
        if (!all)
            return std::unexpected(all.error());
        const auto owner = std::find_if(
            all->begin(), all->end(),
            [&](const auto& row) { return row.id == library.component_library_id; });
        if (owner == all->end())
            return std::unexpected(target_not_writable(
                "Library \"" + library.name + "\" belongs to a missing component library"));
        if (!owner->enabled)
            return std::unexpected(target_not_writable("Component library \"" + owner->name +
                                                       "\" is disabled"));
        if (owner->locked || owner->is_protected)
            return std::unexpected(target_not_writable("Component library \"" + owner->name +
                                                       "\" is locked or protected"));
    }

    auto authorized = kforge::io::authorize_source_write(
        session, library.file_path, library.component_library_id);
    if (!authorized)
    {
        if (authorized.error().kind() == Error::Kind::DbError)
            return std::unexpected(authorized.error());
        return std::unexpected(target_not_writable(
            "Library \"" + library.name + "\" is not writable: " +
            authorized.error().format_message()));
    }
    return {};
}

struct TargetState
{
    core::LibraryMeta library;
    fs::path path;
    std::string path_text;
    std::string owner_id;
    FileState file;
};

Result<TargetState> load_target(const storage::DbSession& session, std::string_view library_id,
                                std::vector<std::string>& warnings)
{
    storage::LibraryRepository libraries(session);
    auto all = libraries.find_all();
    if (!all)
        return std::unexpected(all.error());
    const auto found = find_library(*all, library_id);
    if (!found)
        return std::unexpected(
            target_not_found("Target library \"" + std::string(library_id) + "\" is not registered"));

    TargetState state;
    state.library = *found;
    state.path = state.library.file_path;
    state.path_text = to_utf8(state.path);
    state.owner_id = state.library.component_library_id;

    auto writable = ensure_library_writable(session, state.library);
    if (!writable)
        return std::unexpected(writable.error());

    auto file = read_library_file(state.path, "Target library");
    if (!file)
        return std::unexpected(target_not_writable(
            "Target library \"" + state.library.name + "\" cannot be used: " +
            file.error().format_message()));
    state.file = std::move(*file);
    if (!state.file.exists)
        warnings.push_back("Target file does not exist yet and will be created: " +
                           state.path_text);
    return state;
}

Result<void> verify_source(const storage::DbSession& session,
                           const std::vector<core::LibraryMeta>& libraries,
                           const CutSource& source, std::string* lib_id = nullptr)
{
    const auto library = find_library(libraries, source.library_id);
    if (!library)
        return std::unexpected(source_changed("Source library \"" + source.library_id +
                                              "\" is no longer registered"));
    auto writable = ensure_library_writable(session, *library);
    if (!writable)
        return std::unexpected(writable.error());

    storage::SymbolRepository symbols(session);
    auto symbol = symbols.find_by_id(source.uuid);
    if (!symbol)
        return std::unexpected(source_changed("Symbol \"" + source.name +
                                              "\" no longer exists"));
    if (symbol->name() != source.name || symbol->library_id() != source.library_id)
        return std::unexpected(source_changed("Symbol \"" + source.name +
                                              "\" changed library or name since the cut"));
    if (lib_id != nullptr)
        *lib_id = symbol->lib_id();
    return {};
}

// ---------------------------------------------------------------------------
// 移动分析：dry_run 与提交共用同一套只读判定。
// ---------------------------------------------------------------------------

struct DirectItem
{
    const CutSource* source{nullptr};
    const ExportedSymbol* item{nullptr};
    std::string final_name;
    std::string parent;
    // 库归属的显示身份（历史与 lib_id 恢复用）：旧/新值都来自本次移动。
    std::string old_lib_id;
    std::string new_lib_id;
};

struct MoveAnalysis
{
    MovePlan plan;
    TargetState target;
    std::vector<CutSource> sources;
    std::vector<ExportedSymbol> items;
    std::vector<DirectItem> direct;
    std::vector<std::string> target_nodes;
    std::unordered_map<std::string, FileState> source_files;
    std::unordered_map<std::string, std::string> source_owner;
    std::unordered_map<std::string, std::string> source_lib_id;
    std::unordered_set<std::string> target_db_names;
};

std::string item_key(std::string_view path, std::string_view name)
{
    return std::string(path) + "\n" + std::string(name);
}

std::string path_extension(const fs::path& path)
{
    return lower_copy(to_utf8(path.extension()));
}

Result<MoveAnalysis> analyze_move(storage::Database& db, const MoveRequest& request,
                                  const CutIntent& intent)
{
    MoveAnalysis analysis;
    analysis.plan.target.library_id = request.target_library_id;

    if (intent.sources.empty())
        return std::unexpected(invalid_token("Cut token has no movable symbols"));
    for (const auto& item : intent.items)
    {
        if (item.uuid.empty() && !item.dependency)
            return std::unexpected(invalid_token("Cut token contains an item without identity"));
        if (item.source_path.empty() || item.name.empty())
            return std::unexpected(invalid_token("Cut token contains an incomplete item"));
    }

    analysis.sources = intent.sources;
    analysis.items = intent.items;

    std::unordered_map<std::string, std::size_t> item_by_key;
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
        item_by_key.emplace(item_key(analysis.items[i].source_path, analysis.items[i].name), i);

    std::unordered_set<std::string> direct_names;
    for (const auto& source : analysis.sources)
    {
        if (!direct_names.insert(source.name).second)
            return std::unexpected(invalid_request("Duplicate symbol name in cut: " + source.name));
        const auto it = item_by_key.find(item_key(source.source_path, source.name));
        if (it == item_by_key.end() || analysis.items[it->second].dependency)
            return std::unexpected(invalid_token("Cut token is missing symbol \"" +
                                                 source.name + "\""));
    }

    // 1) DB 层复核：目标、源对象、源库写权限、目标库已有名字。
    {
        auto session = db.session();
        storage::LibraryRepository libraries(session);
        auto all = libraries.find_all();
        if (!all)
            return std::unexpected(all.error());

        auto target = load_target(session, request.target_library_id, analysis.plan.warnings);
        if (!target)
            return std::unexpected(target.error());
        analysis.target = std::move(*target);
        analysis.plan.target.name = analysis.target.library.name;
        analysis.plan.target.file_path = analysis.target.path_text;
        analysis.plan.target.digest = analysis.target.file.digest;

        storage::SymbolRepository symbols(session);
        auto summaries = symbols.find_summaries("", analysis.target.library.id);
        if (!summaries)
            return std::unexpected(summaries.error());
        for (const auto& symbol : *summaries)
            analysis.target_db_names.insert(symbol.name());

        for (const auto& source : analysis.sources)
        {
            std::string old_lib_id;
            auto verified = verify_source(session, *all, source, &old_lib_id);
            if (!verified)
                return std::unexpected(verified.error());
            analysis.source_lib_id.emplace(source.uuid, std::move(old_lib_id));
            const auto library = find_library(*all, source.library_id);
            const std::string owner = library ? library->component_library_id : std::string{};
            const auto [it, inserted] =
                analysis.source_owner.emplace(source.source_path, owner);
            if (!inserted && it->second != owner)
                return std::unexpected(source_changed(
                    "Source file belongs to more than one component library: " +
                    source.source_path));
        }

        // 2) 读源文件并在会话外比较摘要；文件读取失败一律视为源变化。
        for (const auto& source : analysis.sources)
        {
            if (analysis.source_files.find(source.source_path) != analysis.source_files.end())
                continue;
            auto file = read_library_file(fs::u8path(source.source_path), "Source library");
            if (!file)
                return std::unexpected(source_changed(
                    "Source file \"" + source.source_path + "\" cannot be read: " +
                    file.error().format_message()));
            analysis.source_files.emplace(source.source_path, std::move(*file));
        }
        for (const auto& source : analysis.sources)
        {
            const auto& file = analysis.source_files.at(source.source_path);
            if (upper_copy(file.digest) != upper_copy(source.source_digest))
                return std::unexpected(source_changed(
                    "Source file changed since the cut: " + source.source_path));
            if (std::find(file.symbol_names.begin(), file.symbol_names.end(), source.name) ==
                file.symbol_names.end())
                return std::unexpected(source_changed("Symbol \"" + source.name +
                                                      "\" disappeared from " +
                                                      source.source_path));
        }
    }

    // 同一文件移动：不删也不加，直接 noop。
    bool all_in_target = true;
    for (const auto& source : analysis.sources)
    {
        if (source.source_path != analysis.target.path_text)
        {
            all_in_target = false;
            break;
        }
    }
    if (all_in_target)
    {
        analysis.plan.noop = true;
        return analysis;
    }

    bool any_in_target = false;
    for (const auto& source : analysis.sources)
    {
        if (source.source_path == analysis.target.path_text)
        {
            any_in_target = true;
            analysis.plan.conflicts.push_back({source.name, "already_in_target"});
        }
    }

    // 3) 直接项、父依赖与改名。
    std::unordered_map<std::string, std::size_t> direct_by_name;
    for (const auto& source : analysis.sources)
    {
        DirectItem item;
        item.source = &source;
        const auto key = item_key(source.source_path, source.name);
        item.item = &analysis.items.at(item_by_key.at(key));
        item.final_name = source.name;
        const auto lib_id = analysis.source_lib_id.find(source.uuid);
        if (lib_id != analysis.source_lib_id.end())
            item.old_lib_id = lib_id->second;
        analysis.direct.push_back(item);
        direct_by_name.emplace(source.name, analysis.direct.size() - 1);
    }

    for (auto& item : analysis.direct)
    {
        auto parent = symbol_extends_of(item.item->sexpr, item.item->name);
        if (!parent)
            return std::unexpected(symbol_invalid("Symbol \"" + item.item->name +
                                                  "\" cannot be parsed: " +
                                                  parent.error().format_message()));
        if (parent->has_value())
            item.parent = **parent;
        if (item.parent == item.source->name)
            return std::unexpected(symbol_invalid("Symbol \"" + item.source->name +
                                                  "\" extends itself"));
    }

    for (const auto& [from, to] : request.renames)
    {
        if (direct_by_name.find(from) == direct_by_name.end())
            analysis.plan.warnings.push_back("Ignoring rename for \"" + from +
                                             "\": it is not a directly moved symbol");
    }
    for (auto& item : analysis.direct)
    {
        const auto it = request.renames.find(item.source->name);
        if (it != request.renames.end())
            item.final_name = it->second;
        // 库归属显示身份按既有规则是 "库名:符号名"；跨库改名必须一起更新，
        // 不能只改 name/library_id 让 lib_id 继续指向旧库旧名（R8）。
        item.new_lib_id = analysis.target.library.name + ":" + item.final_name;
    }

    {
        std::unordered_set<std::string> final_names;
        for (const auto& item : analysis.direct)
        {
            if (!final_names.insert(item.final_name).second)
                return std::unexpected(invalid_request("Rename would create duplicate symbol \"" +
                                                       item.final_name + "\""));
        }
    }

    std::unordered_set<std::string> target_file_names;
    for (const auto& name : analysis.target.file.symbol_names)
        target_file_names.insert(name);

    // 冲突判定同时看目标文件顶层名与目标库 DB 行，不能依赖 INSERT OR IGNORE 静默去重。
    for (const auto& item : analysis.direct)
    {
        if (target_file_names.find(item.final_name) != target_file_names.end() ||
            analysis.target_db_names.find(item.final_name) != analysis.target_db_names.end())
            analysis.plan.conflicts.push_back({item.source->name, "name_conflict"});
    }

    // 4) 源文件里若有未移动的派生符号仍指向即将离开的父符号，整体拒绝：
    //    逐行修补会变成隐式改内容，不是用户确认的移动。
    std::unordered_set<std::string> moved_names;
    for (const auto& item : analysis.direct)
        moved_names.insert(item.source->name);
    for (const auto& [path, file] : analysis.source_files)
    {
        for (const auto& [child, parent] : file.parent_of)
        {
            if (parent.empty())
                continue;
            if (moved_names.find(parent) != moved_names.end() &&
                moved_names.find(child) == moved_names.end())
                analysis.plan.conflicts.push_back({child, "unmoved_derivatives"});
        }
    }

    // moves 在预检里即使有冲突也返回，前端能看到改名计划；提交时 conflicts 非空会拒绝。
    for (const auto& item : analysis.direct)
        analysis.plan.moves.push_back(
            {item.source->uuid, item.source->name, item.final_name});

    if (!analysis.plan.conflicts.empty())
        return analysis;

    // 5) 直接项拓扑序：父在前。父可能在本次直接项里（跨文件移动的常见情况）。
    std::vector<std::size_t> order;
    std::vector<int> state(analysis.direct.size(), 0);  // 0 未访问 / 1 栈内 / 2 完成
    std::function<Result<void>(std::size_t)> visit = [&](std::size_t index) -> Result<void> {
        if (state[index] == 1)
            return std::unexpected(symbol_invalid("Symbol inheritance cycle at \"" +
                                                  analysis.direct[index].source->name + "\""));
        if (state[index] == 2)
            return {};
        state[index] = 1;
        const auto parent = analysis.direct[index].parent;
        if (!parent.empty())
        {
            const auto found = direct_by_name.find(parent);
            if (found != direct_by_name.end())
            {
                auto child = visit(found->second);
                if (!child)
                    return child;
            }
        }
        state[index] = 2;
        order.push_back(index);
        return {};
    };
    for (std::size_t i = 0; i < analysis.direct.size(); ++i)
    {
        auto visited = visit(i);
        if (!visited)
            return std::unexpected(visited.error());
    }

    // 6) 目标节点：依赖父先写，直接项按拓扑序写；不建 DB 行的父项只随文件走。
    std::unordered_set<std::string> direct_final_names;
    for (const auto& item : analysis.direct)
        direct_final_names.insert(item.final_name);

    std::unordered_set<std::string> added_dependencies;
    std::unordered_set<std::string> warned_parents;
    for (const std::size_t index : order)
    {
        const DirectItem& item = analysis.direct[index];
        std::vector<std::string> chain;
        std::unordered_set<std::string> chain_seen;
        std::string parent = item.parent;
        while (!parent.empty())
        {
            if (direct_by_name.find(parent) != direct_by_name.end())
                break;
            if (target_file_names.find(parent) != target_file_names.end())
            {
                if (warned_parents.insert(parent).second)
                    analysis.plan.warnings.push_back("Reusing parent \"" + parent +
                                                     "\" already present in target");
                break;
            }
            const auto parent_item = item_by_key.find(item_key(item.source->source_path, parent));
            if (parent_item == item_by_key.end())
            {
                // 目标 DB 里登记了这个名字但目标文件里没有内容，无法安全补出父节点。
                analysis.plan.conflicts.push_back({item.source->name, "missing_parent"});
                break;
            }
            if (!chain_seen.insert(parent).second)
                return std::unexpected(symbol_invalid("Symbol inheritance cycle at \"" + parent +
                                                      "\""));
            const ExportedSymbol& dependency = analysis.items[parent_item->second];
            if (!dependency.dependency)
            {
                analysis.plan.conflicts.push_back({item.source->name, "missing_parent"});
                break;
            }
            if (added_dependencies.find(parent) == added_dependencies.end())
                chain.push_back(parent);

            auto next = symbol_extends_of(dependency.sexpr, dependency.name);
            if (!next)
                return std::unexpected(symbol_invalid("Parent \"" + parent +
                                                      "\" cannot be parsed: " +
                                                      next.error().format_message()));
            parent = next->has_value() ? **next : std::string{};
        }

        std::reverse(chain.begin(), chain.end());
        for (const auto& dependency_name : chain)
        {
            if (direct_final_names.find(dependency_name) != direct_final_names.end())
            {
                analysis.plan.conflicts.push_back({dependency_name, "name_conflict"});
                continue;
            }
            const auto parent_item =
                item_by_key.find(item_key(item.source->source_path, dependency_name));
            if (parent_item == item_by_key.end())
                continue;
            analysis.target_nodes.push_back(analysis.items[parent_item->second].sexpr);
            added_dependencies.insert(dependency_name);
            analysis.plan.dependencies.push_back({dependency_name, true});
        }

        std::string final_parent = item.parent;
        if (!item.parent.empty())
        {
            const auto found = direct_by_name.find(item.parent);
            if (found != direct_by_name.end())
                final_parent = analysis.direct[found->second].final_name;
        }

        std::string node = item.item->sexpr;
        if (final_parent != item.parent || item.final_name != item.source->name)
        {
            auto rewritten = rewrite_symbol_node(node, item.source->name, item.final_name,
                                                 item.parent, final_parent, false);
            if (!rewritten)
                return std::unexpected(symbol_invalid("Symbol \"" + item.source->name +
                                                      "\" cannot be renamed: " +
                                                      rewritten.error().format_message()));
            node = std::move(*rewritten);
        }
        analysis.target_nodes.push_back(std::move(node));
    }

    if (!analysis.plan.conflicts.empty())
        return analysis;
    return analysis;
}

// ---------------------------------------------------------------------------
// 提交准备：生成目标/源文件内容，并预先验证能重新解析。
// ---------------------------------------------------------------------------

struct PreparedMove
{
    std::vector<kforge::io::FileChange> changes;
    std::vector<MovePlanItem> moved;
};

Result<std::string> build_target_content(const MoveAnalysis& analysis)
{
    const FileState& target = analysis.target.file;
    const std::string newline =
        target.exists && target.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string block;
    for (const auto& node : analysis.target_nodes)
    {
        block += "\t";
        block += node;
        block += newline;
    }

    if (!target.exists)
        return "(kicad_symbol_lib (version 20231120) (generator kforge)" + newline + block +
               ")" + newline;

    std::string out = target.text.substr(0, target.root_close);
    if (out.empty() || (out.back() != '\n' && out.back() != '\r'))
        out += newline;
    out += block;
    out += target.text.substr(target.root_close);
    return out;
}

Result<std::string> build_source_content(const FileState& file,
                                         const std::vector<std::string>& removed_names)
{
    struct Range
    {
        std::size_t begin{0};
        std::size_t end{0};
    };
    std::vector<Range> ranges;
    for (const auto& name : removed_names)
    {
        const auto open = sexpr::find_node(file.text, "symbol", name);
        if (!open.has_value())
            return std::unexpected(commit_failed("Source file no longer defines symbol \"" +
                                                 name + "\""));
        const auto close = sexpr::find_matching_paren(file.text, *open);
        if (close == std::string::npos)
            return std::unexpected(commit_failed("Source symbol \"" + name +
                                                 "\" is unbalanced"));
        std::size_t begin = *open;
        while (begin > 0 && (file.text[begin - 1] == ' ' || file.text[begin - 1] == '\t'))
            --begin;
        std::size_t end = close + 1;
        if (end < file.text.size() && file.text[end] == '\r')
            ++end;
        if (end < file.text.size() && file.text[end] == '\n')
            ++end;
        ranges.push_back({begin, end});
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const Range& lhs, const Range& rhs) { return lhs.begin < rhs.begin; });
    for (std::size_t i = 1; i < ranges.size(); ++i)
    {
        if (ranges[i].begin < ranges[i - 1].end)
            return std::unexpected(commit_failed("Source symbol ranges overlap"));
    }

    std::string out;
    out.reserve(file.text.size());
    std::size_t cursor = 0;
    for (const auto& range : ranges)
    {
        out.append(file.text.substr(cursor, range.begin - cursor));
        cursor = range.end;
    }
    out.append(file.text.substr(cursor));

    auto parsed = parser::SymbolLibParser::parse_buffer(out, "");
    if (!parsed)
        return std::unexpected(commit_failed(
            "Source file would become invalid after removing moved symbols: " +
            parsed.error().format_message()));
    for (const auto& name : removed_names)
    {
        if (sexpr::find_node(out, "symbol", name).has_value())
            return std::unexpected(commit_failed("Source symbol \"" + name +
                                                 "\" was not removed"));
    }
    return out;
}

Result<void> set_symbol_identity(const storage::DbSession& session, std::string_view id,
                                 std::string_view library_id, std::string_view lib_id)
{
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(session.handle(),
                           "UPDATE symbols SET library_id=?1, lib_id=?2 WHERE id=?3", -1, &raw,
                           nullptr) != SQLITE_OK)
        return std::unexpected(Error::make<Error::Kind::DbError>(
            std::string("prepare library_id update: ") + sqlite3_errmsg(session.handle())));
    struct Guard
    {
        sqlite3_stmt* stmt;
        ~Guard()
        {
            if (stmt != nullptr)
                sqlite3_finalize(stmt);
        }
    } guard{raw};

    if (sqlite3_bind_text(raw, 1, library_id.data(), static_cast<int>(library_id.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(raw, 2, lib_id.data(), static_cast<int>(lib_id.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(raw, 3, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK)
        return std::unexpected(Error::make<Error::Kind::DbError>(
            std::string("bind symbol identity update: ") + sqlite3_errmsg(session.handle())));
    if (sqlite3_step(raw) != SQLITE_DONE)
        return std::unexpected(Error::make<Error::Kind::DbError>(
            std::string("execute library_id update: ") + sqlite3_errmsg(session.handle())));
    if (sqlite3_changes(session.handle()) != 1)
        return std::unexpected(source_changed("Symbol \"" + std::string(id) +
                                              "\" disappeared during move"));
    return {};
}

Result<PreparedMove> prepare_move(const MoveAnalysis& analysis)
{
    PreparedMove prepared;

    auto target_content = build_target_content(analysis);
    if (!target_content)
        return std::unexpected(target_content.error());
    auto parsed = parser::SymbolLibParser::parse_buffer(*target_content, "");
    if (!parsed)
        return std::unexpected(commit_failed(
            "Target library would become invalid after the move: " +
            parsed.error().format_message()));

    kforge::io::FileChange target_change;
    target_change.path = analysis.target.path;
    target_change.content = std::move(*target_content);
    target_change.owner = analysis.target.owner_id;
    target_change.expected_digest = analysis.target.file.digest;
    target_change.allow_missing = !analysis.target.file.exists;
    target_change.expected_missing = !analysis.target.file.exists;
    prepared.changes.push_back(std::move(target_change));

    std::unordered_map<std::string, std::vector<std::string>> remove_by_path;
    for (const auto& item : analysis.direct)
        remove_by_path[item.source->source_path].push_back(item.source->name);

    for (const auto& [path, names] : remove_by_path)
    {
        const auto file_it = analysis.source_files.find(path);
        if (file_it == analysis.source_files.end())
            return std::unexpected(commit_failed("Source file state is missing: " + path));
        auto content = build_source_content(file_it->second, names);
        if (!content)
            return std::unexpected(content.error());

        kforge::io::FileChange source_change;
        source_change.path = fs::u8path(path);
        source_change.content = std::move(*content);
        source_change.expected_digest = file_it->second.digest;
        const auto owner = analysis.source_owner.find(path);
        if (owner != analysis.source_owner.end())
            source_change.owner = owner->second;
        prepared.changes.push_back(std::move(source_change));
    }

    for (const auto& item : analysis.direct)
        prepared.moved.push_back(
            {item.source->uuid, item.source->name, item.final_name});
    return prepared;
}

Error map_move_apply_error(const Error& error)
{
    const auto known = code_of(error);
    if (!known.empty())
        return error;

    const std::string& message = error.message();
    if (message.find("Source changed since it was read") != std::string::npos ||
        message.find("Source appeared since it was read") != std::string::npos ||
        message.find("Source missing:") != std::string::npos)
        return source_changed(message);
    if (message.find("candidate content is malformed") != std::string::npos)
        return commit_failed(message);
    if (error.kind() == Error::Kind::ValidationError)
        return target_not_writable(message);
    if (error.kind() == Error::Kind::NotFound)
        return target_not_found(message);
    return commit_failed(message);
}

}  // namespace

std::string cut_error_code(const util::Error& error)
{
    const auto known = code_of(error);
    return known.empty() ? fallback_code(error, "cut_failed") : known;
}

int cut_error_status(std::string_view code)
{
    if (code == "symbol_not_found")
        return 404;
    if (code == "invalid_request")
        return 400;
    if (code == "target_not_writable")
        return 403;
    return 500;
}

std::string move_error_code(const util::Error& error)
{
    const auto known = code_of(error);
    return known.empty() ? fallback_code(error, "commit_failed") : known;
}

int move_error_status(std::string_view code)
{
    if (code == "target_not_found")
        return 404;
    if (code == "target_not_writable")
        return 403;
    if (code == "invalid_request")
        return 400;
    if (code == "symbol_invalid" || code == "payload_too_large")
        return code == "payload_too_large" ? 413 : 422;
    if (code == "conflict" || code == "source_changed" || code == "operation_conflict" ||
        code == "invalid_token" || code == "history_empty" || code == "history_conflict" ||
        code == "history_full")
        return 409;
    return 500;
}

Result<CutResult> cut_symbols(storage::Database& db, const std::vector<core::Uuid>& ids)
{
    if (ids.empty() || ids.size() > kMaxCutItems)
        return std::unexpected(invalid_request(
            "Cut accepts 1.." + std::to_string(kMaxCutItems) + " symbol ids"));

    auto exported = export_symbols(db, ids);
    if (!exported)
    {
        // export 的 NotFound 在 cut 契约里叫 symbol_not_found。
        if (exported.error().kind() == Error::Kind::NotFound)
            return std::unexpected(Error::make<Error::Kind::NotFound>(
                "symbol_not_found: " + exported.error().format_message()));
        return std::unexpected(exported.error());
    }

    CutResult result;
    result.items = exported->items;
    std::vector<CutSource> sources;
    for (const auto& item : result.items)
    {
        if (item.dependency)
            continue;
        sources.push_back({item.uuid, item.name, item.library_id, item.source_path,
                           item.source_digest});
    }
    if (sources.empty())
        return std::unexpected(invalid_request("Cut selected no movable symbols"));

    {
        auto session = db.session();
        storage::LibraryRepository libraries(session);
        auto all = libraries.find_all();
        if (!all)
            return std::unexpected(all.error());
        for (const auto& source : sources)
        {
            const auto library = find_library(*all, source.library_id);
            if (!library)
                return std::unexpected(Error::make<Error::Kind::NotFound>(
                    "symbol_not_found: Library for symbol \"" + source.name +
                    "\" is not registered"));
            auto writable = ensure_library_writable(session, *library);
            if (!writable)
                return std::unexpected(writable.error());
        }
    }

    result.token = register_cut_intent(std::move(sources), result.items);
    result.summary = "Cut " + std::to_string(ids.size()) + " symbol(s); move is pending";
    if (result.items.size() > ids.size())
        result.warnings.push_back("Inherited parent symbols are included for a complete move");
    return result;
}

void clear_cut_token(std::string_view token)
{
    std::lock_guard lock(g_cut_mutex);
    g_cut_intents.erase(std::string(token));
    prune_cut_order_locked();
}

Result<MovePlan> preview_move(storage::Database& db, const MoveRequest& request)
{
    auto valid = validate_move_request(request);
    if (!valid)
        return std::unexpected(valid.error());
    if (request.dry_run == false)
        return std::unexpected(invalid_request("preview_move only accepts dry_run=true"));

    auto intent = load_cut_intent(request.token);
    if (!intent)
        return std::unexpected(invalid_token("Cut token is unknown, expired or already consumed"));

    auto analysis = analyze_move(db, request, *intent);
    if (!analysis)
        return std::unexpected(analysis.error());
    return analysis->plan;
}

Result<MoveOutcome> move_symbols(storage::Database& db, const MoveRequest& request)
{
    auto valid = validate_move_request(request);
    if (!valid)
        return std::unexpected(valid.error());
    if (request.dry_run)
        return std::unexpected(invalid_request("move requires dry_run=false; use preview for dry_run"));

    const std::string digest = canonical_move_digest(request);
    std::lock_guard lock(editing_commit_mutex());

    {
        const auto cached = g_move_idempotency.find(request.operation_id);
        if (cached != g_move_idempotency.end())
        {
            if (cached->second.digest != digest)
                return std::unexpected(operation_conflict(
                    "operation_id \"" + request.operation_id +
                    "\" was already used for a different move"));
            MoveOutcome outcome = cached->second.outcome;
            outcome.replayed = true;
            return outcome;
        }
    }

    auto intent = load_cut_intent(request.token);
    if (!intent)
        return std::unexpected(invalid_token("Cut token is unknown, expired or already consumed"));

    auto analysis = analyze_move(db, request, *intent);
    if (!analysis)
        return std::unexpected(analysis.error());
    if (!analysis->plan.conflicts.empty())
        return std::unexpected(conflict("Move has " +
                                        std::to_string(analysis->plan.conflicts.size()) +
                                        " unresolved conflict(s)"));
    if (analysis->plan.noop)
    {
        MoveOutcome outcome;
        outcome.noop = true;
        consume_cut_token(request.token);
        remember_move_idempotent(request.operation_id, digest, outcome);
        return outcome;
    }

    if (upper_copy(request.target_digest) != upper_copy(analysis->plan.target.digest))
        return std::unexpected(source_changed(
            "Target file changed since it was read; refresh the plan"));

    auto prepared = prepare_move(*analysis);
    if (!prepared)
        return std::unexpected(prepared.error());

    // 历史单元：所有被改动文件的 before/after 快照 + 每个移动对象的 DB 归属前后值。
    // 容量必须在 apply 之前预检；放不进历史预算时整体拒绝，不能先写后撤销。
    HistoryEntry history;
    history.id = new_history_entry_id();
    history.kind = "move";
    history.count = prepared->moved.size();
    history.target_library_name = analysis->target.library.name;
    if (!analysis->direct.empty())
        history.source_library_name = analysis->direct.front().item->library_name;
    history.created_at_ms = history_now_ms();

    {
        std::unordered_map<std::string, const FileState*> before;
        before.emplace(analysis->target.path_text, &analysis->target.file);
        for (const auto& [path, state] : analysis->source_files)
            before.emplace(path, &state);
        for (const auto& change : prepared->changes)
        {
            const std::string path = to_utf8(change.path);
            const auto it = before.find(path);
            if (it == before.end())
                return std::unexpected(Error::make<Error::Kind::ServiceError>(
                    "history_failed: Missing before snapshot for \"" + path + "\""));
            HistoryFileSnapshot snapshot;
            snapshot.path = path;
            snapshot.owner = change.owner;
            snapshot.exists_before = it->second->exists;
            snapshot.before_content = it->second->text;
            snapshot.exists_after = !change.remove;
            snapshot.after_content = change.remove ? std::string{} : change.content;
            history.files.push_back(std::move(snapshot));
        }
    }
    for (const auto& item : analysis->direct)
    {
        HistoryMoveRow row;
        row.uuid = item.source->uuid;
        row.old_library_id = item.source->library_id;
        row.old_name = item.source->name;
        row.old_lib_id = item.old_lib_id;
        row.new_library_id = analysis->target.library.id;
        row.new_name = item.final_name;
        row.new_lib_id = item.new_lib_id;
        row.kf_id = item.item->kf_id;
        history.moved_rows.push_back(std::move(row));
    }
    auto capacity = precheck_history_capacity(history);
    if (!capacity)
        return std::unexpected(capacity.error());

    const auto& direct = analysis->direct;
    const std::string target_library_id = analysis->target.library.id;
    auto applied = kforge::io::SourceMutationService(db).apply(
        prepared->changes, [&direct, &target_library_id](storage::DbSession& session) -> Result<void> {
            storage::SymbolRepository symbols(session);
            for (const auto& item : direct)
            {
                auto symbol = symbols.find_by_id(item.source->uuid);
                if (!symbol)
                    return std::unexpected(source_changed(
                        "Symbol \"" + item.source->name + "\" disappeared during move"));
                if (symbol->name() != item.source->name ||
                    symbol->library_id() != item.source->library_id)
                    return std::unexpected(source_changed(
                        "Symbol \"" + item.source->name +
                        "\" changed library or name during move"));
                symbol->set_name(item.final_name);
                symbol->set_library_id(target_library_id);
                auto updated = symbols.update(*symbol);
                if (!updated)
                    return std::unexpected(updated.error());
                auto moved = set_symbol_identity(session, item.source->uuid, target_library_id,
                                                 item.new_lib_id);
                if (!moved)
                    return std::unexpected(moved.error());
            }
            return {};
        });
    if (!applied)
        return std::unexpected(map_move_apply_error(applied.error()));

    push_history(std::move(history));
    consume_cut_token(request.token);

    MoveOutcome outcome;
    outcome.moved = prepared->moved;
    remember_move_idempotent(request.operation_id, digest, outcome);
    return outcome;
}

namespace testing
{
void advance_cut_clock(std::chrono::seconds delta)
{
    std::lock_guard lock(g_cut_mutex);
    g_cut_clock_offset += delta;
}

void reset_editing_state()
{
    {
        std::lock_guard lock(g_cut_mutex);
        g_cut_intents.clear();
        g_cut_order.clear();
        g_cut_clock_offset = {};
    }
    {
        std::lock_guard lock(editing_commit_mutex());
        g_move_idempotency.clear();
        g_move_order.clear();
    }
    reset_paste_state();
    reset_history();
}

std::string install_cut_intent_for_test(std::vector<ExportedSymbol> items)
{
    std::vector<CutSource> sources;
    for (const auto& item : items)
    {
        if (item.dependency)
            continue;
        sources.push_back(CutSource{item.uuid, item.name, item.library_id, item.source_path,
                                    item.source_digest});
    }
    if (sources.empty())
        return {};
    return register_cut_intent(std::move(sources), items);
}
}  // namespace testing

}  // namespace kforge::services
