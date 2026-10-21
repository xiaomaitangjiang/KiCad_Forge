#include "interface/service/editing/paste.h"

#include "interface/service/editing/history.h"
#include "interface/service/editing/node_util.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/db/database.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/dom/dom_builder.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/sexpr/token/tokenizer.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"

namespace kforge::services
{
namespace
{

using util::Error;
using util::Result;
using nlohmann::json;
namespace fs = std::filesystem;
namespace sexpr = kforge::sexpr;

constexpr std::string_view kNewFileHeader =
    "(kicad_symbol_lib (version 20231120) (generator kforge)\n";

// 稳定错误码：HTTP 层与前端按它分支，不解析自然语言消息。
constexpr std::string_view kTargetNotFound = "target_not_found";
constexpr std::string_view kTargetNotWritable = "target_not_writable";
constexpr std::string_view kInvalidRequest = "invalid_request";
constexpr std::string_view kPayloadTooLarge = "payload_too_large";
constexpr std::string_view kSymbolInvalid = "symbol_invalid";
constexpr std::string_view kConflict = "conflict";
constexpr std::string_view kSourceChanged = "source_changed";
constexpr std::string_view kOperationConflict = "operation_conflict";
constexpr std::string_view kCommitFailed = "commit_failed";

Error make_error(Error::Kind kind, std::string code, std::string message)
{
    const std::string text = std::move(code) + ": " + std::move(message);
    switch (kind)
    {
        case Error::Kind::NotFound:
            return Error::make<Error::Kind::NotFound>(text);
        case Error::Kind::ValidationError:
            return Error::make<Error::Kind::ValidationError>(text);
        case Error::Kind::ParseError:
            return Error::make<Error::Kind::ParseError>(text);
        case Error::Kind::ServiceError:
            return Error::make<Error::Kind::ServiceError>(text);
        case Error::Kind::IoError:
            return Error::make<Error::Kind::IoError>(text);
        case Error::Kind::DbError:
            return Error::make<Error::Kind::DbError>(text);
        default:
            return Error::make<Error::Kind::InternalError>(text);
    }
}

Error invalid_request(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kInvalidRequest), std::move(message));
}
Error payload_too_large(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kPayloadTooLarge), std::move(message));
}
Error target_not_found(std::string message)
{
    return make_error(Error::Kind::NotFound, std::string(kTargetNotFound), std::move(message));
}
Error target_not_writable(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kTargetNotWritable), std::move(message));
}
Error symbol_invalid(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kSymbolInvalid), std::move(message));
}
Error conflict(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kConflict), std::move(message));
}
Error source_changed(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kSourceChanged), std::move(message));
}
Error operation_conflict(std::string message)
{
    return make_error(Error::Kind::ValidationError, std::string(kOperationConflict), std::move(message));
}
Error commit_failed(std::string message)
{
    return make_error(Error::Kind::InternalError, std::string(kCommitFailed), std::move(message));
}

bool is_known_code(std::string_view code)
{
    return code == kTargetNotFound || code == kTargetNotWritable || code == kInvalidRequest ||
           code == kPayloadTooLarge || code == kSymbolInvalid || code == kConflict ||
           code == kSourceChanged || code == kOperationConflict || code == kCommitFailed ||
           code == "history_empty" || code == "history_conflict" || code == "history_full" ||
           code == "history_failed";
}

struct ParsedNode
{
    std::string text;  // 单个顶层节点的完整原文
    std::string parent;
    std::string footprint;
};

/// 客户端 sexpr 必须是「恰好一个 (symbol "name" ...) 顶层节点」。
/// 允许多余空白；多一个节点、顶层类型不对、声明名与节点名不一致都拒绝 ——
/// 粘贴时只信任这一份文本，不能再回头读源文件。
Result<ParsedNode> parse_symbol_payload(const PasteItem& item)
{
    std::string_view text = item.sexpr;
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return std::unexpected(symbol_invalid("Symbol \"" + item.name + "\" has empty sexpr content"));
    if (text[first] != '(')
        return std::unexpected(symbol_invalid("Symbol \"" + item.name +
                                              "\" sexpr must start with a top-level (symbol ...) node"));
    const auto close = sexpr::find_matching_paren(text, first);
    if (close == std::string_view::npos)
        return std::unexpected(
            symbol_invalid("Symbol \"" + item.name + "\" has unbalanced parentheses"));
    for (std::size_t i = close + 1; i < text.size(); ++i)
    {
        if (!is_space(text[i]))
            return std::unexpected(symbol_invalid(
                "Symbol \"" + item.name +
                "\" must contain exactly one top-level node (trailing content was found)"));
    }
    ParsedNode parsed;
    parsed.text = std::string(text.substr(first, close - first + 1));

    sexpr::DomBuilder builder;
    auto dom = builder.parse_one(parsed.text);
    if (!dom)
        return std::unexpected(symbol_invalid("Symbol \"" + item.name + "\" cannot be parsed: " +
                                              dom.error().format_message()));
    if ((*dom)->type() != "symbol")
        return std::unexpected(symbol_invalid("Symbol \"" + item.name +
                                              "\" sexpr is (\"..." + std::string((*dom)->type()) +
                                              " ...), not a (symbol ...) node"));
    const std::string node_name((*dom)->atom_value());
    if (node_name != item.name)
        return std::unexpected(symbol_invalid("Symbol \"" + item.name +
                                              "\" declared name does not match the node name \"" +
                                              node_name + "\""));
    if (const auto* ext = (*dom)->find_child("extends"); ext != nullptr)
    {
        if (!ext->is_atom() || ext->atom_value().empty())
            return std::unexpected(symbol_invalid("Symbol \"" + item.name +
                                                  "\" has an (extends ...) node without a parent name"));
        parsed.parent = std::string(ext->atom_value());
        std::string why;
        if (!is_plain_name(parsed.parent, why))
            return std::unexpected(symbol_invalid("Symbol \"" + item.name +
                                                  "\" extends an invalid parent name: " + why));
    }
    for (const auto* prop : (*dom)->find_children("property"))
    {
        const auto key = prop->property("Footprint");
        if (key.has_value())
        {
            parsed.footprint = std::string(*key);
            break;
        }
    }
    return parsed;
}

Result<FileState> read_target_file(const fs::path& path)
{
    return read_library_file(path, "Target library");
}

Result<std::string> file_symbol_node(const FileState& file, const std::string& name)
{
    return kforge::services::file_symbol_node(file, name, "Target library");
}

struct TargetState
{
    core::LibraryMeta library;
    fs::path path;
    std::string path_text;
    std::string owner_id;
    bool owner_missing{false};
    FileState file;
};

Result<TargetState> load_target(const storage::DbSession& session, const PasteRequest& request,
                                std::vector<std::string>& warnings)
{
    TargetState target;
    storage::LibraryRepository libraries(session);
    auto rows = libraries.find_all();
    if (!rows)
        return std::unexpected(rows.error());
    const core::LibraryMeta* found = nullptr;
    for (const auto& row : *rows)
    {
        if (row.id == request.target_library_id)
        {
            found = &row;
            break;
        }
    }
    if (found == nullptr)
        return std::unexpected(target_not_found("Target library is not registered: " +
                                                request.target_library_id));
    target.library = *found;
    target.path = found->file_path;
    target.path_text = to_utf8(target.path);
    if (target.path.empty())
        return std::unexpected(
            target_not_writable("Target library \"" + target.library.name + "\" has no source file"));
    if (lower_copy(to_utf8(target.path.extension())) != ".kicad_sym")
        return std::unexpected(target_not_writable("Target library path is not a .kicad_sym file: " +
                                                   target.path_text));

    target.owner_id = found->component_library_id;
    if (!target.owner_id.empty())
    {
        storage::ComponentLibraryRepository components(session);
        auto all = components.find_all();
        if (!all)
            return std::unexpected(all.error());
        const storage::ComponentLibraryRepository::ComponentLibrary* owner = nullptr;
        for (const auto& row : *all)
        {
            if (row.id == target.owner_id)
            {
                owner = &row;
                break;
            }
        }
        if (owner == nullptr)
        {
            target.owner_missing = true;
            return std::unexpected(target_not_writable("Owning component library of \"" +
                                                       target.library.name +
                                                       "\" no longer exists: " + target.owner_id));
        }
        if (!owner->enabled)
            return std::unexpected(target_not_writable("Owning component library \"" +
                                                       owner->name + "\" is disabled"));
        if (owner->locked || owner->is_protected)
            return std::unexpected(target_not_writable("Owning component library \"" +
                                                       owner->name + "\" is locked or protected"));
    }
    else
    {
        warnings.push_back("target library \"" + target.library.name +
                           "\" has no owning component library; only the registered source is "
                           "used for the write check");
    }

    auto authorized = io::authorize_source_write(session, target.path, target.owner_id);
    if (!authorized)
    {
        if (authorized.error().kind() == Error::Kind::DbError)
            return std::unexpected(authorized.error());
        return std::unexpected(target_not_writable("Target library is not writable: " +
                                                   authorized.error().format_message()));
    }

    auto file = read_target_file(target.path);
    if (!file)
        return std::unexpected(file.error());
    target.file = std::move(*file);
    if (!target.file.exists)
        warnings.push_back("target library file does not exist yet and will be created: " +
                           target.path_text);
    return target;
}

struct ItemState
{
    std::string source_name;
    std::string final_name;
    std::string parent;  // 源父名
    std::string parent_final;
    std::string footprint;
    std::string raw_node;
    std::string node;  // 改名/剥 KF 后的最终文本
    bool dependency{false};
    bool reachable{false};
    bool write{false};
    int parent_index{-1};
};

struct Analysis
{
    PastePlan plan;
    TargetState target;
    std::vector<ItemState> items;
    std::vector<std::size_t> order;  // 实际写入顺序（父在前）
};

Result<void> validate_request(const PasteRequest& request)
{
    if (request.target_library_id.empty())
        return std::unexpected(invalid_request("target_library_id is required"));
    if (request.target_library_id.size() > 64)
        return std::unexpected(invalid_request("target_library_id is too long"));
    if (request.operation_id.empty())
        return std::unexpected(invalid_request("operation_id is required"));
    if (request.operation_id.size() > kMaxPasteOperationIdLength)
        return std::unexpected(invalid_request(
            "operation_id must be at most " + std::to_string(kMaxPasteOperationIdLength) +
            " bytes"));
    if (!request.target_digest.empty() && !is_digest_text(request.target_digest))
        return std::unexpected(invalid_request(
            "target_digest must be 16 hexadecimal characters (xxh64) or empty"));
    if (request.items.empty())
        return std::unexpected(invalid_request("items must contain at least one symbol"));
    if (request.items.size() > kMaxPasteItems)
        return std::unexpected(
            invalid_request("Too many clipboard items: " + std::to_string(request.items.size()) +
                            " > " + std::to_string(kMaxPasteItems)));

    std::size_t total = 0;
    for (const auto& item : request.items)
    {
        std::string why;
        if (!is_plain_name(item.name, why))
            return std::unexpected(invalid_request("Invalid symbol name: " + why));
        if (item.sexpr.empty())
            return std::unexpected(invalid_request("Symbol \"" + item.name +
                                                   "\" has empty sexpr content"));
        total += item.name.size() + item.sexpr.size() + 64;
    }
    for (const auto& [from, to] : request.renames)
    {
        std::string why;
        if (!is_plain_name(from, why))
            return std::unexpected(invalid_request("Invalid rename source name: " + why));
        if (!is_plain_name(to, why))
            return std::unexpected(invalid_request("Invalid rename target name: " + why));
        total += from.size() + to.size() + 8;
    }
    if (total > kMaxPasteBytes)
        return std::unexpected(payload_too_large(
            "Clipboard payload is " + std::to_string(total) + " bytes, exceeding the " +
            std::to_string(kMaxPasteBytes) + " bytes limit"));
    return {};
}

/// 只读分析：目标校验 + 剪贴板解析 + 依赖闭包 + 冲突 + 写入顺序。
/// 预检与提交共用同一份逻辑，避免「预检说可以、提交才失败」的口径漂移。
Result<Analysis> analyze(const storage::DbSession& session, const PasteRequest& request)
{
    Analysis analysis;
    auto target = load_target(session, request, analysis.plan.warnings);
    if (!target)
        return std::unexpected(target.error());
    analysis.target = std::move(*target);
    analysis.plan.target.library_id = analysis.target.library.id;
    analysis.plan.target.name = analysis.target.library.name;
    analysis.plan.target.file_path = analysis.target.path_text;
    analysis.plan.target.digest = analysis.target.file.digest;
    analysis.plan.target.exists = analysis.target.file.exists;

    std::unordered_set<std::string> db_names;
    {
        storage::SymbolRepository symbols(session);
        auto rows = symbols.find_summaries("", analysis.target.library.id);
        if (!rows)
            return std::unexpected(rows.error());
        for (const auto& row : *rows)
            db_names.insert(row.name());
    }

    std::unordered_map<std::string, std::size_t> index;
    analysis.items.reserve(request.items.size());
    for (std::size_t i = 0; i < request.items.size(); ++i)
    {
        const auto& item = request.items[i];
        if (index.count(item.name) != 0)
            return std::unexpected(invalid_request("Duplicate clipboard item name: \"" +
                                                   item.name + "\""));
        ItemState state;
        state.source_name = item.name;
        state.final_name = item.name;
        state.dependency = item.dependency;
        auto parsed = parse_symbol_payload(item);
        if (!parsed)
            return std::unexpected(parsed.error());
        state.parent = parsed->parent;
        state.footprint = parsed->footprint;
        state.raw_node = std::move(parsed->text);
        index.emplace(item.name, i);
        analysis.items.push_back(std::move(state));
    }

    // 改名：只作用于直接粘贴的对象；继承闭包里的父符号保持原名（改名会让
    // 源文件之外的其它引用失配，超出这次粘贴的授权范围）。
    for (auto& state : analysis.items)
    {
        const auto rename = request.renames.find(state.source_name);
        if (rename == request.renames.end())
            continue;
        if (state.dependency)
        {
            analysis.plan.warnings.push_back("rename for inherited dependency \"" +
                                             state.source_name + "\" was ignored");
            continue;
        }
        state.final_name = rename->second;
    }
    for (const auto& [from, to] : request.renames)
    {
        if (index.count(from) == 0)
            analysis.plan.warnings.push_back("rename for unknown symbol \"" + from +
                                             "\" was ignored");
    }

    std::unordered_map<std::string, std::string> final_owner;
    for (const auto& state : analysis.items)
    {
        auto [it, inserted] = final_owner.emplace(state.final_name, state.source_name);
        if (!inserted)
            return std::unexpected(invalid_request(
                "Symbol name \"" + state.final_name + "\" is used by both \"" + it->second +
                "\" and \"" + state.source_name + "\"; rename one of them"));
    }

    // 继承父必须是剪贴板里的对象或目标库里已有的顶层符号。
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        auto& state = analysis.items[i];
        if (state.parent.empty())
            continue;
        const auto parent = index.find(state.parent);
        if (parent != index.end())
        {
            state.parent_index = static_cast<int>(parent->second);
            state.parent_final = analysis.items[parent->second].final_name;
        }
        else if (analysis.target.file.parent_of.count(state.parent) != 0)
        {
            state.parent_final = state.parent;
        }
        else
        {
            return std::unexpected(symbol_invalid(
                "Symbol \"" + state.source_name + "\" extends \"" + state.parent +
                "\", which is neither included in the clipboard nor present in the target "
                "library"));
        }
    }

    // 环检测（每个对象最多一个父）。
    std::vector<int> color(analysis.items.size(), 0);
    std::function<Result<void>(std::size_t)> visit = [&](std::size_t i) -> Result<void>
    {
        if (color[i] == 1)
            return std::unexpected(symbol_invalid("Clipboard contains a cyclic (extends ...) chain "
                                                  "involving \"" +
                                                  analysis.items[i].source_name + "\""));
        if (color[i] == 2)
            return {};
        color[i] = 1;
        if (analysis.items[i].parent_index >= 0)
        {
            auto nested = visit(static_cast<std::size_t>(analysis.items[i].parent_index));
            if (!nested)
                return nested;
        }
        color[i] = 2;
        return {};
    };
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        auto checked = visit(i);
        if (!checked)
            return std::unexpected(checked.error());
    }

    // 可达性：只有从直接粘贴对象出发的继承闭包才会写入；payload 里多余的
    // dependency 项忽略并告警，避免把无关内容偷偷写进目标库。
    std::function<void(std::size_t)> mark = [&](std::size_t i)
    {
        if (analysis.items[i].reachable)
            return;
        analysis.items[i].reachable = true;
        if (analysis.items[i].parent_index >= 0)
            mark(static_cast<std::size_t>(analysis.items[i].parent_index));
    };
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        if (!analysis.items[i].dependency)
            mark(i);
    }

    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        auto& state = analysis.items[i];
        if (!state.reachable)
        {
            if (state.dependency)
                analysis.plan.warnings.push_back("unused inherited dependency \"" +
                                                 state.source_name + "\" was ignored");
            continue;
        }
        if (!state.dependency)
        {
            state.write = true;
            continue;
        }
        const auto existing = analysis.target.file.parent_of.find(state.source_name);
        if (existing == analysis.target.file.parent_of.end())
        {
            state.write = true;
            continue;
        }
        auto node = file_symbol_node(analysis.target.file, state.source_name);
        if (!node)
            return std::unexpected(node.error());
        if (normalize_node(*node) != normalize_node(state.raw_node))
            return std::unexpected(symbol_invalid(
                "Target library already defines inherited symbol \"" + state.source_name +
                "\" with different content; rename the pasted symbol or remove the conflict"));
        analysis.plan.warnings.push_back("inherited symbol \"" + state.source_name +
                                         "\" already exists in the target library and will be "
                                         "reused");
    }

    // 冲突：DB 唯一键 (library_id,name) 与文件顶层名都必须查，不能依赖
    // INSERT OR IGNORE 静默吞掉重复行。
    for (const auto& state : analysis.items)
    {
        if (!state.reachable || state.dependency)
            continue;
        const bool clash = db_names.count(state.final_name) != 0 ||
                           analysis.target.file.parent_of.count(state.final_name) != 0;
        if (clash)
            analysis.plan.conflicts.push_back(state.source_name);
    }

    // 写入顺序：父在前。只有会真正写入文件的对象进 order。
    std::vector<char> emitted(analysis.items.size(), 0);
    std::function<void(std::size_t)> emit = [&](std::size_t i)
    {
        if (emitted[i])
            return;
        emitted[i] = 1;
        if (analysis.items[i].parent_index >= 0)
            emit(static_cast<std::size_t>(analysis.items[i].parent_index));
        if (analysis.items[i].write)
            analysis.order.push_back(i);
    };
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        if (!analysis.items[i].dependency)
            emit(i);
    }

    // 生成最终节点文本（改名 + 父名重写 + 剥旧 KF）。
    for (std::size_t i = 0; i < analysis.items.size(); ++i)
    {
        auto& state = analysis.items[i];
        if (!state.write)
            continue;
        auto rewritten = rewrite_symbol_node(state.raw_node, state.source_name,
                                             state.final_name, state.parent, state.parent_final);
        if (!rewritten)
            return std::unexpected(rewritten.error());
        state.node = std::move(*rewritten);
    }

    // 封装引用：不默认复制依赖库；目标库解析不唯一时明确告警，不自动绑定。
    for (const std::size_t i : analysis.order)
    {
        const auto& state = analysis.items[i];
        if (state.footprint.find(':') == std::string::npos)
            continue;
        const auto colon = state.footprint.find(':');
        storage::FootprintRepository footprints(session);
        auto resolved = footprints.resolve_legacy_reference(
            state.footprint.substr(0, colon), state.footprint.substr(colon + 1));
        if (resolved)
            continue;
        if (resolved.error().kind() == Error::Kind::DbError)
            return std::unexpected(resolved.error());
        analysis.plan.warnings.push_back("footprint reference \"" + state.footprint +
                                         "\" could not be resolved uniquely in the target "
                                         "database; it will be kept as-is");
    }

    for (const auto& state : analysis.items)
    {
        if (!state.reachable || state.dependency)
            continue;
        analysis.plan.creates.push_back({state.source_name, state.final_name});
    }
    return analysis;
}

struct Prepared
{
    std::string content;
    std::vector<core::Symbol> symbols;  // 按创建顺序，已带 library_id/新 KF
};

Result<std::string> build_library_content(const TargetState& target,
                                          const std::vector<std::string>& nodes)
{
    std::string block;
    for (const auto& node : nodes)
    {
        block += "\t";
        block += node;
        block += "\n";
    }
    if (!target.file.exists)
        return std::string(kNewFileHeader) + block + ")\n";

    std::string out = target.file.text.substr(0, target.file.root_close);
    if (out.empty() || out.back() != '\n')
        out += '\n';
    out += block;
    out += target.file.text.substr(target.file.root_close);
    return out;
}

Result<Prepared> prepare_paste(const Analysis& analysis)
{
    std::vector<std::string> nodes;
    nodes.reserve(analysis.order.size());
    for (const std::size_t i : analysis.order)
        nodes.push_back(analysis.items[i].node);

    auto content = build_library_content(analysis.target, nodes);
    if (!content)
        return std::unexpected(content.error());

    auto parsed = parser::SymbolLibParser::parse_buffer(*content, analysis.target.library.name);
    if (!parsed)
        return std::unexpected(symbol_invalid("Pasted content would produce an invalid symbol "
                                              "library: " +
                                              parsed.error().format_message()));

    std::unordered_map<std::string, core::Symbol> by_name;
    for (auto& symbol : parsed->items)
        by_name.emplace(symbol.name(), std::move(symbol));

    Prepared prepared;
    std::unordered_map<std::string, std::string> kf_by_name;
    for (const auto& create : analysis.plan.creates)
    {
        auto it = by_name.find(create.new_name);
        if (it == by_name.end())
            return std::unexpected(commit_failed("Pasted symbol \"" + create.new_name +
                                                 "\" is missing from the generated library"));
        core::Symbol symbol = it->second;
        symbol.set_library_id(analysis.target.library.id);
        symbol.refresh_Kicad_Forge_ID();
        kf_by_name[create.new_name] = symbol.Kicad_Forge_ID();
        prepared.symbols.push_back(std::move(symbol));
    }

    const int patched = sexpr::patch_kf_id_content(*content, kf_by_name);
    if (static_cast<std::size_t>(patched) != prepared.symbols.size())
        return std::unexpected(commit_failed("Failed to write Kicad_Forge_ID into every pasted "
                                             "symbol (patched " +
                                             std::to_string(patched) + " of " +
                                             std::to_string(prepared.symbols.size()) + ")"));

    // 用最终文件内容重新解析：数据库行与落盘字节必须来自同一份内容。
    auto final_parsed =
        parser::SymbolLibParser::parse_buffer(*content, analysis.target.library.name);
    if (!final_parsed)
        return std::unexpected(commit_failed("Generated library content cannot be re-parsed: " +
                                             final_parsed.error().format_message()));
    std::unordered_map<std::string, core::Symbol> final_by_name;
    for (auto& symbol : final_parsed->items)
        final_by_name.emplace(symbol.name(), std::move(symbol));
    for (auto& symbol : prepared.symbols)
    {
        auto it = final_by_name.find(symbol.name());
        if (it == final_by_name.end())
            return std::unexpected(commit_failed("Pasted symbol \"" + symbol.name() +
                                                 "\" disappeared after KF patching"));
        if (it->second.Kicad_Forge_ID() != symbol.Kicad_Forge_ID())
            return std::unexpected(commit_failed("Kicad_Forge_ID mismatch for pasted symbol \"" +
                                                 symbol.name() + "\""));
        it->second.set_library_id(analysis.target.library.id);
        symbol = std::move(it->second);
    }
    prepared.content = std::move(*content);
    return prepared;
}

std::string canonical_request_digest(const PasteRequest& request)
{
    json items = json::array();
    for (const auto& item : request.items)
    {
        json row;
        row["name"] = item.name;
        row["sexpr"] = item.sexpr;
        row["dependency"] = item.dependency;
        items.push_back(std::move(row));
    }
    json renames = json::object();
    for (const auto& [from, to] : request.renames)
        renames[from] = to;
    json canonical;
    canonical["target_library_id"] = request.target_library_id;
    canonical["items"] = std::move(items);
    canonical["renames"] = std::move(renames);
    // target_digest 刻意不进摘要：同一次操作网络重试时目标摘要可能已经变化，
    // 但重放必须返回首次结果而不是误报 source_changed。
    return io::content_digest(canonical.dump());
}

struct IdempotencyEntry
{
    std::string request_digest;
    std::vector<PasteCreated> created;
};

std::map<std::string, IdempotencyEntry, std::less<>> g_idempotency;
std::deque<std::string> g_idempotency_order;

const IdempotencyEntry* find_idempotent(const std::string& operation_id)
{
    const auto it = g_idempotency.find(operation_id);
    return it == g_idempotency.end() ? nullptr : &it->second;
}

void remember_idempotent(const std::string& operation_id, std::string digest,
                         std::vector<PasteCreated> created)
{
    if (auto it = g_idempotency.find(operation_id); it != g_idempotency.end())
    {
        it->second.request_digest = std::move(digest);
        it->second.created = std::move(created);
        return;
    }
    g_idempotency.emplace(operation_id, IdempotencyEntry{std::move(digest), std::move(created)});
    g_idempotency_order.push_back(operation_id);
    while (g_idempotency_order.size() > kMaxPasteIdempotencyEntries)
    {
        g_idempotency.erase(g_idempotency_order.front());
        g_idempotency_order.pop_front();
    }
}

Result<PasteOutcome> map_apply_error(const Error& error)
{
    const std::string& message = error.message();
    if (message.find("Source changed since it was read") != std::string::npos ||
        message.find("Source appeared since it was read") != std::string::npos ||
        message.find("Source missing:") != std::string::npos)
    {
        return std::unexpected(source_changed(
            "Target library changed since it was read; reload the target and retry"));
    }
    if (message.find("candidate content is malformed") != std::string::npos)
        return std::unexpected(commit_failed("Generated library content failed validation: " +
                                             message));
    if (error.kind() == Error::Kind::ValidationError || error.kind() == Error::Kind::NotFound)
        return std::unexpected(target_not_writable("Target library is not writable: " + message));
    if (error.kind() == Error::Kind::IoError || error.kind() == Error::Kind::DbError)
        return std::unexpected(commit_failed(message));
    return std::unexpected(error);
}

}  // namespace

std::string paste_error_code(const util::Error& error)
{
    const std::string& message = error.message();
    const auto colon = message.find(": ");
    const std::string_view prefix =
        colon == std::string::npos ? std::string_view(message) : std::string_view(message).substr(0, colon);
    if (is_known_code(prefix))
        return std::string(prefix);
    switch (error.kind())
    {
        case Error::Kind::NotFound:
            return std::string(kTargetNotFound);
        case Error::Kind::ValidationError:
            return std::string(kInvalidRequest);
        case Error::Kind::ParseError:
        case Error::Kind::ServiceError:
            return std::string(kSymbolInvalid);
        case Error::Kind::DbError:
        case Error::Kind::IoError:
            return std::string(kCommitFailed);
        default:
            return std::string(kCommitFailed);
    }
}

int paste_error_status(std::string_view code)
{
    if (code == kTargetNotFound)
        return 404;
    if (code == kTargetNotWritable)
        return 403;
    if (code == kInvalidRequest)
        return 400;
    if (code == kPayloadTooLarge)
        return 413;
    if (code == kSymbolInvalid)
        return 422;
    if (code == "history_conflict" || code == "history_full" || code == "history_empty")
        return 409;
    if (code == "history_failed")
        return 500;
    if (code == kConflict || code == kSourceChanged || code == kOperationConflict)
        return 409;
    return 500;
}

util::Result<PastePlan> preview_paste(storage::Database& db, const PasteRequest& request)
{
    auto valid = validate_request(request);
    if (!valid)
        return std::unexpected(valid.error());
    auto session = db.session();
    auto analysis = analyze(session, request);
    if (!analysis)
        return std::unexpected(analysis.error());
    return analysis->plan;
}

util::Result<PasteOutcome> paste_symbols(storage::Database& db, const PasteRequest& request)
{
    auto valid = validate_request(request);
    if (!valid)
        return std::unexpected(valid.error());
    if (request.dry_run)
        return std::unexpected(invalid_request("dry_run requests must go through preview_paste"));

    // 进程内一次只允许一个粘贴提交：幂等缓存、目标复核、文件变更、DB 插入
    // 必须在同一临界区里，否则同 operation_id 的并发重试可能双写。
    const std::string digest = canonical_request_digest(request);
    std::lock_guard<std::mutex> lock(editing_commit_mutex());
    if (const IdempotencyEntry* existing = find_idempotent(request.operation_id);
        existing != nullptr)
    {
        if (existing->request_digest != digest)
            return std::unexpected(operation_conflict(
                "operation_id \"" + request.operation_id +
                "\" was already used for a different paste request; use a new operation_id"));
        PasteOutcome replayed;
        replayed.created = existing->created;
        replayed.replayed = true;
        return replayed;
    }

    Analysis analysis;
    {
        // 先做一次只读分析（复核目标、摘要、冲突），再交给变更服务在锁内二次复核。
        auto session = db.session();
        auto analyzed = analyze(session, request);
        if (!analyzed)
            return std::unexpected(analyzed.error());
        analysis = std::move(*analyzed);
    }
    if (!analysis.plan.conflicts.empty())
    {
        std::string names;
        for (const auto& name : analysis.plan.conflicts)
        {
            if (!names.empty())
                names += ", ";
            names += '"' + name + '"';
        }
        return std::unexpected(conflict(
            "Target library \"" + analysis.target.library.name +
            "\" already defines " + names + "; rename the pasted symbols or choose another target"));
    }
    // 封装/3D 引用无法唯一解析时不能默默写入：预检给出 warning，提交必须拒绝，
    // 直到前端提供明确映射（ED-035）。
    for (const auto& warning : analysis.plan.warnings)
    {
        if (warning.rfind("footprint reference", 0) == 0)
            return std::unexpected(symbol_invalid(
                "The pasted symbols contain an unresolved footprint reference: " + warning +
                "; map it explicitly before pasting"));
    }
    if (upper_copy(request.target_digest) != analysis.plan.target.digest)
        return std::unexpected(source_changed(
            "Target library changed since it was read; reload the target and retry"));

    auto prepared = prepare_paste(analysis);
    if (!prepared)
        return std::unexpected(prepared.error());

    // 历史单元必须在 apply 之前构造并预检容量：若整次粘贴放不进历史预算，
    // 文件与 DB 都不能写，更不能写完后才发现无法撤销。
    HistoryEntry history;
    history.id = new_history_entry_id();
    history.kind = "paste";
    history.count = prepared->symbols.size();
    history.target_library_name = analysis.target.library.name;
    history.created_at_ms = history_now_ms();
    HistoryFileSnapshot snapshot;
    snapshot.path = analysis.target.path_text;
    snapshot.owner = analysis.target.owner_id;
    snapshot.exists_before = analysis.target.file.exists;
    snapshot.before_content = analysis.target.file.text;
    snapshot.exists_after = true;
    snapshot.after_content = prepared->content;
    history.files.push_back(std::move(snapshot));
    history.created_symbols = prepared->symbols;
    auto capacity = precheck_history_capacity(history);
    if (!capacity)
        return std::unexpected(capacity.error());

    io::FileChange change;
    change.path = analysis.target.path;
    change.content = std::move(prepared->content);
    change.expected_digest = analysis.target.file.digest;
    change.allow_missing = !analysis.target.file.exists;
    change.expected_missing = !analysis.target.file.exists;
    change.owner = analysis.target.owner_id;

    std::vector<PasteCreated> created;
    std::vector<core::Symbol> inserted_symbols;
    io::SourceMutationService mutations(db);
    auto applied = mutations.apply(
        {change},
        [&](storage::DbSession& session) -> util::Result<void>
        {
            storage::SymbolRepository symbols(session);
            created.reserve(prepared->symbols.size());
            inserted_symbols.reserve(prepared->symbols.size());
            for (const auto& symbol : prepared->symbols)
            {
                auto row = symbols.insert(symbol);
                if (!row)
                    return std::unexpected(row.error());
                created.push_back({row->id(), row->name()});
                inserted_symbols.push_back(std::move(*row));
            }
            return {};
        });
    if (!applied)
        return map_apply_error(applied.error());

    // 用真实插入行替换预检时的占位副本，重做才能恢复完全相同的 UUID/KF。
    history.created_symbols = std::move(inserted_symbols);
    push_history(std::move(history));
    remember_idempotent(request.operation_id, digest, created);
    PasteOutcome outcome;
    outcome.created = std::move(created);
    outcome.replayed = false;
    return outcome;
}

namespace testing
{
void reset_paste_state()
{
    std::lock_guard<std::mutex> lock(editing_commit_mutex());
    g_idempotency.clear();
    g_idempotency_order.clear();
}
}  // namespace testing

}  // namespace kforge::services
