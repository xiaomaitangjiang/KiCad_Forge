#include "history.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "core/db/db_session.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"

namespace kforge::services
{
namespace
{
using util::Error;
using util::Result;

namespace fs = std::filesystem;

constexpr std::string_view kHistoryEmpty = "history_empty";
constexpr std::string_view kHistoryConflict = "history_conflict";
constexpr std::string_view kHistoryFull = "history_full";
constexpr std::string_view kHistoryFailed = "history_failed";

Error history_empty_error(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("history_empty: " + std::move(detail));
}

Error history_conflict(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("history_conflict: " + std::move(detail));
}

Error history_full(std::string detail)
{
    return Error::make<Error::Kind::ValidationError>("history_full: " + std::move(detail));
}

Error history_failed(std::string detail)
{
    return Error::make<Error::Kind::InternalError>("history_failed: " + std::move(detail));
}

bool known_code(std::string_view code)
{
    return code == kHistoryEmpty || code == kHistoryConflict || code == kHistoryFull ||
           code == kHistoryFailed;
}

std::string error_code_of(const Error& error)
{
    const std::string& message = error.message();
    const auto colon = message.find(':');
    if (colon == std::string::npos)
        return {};
    const std::string_view head(message.data(), colon);
    if (known_code(head))
        return std::string(head);
    return {};
}

// ---------------------------------------------------------------------------
// 进程内会话状态。所有读写都在 editing_commit_mutex() 下进行。
// ---------------------------------------------------------------------------

struct HistoryLimits
{
    std::size_t entries{kMaxHistoryEntries};
    std::size_t bytes{kMaxHistoryBytes};
    std::size_t file_bytes{kMaxHistoryFileBytes};
};

std::mutex g_editing_mutex;
std::vector<HistoryEntry> g_entries;
std::size_t g_cursor{0};  // 下一个可重做的下标
HistoryLimits g_limits;
std::atomic<std::uint64_t> g_next_id{1};

std::size_t estimate_symbol_bytes(const core::Symbol& symbol)
{
    // 保守估算：固定开销 + 所有字符串 + 属性/引脚条目。高于实际保存量，
    // 使容量预检不会因为估算偏低而在提交后才发现放不下。
    std::size_t bytes = 256;
    bytes += symbol.id().size() + symbol.library_id().size() + symbol.name().size() +
             symbol.lib_id().size() + symbol.reference_prefix().size() +
             symbol.default_value().size() + symbol.footprint().size() +
             symbol.datasheet().size() + symbol.description().size() + symbol.mpn().size() +
             symbol.Kicad_Forge_ID().size() + symbol.Pre_Kicad_Forge_ID().size() +
             symbol.component_type_id.size() + symbol.package_type_id.size();
    for (const auto& [key, value] : symbol.properties())
        bytes += 48 + key.size() + value.size();
    for (const auto& pin : symbol.pins())
        bytes += 96 + pin.number.size() + pin.name.size() + pin.electrical_type.size();
    if (symbol.value_range)
        bytes += 32 + symbol.value_range->unit.size();
    return bytes;
}

std::size_t estimate_move_row_bytes(const HistoryMoveRow& row)
{
    return 256 + row.uuid.size() + row.old_library_id.size() + row.old_name.size() +
           row.old_lib_id.size() + row.new_library_id.size() + row.new_name.size() +
           row.new_lib_id.size() + row.kf_id.size();
}

std::size_t history_entry_bytes(const HistoryEntry& entry)
{
    std::size_t bytes = 128 + entry.id.size() + entry.kind.size() +
                        entry.target_library_name.size() + entry.source_library_name.size();
    for (const auto& file : entry.files)
        bytes += 128 + file.path.size() + file.owner.size() + file.before_content.size() +
                 file.after_content.size();
    for (const auto& symbol : entry.created_symbols)
        bytes += estimate_symbol_bytes(symbol);
    for (const auto& row : entry.moved_rows)
        bytes += estimate_move_row_bytes(row);
    return bytes;
}

std::size_t total_history_bytes()
{
    std::size_t bytes = 0;
    for (const auto& entry : g_entries)
        bytes += history_entry_bytes(entry);
    return bytes;
}

HistoryEntrySummary summarize(const HistoryEntry& entry)
{
    HistoryEntrySummary summary;
    summary.kind = entry.kind;
    summary.count = entry.count;
    summary.target_library_name = entry.target_library_name;
    summary.source_library_name = entry.source_library_name;
    summary.created_at_ms = entry.created_at_ms;
    return summary;
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
        return std::unexpected(
            history_conflict("A moved symbol disappeared before the history operation"));
    return {};
}

Result<void> ensure_name_free(storage::SymbolRepository& symbols,
                              const std::string& library_id, const std::string& name,
                              std::string_view except_uuid)
{
    auto rows = symbols.find_by_library(library_id);
    if (!rows)
        return std::unexpected(rows.error());
    for (const auto& row : *rows)
    {
        if (row.name() == name && row.id() != except_uuid)
            return std::unexpected(history_conflict(
                "Library already contains a symbol named \"" + name +
                "\"; the history item cannot be restored without a name conflict"));
    }
    return {};
}

Result<core::Symbol> find_current(storage::SymbolRepository& symbols, std::string_view uuid,
                                  std::string_view name_for_message)
{
    auto current = symbols.find_by_id(std::string(uuid));
    if (!current)
    {
        if (current.error().kind() == Error::Kind::NotFound)
            return std::unexpected(history_conflict(
                "Symbol \"" + std::string(name_for_message) +
                "\" no longer exists; the history item cannot be restored"));
        return std::unexpected(current.error());
    }
    return *current;
}

bool same_value_range(const std::optional<core::ValueRange>& a,
                      const std::optional<core::ValueRange>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    return a->min == b->min && a->max == b->max && a->unit == b->unit;
}

bool same_symbol_row(const core::Symbol& a, const core::Symbol& b)
{
    if (a.id() != b.id() || a.library_id() != b.library_id() || a.name() != b.name() ||
        a.lib_id() != b.lib_id() || a.is_power() != b.is_power() ||
        a.reference_prefix() != b.reference_prefix() ||
        a.default_value() != b.default_value() || a.footprint() != b.footprint() ||
        a.datasheet() != b.datasheet() || a.description() != b.description() ||
        a.mpn() != b.mpn() || a.Kicad_Forge_ID() != b.Kicad_Forge_ID() ||
        a.Pre_Kicad_Forge_ID() != b.Pre_Kicad_Forge_ID() ||
        a.component_type_id != b.component_type_id ||
        a.package_type_id != b.package_type_id || a.properties() != b.properties() ||
        !same_value_range(a.value_range, b.value_range) ||
        a.pins().size() != b.pins().size())
        return false;
    for (std::size_t i = 0; i < a.pins().size(); ++i)
    {
        const auto& left = a.pins()[i];
        const auto& right = b.pins()[i];
        if (left.number != right.number || left.name != right.name ||
            left.electrical_type != right.electrical_type || left.x != right.x ||
            left.y != right.y)
            return false;
    }
    return true;
}

Result<void> restore_paste_undo(storage::DbSession& session, const HistoryEntry& entry,
                                std::vector<HistoryRestored>& restored)
{
    storage::SymbolRepository symbols(session);
    storage::RelationshipRepository relationships(session);
    for (const auto& expected : entry.created_symbols)
    {
        auto current = find_current(symbols, expected.id(), expected.name());
        if (!current)
            return std::unexpected(current.error());
        if (!same_symbol_row(*current, expected))
            return std::unexpected(history_conflict(
                "Symbol \"" + expected.name() +
                "\" was edited after the paste; undo would discard newer metadata, pins or identity"));
        // 粘贴本身不建立封装关联：之后新增的关系不能在 undo 的 CASCADE 删除里被静默带走。
        auto links = relationships.find_symbol_link_rows(expected.id());
        if (!links)
            return std::unexpected(links.error());
        if (!links->empty())
            return std::unexpected(history_conflict(
                "Symbol \"" + expected.name() +
                "\" gained footprint relationships after the paste; undo would delete them"));
        auto removed = symbols.remove(expected.id());
        if (!removed)
            return std::unexpected(removed.error());
        restored.push_back({expected.id(), expected.name()});
    }
    return {};
}

Result<void> restore_paste_redo(storage::DbSession& session, const HistoryEntry& entry,
                                std::vector<HistoryRestored>& restored)
{
    storage::SymbolRepository symbols(session);
    for (const auto& expected : entry.created_symbols)
    {
        auto existing = symbols.find_by_id(expected.id());
        if (existing)
            return std::unexpected(history_conflict(
                "Symbol \"" + expected.name() +
                "\" already exists again; redo would duplicate it"));
        if (existing.error().kind() != Error::Kind::NotFound)
            return std::unexpected(existing.error());

        auto free_name = ensure_name_free(symbols, expected.library_id(), expected.name(),
                                          expected.id());
        if (!free_name)
            return std::unexpected(free_name.error());

        auto inserted = symbols.insert(expected);
        if (!inserted)
            return std::unexpected(inserted.error());
        if (inserted->id() != expected.id())
            return std::unexpected(history_conflict(
                "The database refused to restore symbol \"" + expected.name() +
                "\" with its original UUID"));

        auto check = symbols.find_by_id(expected.id());
        if (!check)
            return std::unexpected(history_conflict(
                "Symbol \"" + expected.name() + "\" disappeared while it was being restored"));
        if (check->name() != expected.name() ||
            check->library_id() != expected.library_id() ||
            check->Kicad_Forge_ID() != expected.Kicad_Forge_ID())
            return std::unexpected(history_conflict(
                "Symbol \"" + expected.name() +
                "\" was not restored with its original identity"));
        restored.push_back({expected.id(), expected.name()});
    }
    return {};
}

Result<void> restore_move_undo(storage::DbSession& session, const HistoryEntry& entry,
                               std::vector<HistoryRestored>& restored)
{
    storage::SymbolRepository symbols(session);
    for (const auto& row : entry.moved_rows)
    {
        auto current = find_current(symbols, row.uuid, row.new_name);
        if (!current)
            return std::unexpected(current.error());
        if (current->library_id() != row.new_library_id || current->name() != row.new_name ||
            current->lib_id() != row.new_lib_id ||
            current->Kicad_Forge_ID() != row.kf_id)
            return std::unexpected(history_conflict(
                "Symbol \"" + row.new_name +
                "\" changed since the move; undo would overwrite another edit"));

        auto free_name = ensure_name_free(symbols, row.old_library_id, row.old_name, row.uuid);
        if (!free_name)
            return std::unexpected(free_name.error());

        core::Symbol updated = *current;
        updated.set_name(row.old_name);
        updated.set_library_id(row.old_library_id);
        auto saved = symbols.update(updated);
        if (!saved)
            return std::unexpected(saved.error());
        auto identity = set_symbol_identity(session, row.uuid, row.old_library_id,
                                            row.old_lib_id);
        if (!identity)
            return std::unexpected(identity.error());
        restored.push_back({row.uuid, row.old_name});
    }
    return {};
}

Result<void> restore_move_redo(storage::DbSession& session, const HistoryEntry& entry,
                               std::vector<HistoryRestored>& restored)
{
    storage::SymbolRepository symbols(session);
    for (const auto& row : entry.moved_rows)
    {
        auto current = find_current(symbols, row.uuid, row.old_name);
        if (!current)
            return std::unexpected(current.error());
        if (current->library_id() != row.old_library_id || current->name() != row.old_name ||
            current->lib_id() != row.old_lib_id ||
            current->Kicad_Forge_ID() != row.kf_id)
            return std::unexpected(history_conflict(
                "Symbol \"" + row.old_name +
                "\" changed since the undo; redo would overwrite another edit"));

        auto free_name = ensure_name_free(symbols, row.new_library_id, row.new_name, row.uuid);
        if (!free_name)
            return std::unexpected(free_name.error());

        core::Symbol updated = *current;
        updated.set_name(row.new_name);
        updated.set_library_id(row.new_library_id);
        auto saved = symbols.update(updated);
        if (!saved)
            return std::unexpected(saved.error());
        auto identity = set_symbol_identity(session, row.uuid, row.new_library_id,
                                            row.new_lib_id);
        if (!identity)
            return std::unexpected(identity.error());
        restored.push_back({row.uuid, row.new_name});
    }
    return {};
}

Result<std::vector<io::FileChange>> build_file_changes(const HistoryEntry& entry, bool undo)
{
    if (entry.files.empty())
        return std::unexpected(history_failed("History entry has no file snapshots"));

    std::vector<io::FileChange> changes;
    changes.reserve(entry.files.size());
    for (const auto& snapshot : entry.files)
    {
        if (snapshot.path.empty())
            return std::unexpected(history_failed("History entry has an empty file path"));

        io::FileChange change;
        change.path = fs::u8path(snapshot.path);
        change.owner = snapshot.owner;

        if (undo)
        {
            if (!snapshot.exists_after)
            {
                if (!snapshot.exists_before)
                    continue;
                // 操作后文件不存在（例如粘贴新建、撤销时先删掉的目标文件）；
                // 恢复 before：必须仍不存在，抢建出来的一律按冲突拒绝。
                change.content = snapshot.before_content;
                change.allow_missing = true;
                change.expected_missing = true;
            }
            else
            {
                change.expected_digest = io::content_digest(snapshot.after_content);
                if (snapshot.exists_before)
                    change.content = snapshot.before_content;
                else
                    change.remove = true;
            }
        }
        else
        {
            if (!snapshot.exists_before)
            {
                if (!snapshot.exists_after)
                    continue;
                // 重做时 after 文件在撤销中被删掉了，必须仍不存在才重建。
                change.content = snapshot.after_content;
                change.allow_missing = true;
                change.expected_missing = true;
            }
            else
            {
                change.expected_digest = io::content_digest(snapshot.before_content);
                if (snapshot.exists_after)
                    change.content = snapshot.after_content;
                else
                    change.remove = true;
            }
        }
        changes.push_back(std::move(change));
    }
    if (changes.empty())
        return std::unexpected(history_failed("History entry has no effective file changes"));
    return changes;
}

Result<void> map_apply_error(const Error& error)
{
    const std::string& message = error.message();
    if (message.rfind("history_conflict: ", 0) == 0 ||
        message.rfind("history_full: ", 0) == 0 || message.rfind("history_empty: ", 0) == 0 ||
        message.rfind("history_failed: ", 0) == 0)
        return std::unexpected(error);

    if (message.find("Source changed since it was read") != std::string::npos ||
        message.find("Source appeared since it was read") != std::string::npos ||
        message.find("Source missing:") != std::string::npos)
        return std::unexpected(history_conflict(message));

    if (error.kind() == Error::Kind::ValidationError || error.kind() == Error::Kind::NotFound)
        return std::unexpected(history_conflict(message));

    return std::unexpected(history_failed(message));
}

Result<HistoryOutcome> apply_entry(storage::Database& db, const HistoryEntry& entry, bool undo)
{
    auto changes = build_file_changes(entry, undo);
    if (!changes)
        return std::unexpected(changes.error());

    std::vector<HistoryRestored> restored;
    io::SourceMutationService mutations(db);
    auto applied = mutations.apply(
        *changes,
        [&](storage::DbSession& session) -> Result<void>
        {
            if (undo)
            {
                if (entry.kind == "paste")
                    return restore_paste_undo(session, entry, restored);
                if (entry.kind == "move")
                    return restore_move_undo(session, entry, restored);
            }
            else
            {
                if (entry.kind == "paste")
                    return restore_paste_redo(session, entry, restored);
                if (entry.kind == "move")
                    return restore_move_redo(session, entry, restored);
            }
            return std::unexpected(history_failed("Unknown history entry kind \"" + entry.kind +
                                                  "\""));
        });
    if (!applied)
        return std::unexpected(map_apply_error(applied.error()).error());

    HistoryOutcome outcome;
    outcome.kind = entry.kind;
    outcome.count = entry.count;
    outcome.restored = std::move(restored);
    outcome.can_undo = g_cursor > 0;
    outcome.can_redo = g_cursor < g_entries.size();
    return outcome;
}

}  // namespace

std::mutex& editing_commit_mutex()
{
    return g_editing_mutex;
}

namespace testing
{
void reset_history()
{
    std::lock_guard lock(editing_commit_mutex());
    g_entries.clear();
    g_cursor = 0;
    g_limits = HistoryLimits{};
    g_next_id.store(1);
}

void set_history_limits(std::size_t entries, std::size_t bytes, std::size_t file_bytes)
{
    std::lock_guard lock(editing_commit_mutex());
    g_limits.entries = std::max<std::size_t>(1, entries);
    g_limits.bytes = std::max<std::size_t>(1, bytes);
    g_limits.file_bytes = std::max<std::size_t>(1, file_bytes);
}
}  // namespace testing

std::string new_history_entry_id()
{
    char buffer[40] = {};
    const auto value = g_next_id.fetch_add(1, std::memory_order_relaxed);
    std::snprintf(buffer, sizeof(buffer), "hist-%016llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

long long history_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Result<void> precheck_history_capacity(const HistoryEntry& entry)
{
    for (const auto& file : entry.files)
    {
        if (file.before_content.size() > g_limits.file_bytes ||
            file.after_content.size() > g_limits.file_bytes)
            return std::unexpected(history_full(
                "History snapshot for \"" + file.path + "\" exceeds the " +
                std::to_string(g_limits.file_bytes) + "-byte per-file history budget"));
    }
    const std::size_t bytes = history_entry_bytes(entry);
    if (bytes > g_limits.bytes)
        return std::unexpected(history_full(
            "This edit needs " + std::to_string(bytes) + " bytes of history, above the " +
            std::to_string(g_limits.bytes) + "-byte session budget"));
    return {};
}

void note_successful_edit()
{
    std::lock_guard lock(editing_commit_mutex());
    // 一次新的成功编辑会让 redo 分支失效；历史指针保持在当前位置，
    // 之后的 undo 只回退到这次编辑之前（ED-054）。
    if (g_cursor < g_entries.size())
        g_entries.erase(g_entries.begin() + static_cast<std::ptrdiff_t>(g_cursor),
                        g_entries.end());
}

void push_history(HistoryEntry entry)
{
    // 调用方已通过 precheck_history_capacity；仍按条目/总字节上限整条淘汰最旧单元。
    g_entries.erase(g_entries.begin() + static_cast<std::ptrdiff_t>(g_cursor), g_entries.end());
    g_entries.push_back(std::move(entry));
    g_cursor = g_entries.size();

    while (g_entries.size() > g_limits.entries)
    {
        g_entries.erase(g_entries.begin());
        if (g_cursor > 0)
            --g_cursor;
    }
    while (g_entries.size() > 1 && total_history_bytes() > g_limits.bytes)
    {
        g_entries.erase(g_entries.begin());
        if (g_cursor > 0)
            --g_cursor;
    }
}

HistoryState history_state()
{
    std::lock_guard lock(editing_commit_mutex());
    HistoryState state;
    state.count = g_entries.size();
    state.bytes = total_history_bytes();
    state.can_undo = g_cursor > 0;
    state.can_redo = g_cursor < g_entries.size();
    state.undo_index = state.can_undo ? g_cursor - 1 : g_entries.size();
    state.redo_index = state.can_redo ? g_cursor : g_entries.size();
    state.entries.reserve(g_entries.size());
    for (const auto& entry : g_entries)
        state.entries.push_back(summarize(entry));
    return state;
}

Result<HistoryOutcome> undo_history(storage::Database& db)
{
    std::lock_guard lock(editing_commit_mutex());
    if (g_cursor == 0)
        return std::unexpected(
            history_empty_error("There is no successful edit left to undo in this session"));

    auto outcome = apply_entry(db, g_entries[g_cursor - 1], true);
    if (!outcome)
        return std::unexpected(outcome.error());
    --g_cursor;
    outcome->can_undo = g_cursor > 0;
    outcome->can_redo = g_cursor < g_entries.size();
    return outcome;
}

Result<HistoryOutcome> redo_history(storage::Database& db)
{
    std::lock_guard lock(editing_commit_mutex());
    if (g_cursor >= g_entries.size())
        return std::unexpected(
            history_empty_error("There is no undone edit left to redo in this session"));

    auto outcome = apply_entry(db, g_entries[g_cursor], false);
    if (!outcome)
        return std::unexpected(outcome.error());
    ++g_cursor;
    outcome->can_undo = g_cursor > 0;
    outcome->can_redo = g_cursor < g_entries.size();
    return outcome;
}

std::string history_error_code(const util::Error& error)
{
    const auto code = error_code_of(error);
    if (!code.empty())
        return code;
    return std::string(kHistoryFailed);
}

int history_error_status(std::string_view code)
{
    if (code == kHistoryEmpty || code == kHistoryConflict || code == kHistoryFull)
        return 409;
    return 500;
}

}  // namespace kforge::services
