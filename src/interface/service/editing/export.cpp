#include "interface/service/editing/export.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/db/database.h"
#include "core/io/sexpr/dom/dom_builder.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"

namespace kforge::services
{
namespace
{

using util::Error;
using util::Result;
namespace fs = std::filesystem;

/// 一次导出接受的直接选中对象数上限（与剪贴板容量 ED-034 对齐）。
constexpr std::size_t kMaxExportItems = 200;
/// 继承链深度上限：解析器已拒绝循环，这里再兜一层损坏文件。
constexpr int kMaxExtendsDepth = 64;

std::string to_utf8(const fs::path& path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

Result<std::string> read_file_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected(
            Error::make<Error::Kind::IoError>("Cannot open source file: " + to_utf8(path)));
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::unexpected(
            Error::make<Error::Kind::IoError>("Cannot read source file: " + to_utf8(path)));
    return bytes;
}

/// 按名称定位顶层 symbol 节点，返回完整原文（含括号）。
/// 依赖 token 级的 find_node（quote 安全、只认顶层），不是子串搜索。
Result<std::string> locate_node(std::string_view file_text, const fs::path& source,
                                const std::string& name)
{
    const auto open = sexpr::find_node(file_text, "symbol", name);
    if (!open.has_value())
        return std::unexpected(Error::make<Error::Kind::ServiceError>(
            "Source file " + to_utf8(source) + " has no top-level symbol named \"" + name +
            "\"; the database and the file are out of sync"));
    const auto close = sexpr::find_matching_paren(file_text, *open);
    if (close == std::string_view::npos)
        return std::unexpected(Error::make<Error::Kind::ParseError>(
            "Unbalanced symbol node \"" + name + "\" in " + to_utf8(source)));
    return std::string(file_text.substr(*open, close - *open + 1));
}

/// 节点自带的 (extends "Parent")；没有继承返回空 optional。
Result<std::optional<std::string>> extends_of(std::string_view node_text, const std::string& name)
{
    sexpr::DomBuilder builder;
    auto parsed = builder.parse_one(node_text);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* ext = (*parsed)->find_child("extends");
    if (ext == nullptr)
        return std::optional<std::string>{};
    if (!ext->is_atom() || ext->atom_value().empty())
        return std::unexpected(Error::make<Error::Kind::ParseError>(
            "Symbol \"" + name + "\" has an (extends ...) node without a parent name"));
    return std::make_optional(std::string(ext->atom_value()));
}

/// 已读入的源文件：整文件原文 + 摘要，一次导出内复用（digest 只算一次）。
struct SourceText
{
    std::string text;
    std::string digest;
};

struct Resolved
{
    const core::Symbol* symbol{nullptr};
    const core::LibraryMeta* library{nullptr};
    std::string path;  // UTF-8
};

}  // namespace

util::Result<ExportResult> export_symbols(storage::Database& db,
                                          const std::vector<core::Uuid>& ids)
{
    if (ids.empty())
        return std::unexpected(Error::make<Error::Kind::ValidationError>(
            "ids must contain at least one symbol id"));
    if (ids.size() > kMaxExportItems)
        return std::unexpected(Error::make<Error::Kind::ValidationError>(
            "Too many symbols to export: " + std::to_string(ids.size()) + " > " +
            std::to_string(kMaxExportItems)));

    // 去重但保留请求顺序；非法 id 直接拒绝，不做「跳过算成功」。
    std::vector<std::string> ordered;
    ordered.reserve(ids.size());
    std::unordered_set<std::string> seen;
    for (const auto& id : ids)
    {
        if (id.empty() || id.size() > 64)
            return std::unexpected(
                Error::make<Error::Kind::ValidationError>("Invalid symbol id"));
        if (seen.insert(id).second)
            ordered.push_back(id);
    }

    std::vector<core::Symbol> symbols;
    std::unordered_map<std::string, core::LibraryMeta> libraries;
    {
        // 只在读 DB 期间持有 session；文件读取放在锁外，避免长事务。
        auto session = db.session();
        storage::LibraryRepository lib_repo(session);
        auto libs = lib_repo.find_all();
        if (!libs)
            return std::unexpected(libs.error());
        for (auto& lib : *libs)
            libraries[lib.id] = lib;

        storage::SymbolRepository sym_repo(session);
        symbols.reserve(ordered.size());
        for (const auto& id : ordered)
        {
            auto symbol = sym_repo.find_by_id(id);
            if (!symbol)
                return std::unexpected(symbol.error());  // NotFound 原样上报
            symbols.push_back(std::move(*symbol));
        }
    }

    // 解析库归属 + 直接选中项的 (源文件, 名称) 键：依赖插入时跳过这些键，
    // 保证「既是父符号又被直接选中」的对象只出现一次且 dependency=false。
    std::vector<Resolved> resolved;
    resolved.reserve(symbols.size());
    std::unordered_set<std::string> direct_keys;
    for (const auto& symbol : symbols)
    {
        auto lib_it = libraries.find(symbol.library_id());
        if (lib_it == libraries.end())
            return std::unexpected(Error::make<Error::Kind::NotFound>(
                "Library of symbol \"" + symbol.name() +
                "\" is not registered: " + symbol.library_id()));
        const auto& lib = lib_it->second;
        if (lib.file_path.empty())
            return std::unexpected(Error::make<Error::Kind::NotFound>(
                "Library \"" + lib.name + "\" has no source file path"));
        Resolved row;
        row.symbol = &symbol;
        row.library = &lib;
        row.path = to_utf8(lib.file_path);
        direct_keys.insert(row.path + '\n' + symbol.name());
        resolved.push_back(std::move(row));
    }

    ExportResult result;
    std::unordered_map<std::string, SourceText> files;
    std::unordered_set<std::string> exported;  // path + '\n' + name

    auto load_file = [&](const fs::path& path) -> Result<const SourceText*>
    {
        const auto key = to_utf8(path);
        auto it = files.find(key);
        if (it != files.end())
            return &it->second;
        auto bytes = read_file_bytes(path);
        if (!bytes)
            return std::unexpected(bytes.error());
        SourceText loaded;
        loaded.digest = io::content_digest(*bytes);
        loaded.text = std::move(*bytes);
        auto [pos, inserted] = files.emplace(key, std::move(loaded));
        return &pos->second;
    };

    for (const auto& row : resolved)
    {
        const auto& symbol = *row.symbol;
        const auto& lib = *row.library;
        auto source = load_file(lib.file_path);
        if (!source)
            return std::unexpected(source.error());
        const SourceText& file = **source;

        auto node = locate_node(file.text, lib.file_path, symbol.name());
        if (!node)
            return std::unexpected(node.error());

        // 继承闭包：父节点必须也在同一文件里（SymbolLibParser 就是这么约束的）。
        std::vector<std::string> dep_names;  // 近 → 远
        std::vector<std::string> dep_texts;
        std::unordered_set<std::string> chain{symbol.name()};
        std::string current_name = symbol.name();
        std::string current_text = *node;
        for (int depth = 0; depth < kMaxExtendsDepth; ++depth)
        {
            auto parent = extends_of(current_text, current_name);
            if (!parent)
                return std::unexpected(parent.error());
            if (!parent->has_value())
                break;
            const std::string parent_name = **parent;
            if (!chain.insert(parent_name).second)
                return std::unexpected(Error::make<Error::Kind::ParseError>(
                    "Symbol \"" + symbol.name() + "\" has a cyclic (extends ...) chain at \"" +
                    parent_name + "\""));
            auto parent_text = locate_node(file.text, lib.file_path, parent_name);
            if (!parent_text)
                return std::unexpected(parent_text.error());
            dep_names.push_back(parent_name);
            dep_texts.push_back(*parent_text);
            current_name = parent_name;
            current_text = *parent_text;
        }
        if (dep_names.size() >= static_cast<std::size_t>(kMaxExtendsDepth))
            return std::unexpected(Error::make<Error::Kind::ParseError>(
                "Symbol \"" + symbol.name() + "\" has an (extends ...) chain deeper than " +
                std::to_string(kMaxExtendsDepth)));

        // 依赖在前：父/祖父……在前，子在后（ED-030 的恢复顺序）。
        for (std::size_t i = dep_names.size(); i > 0; --i)
        {
            const auto& dep_name = dep_names[i - 1];
            const auto key = row.path + '\n' + dep_name;
            if (direct_keys.count(key) != 0 || !exported.insert(key).second)
                continue;
            ExportedSymbol dep;
            dep.name = dep_name;
            dep.library_id = lib.id;
            dep.library_name = lib.name;
            dep.source_path = row.path;
            dep.source_digest = file.digest;
            dep.sexpr = dep_texts[i - 1];
            dep.dependency = true;
            result.items.push_back(std::move(dep));
        }

        const auto key = row.path + '\n' + symbol.name();
        if (!exported.insert(key).second)
            continue;  // 防御：同一 (库, 名称) 不会重复
        ExportedSymbol item;
        item.uuid = symbol.id();
        item.kf_id = symbol.Kicad_Forge_ID();
        item.name = symbol.name();
        item.library_id = lib.id;
        item.library_name = lib.name;
        item.source_path = row.path;
        item.source_digest = file.digest;
        item.sexpr = std::move(*node);
        item.dependency = false;
        result.items.push_back(std::move(item));
    }

    return result;
}

}  // namespace kforge::services
