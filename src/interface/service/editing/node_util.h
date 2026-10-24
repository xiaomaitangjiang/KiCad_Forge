#pragma once

// 宿主编辑服务共用的 KiCad 文本工具（S2 粘贴 / S3 移动）。
//
// 这里的函数只做「文本 ↔ 节点」的机械操作：token 级定位、改名、剥离 KF、
// 读取并校验 .kicad_sym 文件。它们不访问数据库、不做写授权、不决定业务错误码；
// 调用方负责把失败映射成对外稳定错误码。

#include <algorithm>
#include <cctype>
#include <cstddef>
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

#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/dom/dom_builder.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/sexpr/token/tokenizer.h"
#include "core/io/source_mutation.h"
#include "util/error.h"

namespace kforge::services
{

namespace fs = std::filesystem;

using util::Error;
using util::Result;

/// 名称上限与 S2/S3 的粘贴/移动契约一致。
inline constexpr std::size_t kMaxEditingNameLength = 255;

inline std::string to_utf8(const fs::path& path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

inline std::string lower_copy(std::string_view text)
{
    std::string out(text);
    for (auto& ch : out)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

inline std::string upper_copy(std::string_view text)
{
    std::string out(text);
    for (auto& ch : out)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

inline bool is_space(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

inline std::string_view trim_view(std::string_view text)
{
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    return text;
}

inline std::string escape_sexpr(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 2);
    for (const char ch : text)
    {
        if (ch == '"' || ch == '\\')
            out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

inline std::string quote_text(std::string_view text)
{
    return '"' + escape_sexpr(text) + '"';
}

/// 符号名必须能安全地放进 (symbol "name" ...) 并原样比较。
/// 带引号/反斜杠/控制字符的名字拒绝：编辑路径按原始 token 文本匹配，
/// 不做转义猜测，避免把不同对象当成同一个。
inline bool is_plain_name(std::string_view name, std::string& why)
{
    if (name.empty())
    {
        why = "name is empty";
        return false;
    }
    if (name.size() > kMaxEditingNameLength)
    {
        why = "name is longer than " + std::to_string(kMaxEditingNameLength) + " bytes";
        return false;
    }
    for (const char ch : name)
    {
        const auto uch = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\')
        {
            why = "name must not contain quotes or backslashes";
            return false;
        }
        if (uch < 0x20 || uch == 0x7f)
        {
            why = "name must not contain control characters";
            return false;
        }
    }
    return true;
}

inline bool is_digest_text(std::string_view text)
{
    if (text.size() != 16)
        return false;
    for (const char ch : text)
    {
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0)
            return false;
    }
    return true;
}

inline Result<bool> path_exists(const fs::path& path)
{
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec)
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            "Cannot stat file " + to_utf8(path) + ": " + ec.message()));
    return exists;
}

inline Result<std::string> read_file_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::unexpected(
            Error::make<Error::Kind::IoError>("Cannot open file: " + to_utf8(path)));
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::unexpected(
            Error::make<Error::Kind::IoError>("Cannot read file: " + to_utf8(path)));
    return bytes;
}

/// 去掉节点自带的 KF 属性：粘贴副本的身份由宿主重新生成，不能沿用剪贴板里的
/// 旧指纹。
inline std::string strip_kf_properties(std::string_view node)
{
    sexpr::Tokenizer tokenizer(node);
    auto tokens = tokenizer.tokenize_all();
    if (!tokens)
        return std::string(node);

    std::vector<std::pair<std::size_t, std::size_t>> ranges;  // [open, close]
    for (std::size_t i = 0; i + 2 < tokens->size(); ++i)
    {
        const auto& tok = (*tokens)[i];
        if (tok.type != sexpr::TokenType::LParen)
            continue;
        const auto& type = (*tokens)[i + 1];
        const auto& key = (*tokens)[i + 2];
        if (type.type != sexpr::TokenType::Atom || type.text != "property")
            continue;
        if (key.type != sexpr::TokenType::String && key.type != sexpr::TokenType::Atom)
            continue;
        if (key.text != "Kicad_Forge_ID" && key.text != "Pre_Kicad_Forge_ID")
            continue;
        const auto open = static_cast<std::size_t>(tok.text.data() - node.data());
        const auto close = sexpr::find_matching_paren(node, open);
        if (close == std::string_view::npos)
            return std::string(node);
        ranges.emplace_back(open, close);
    }
    if (ranges.empty())
        return std::string(node);

    std::string out;
    out.reserve(node.size());
    std::size_t cursor = 0;
    for (const auto& [open, close] : ranges)
    {
        if (open < cursor)
            continue;  // 嵌套/重叠的损坏异常：只删最外层一次
        out.append(node.substr(cursor, open - cursor));
        cursor = close + 1;
    }
    out.append(node.substr(cursor));
    return out;
}

inline std::string normalize_node(std::string_view node)
{
    const std::string stripped = strip_kf_properties(node);
    return std::string(trim_view(stripped));
}

struct Replacement
{
    std::size_t begin{0};
    std::size_t end{0};
    std::string text;
};

/// 改名 + 依赖父改名重写，同时剥离旧 KF 属性。
/// 只替换 token 级的目标（symbol 名、unit 子符号名前缀、extends 原子），
/// 图形/属性/未知节点逐字保留。
inline Result<std::string> rewrite_symbol_node(std::string_view node, std::string_view source_name,
                                               std::string_view final_name,
                                               std::string_view source_parent,
                                               std::string_view final_parent,
                                               bool drop_kf = true)
{
    sexpr::Tokenizer tokenizer(node);
    auto tokens = tokenizer.tokenize_all();
    if (!tokens)
        return std::unexpected(Error::make<Error::Kind::ValidationError>(
            "Symbol \"" + std::string(source_name) + "\" cannot be tokenized: " +
            tokens.error().format_message()));

    std::vector<Replacement> replacements;
    int depth = 0;
    for (std::size_t i = 0; i < tokens->size(); ++i)
    {
        const auto& tok = (*tokens)[i];
        if (tok.type == sexpr::TokenType::LParen)
        {
            const auto open = static_cast<std::size_t>(tok.text.data() - node.data());
            if (i + 2 < tokens->size() && (*tokens)[i + 1].type == sexpr::TokenType::Atom &&
                (*tokens)[i + 1].text == "symbol")
            {
                const auto& name_tok = (*tokens)[i + 2];
                if (name_tok.type == sexpr::TokenType::Atom ||
                    name_tok.type == sexpr::TokenType::String)
                {
                    const std::string_view raw = name_tok.text;
                    std::string renamed;
                    if (depth == 0)
                    {
                        renamed = std::string(final_name);
                    }
                    else if (raw.size() > source_name.size() &&
                             raw.compare(0, source_name.size(), source_name) == 0 &&
                             raw[source_name.size()] == '_')
                    {
                        // 多单元子符号名 = 父名 + "_<unit>_<style>"，跟着改名。
                        renamed = std::string(final_name) +
                                  std::string(raw.substr(source_name.size()));
                    }
                    if (!renamed.empty() && renamed != raw)
                    {
                        const auto begin =
                            static_cast<std::size_t>(name_tok.text.data() - node.data());
                        Replacement replacement;
                        replacement.begin = begin;
                        replacement.end = begin + raw.size();
                        // String token 的 text 不含两侧引号（偏移落在引号内），
                        // 替换时只能换掉内容；Atom 则需要自己补引号。
                        replacement.text = name_tok.type == sexpr::TokenType::String
                                               ? escape_sexpr(renamed)
                                               : quote_text(renamed);
                        replacements.push_back(std::move(replacement));
                    }
                }
            }
            if (drop_kf && depth >= 1 && i + 2 < tokens->size() &&
                (*tokens)[i + 1].type == sexpr::TokenType::Atom &&
                (*tokens)[i + 1].text == "property")
            {
                const auto& key = (*tokens)[i + 2];
                if ((key.type == sexpr::TokenType::String ||
                     key.type == sexpr::TokenType::Atom) &&
                    (key.text == "Kicad_Forge_ID" || key.text == "Pre_Kicad_Forge_ID"))
                {
                    const auto close = sexpr::find_matching_paren(node, open);
                    if (close == std::string_view::npos)
                        return std::unexpected(Error::make<Error::Kind::ValidationError>(
                            "Symbol \"" + std::string(source_name) +
                            "\" has unbalanced KF property"));
                    Replacement drop;
                    drop.begin = open;
                    drop.end = close + 1;
                    replacements.push_back(std::move(drop));
                }
            }
            ++depth;
        }
        else if (tok.type == sexpr::TokenType::RParen)
        {
            --depth;
        }
        else if (tok.type == sexpr::TokenType::Atom && tok.text == "extends" && i > 0 &&
                 i + 1 < tokens->size() &&
                 (*tokens)[i - 1].type == sexpr::TokenType::LParen &&
                 ((*tokens)[i + 1].type == sexpr::TokenType::Atom ||
                  (*tokens)[i + 1].type == sexpr::TokenType::String))
        {
            const auto& parent_tok = (*tokens)[i + 1];
            if (parent_tok.text == source_parent && source_parent != final_parent &&
                !final_parent.empty())
            {
                const auto begin =
                    static_cast<std::size_t>(parent_tok.text.data() - node.data());
                Replacement replacement;
                replacement.begin = begin;
                replacement.end = begin + parent_tok.text.size();
                replacement.text = parent_tok.type == sexpr::TokenType::String
                                       ? escape_sexpr(final_parent)
                                       : quote_text(final_parent);
                replacements.push_back(std::move(replacement));
            }
        }
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const Replacement& lhs, const Replacement& rhs) { return lhs.begin < rhs.begin; });
    std::string out;
    out.reserve(node.size() + 32);
    std::size_t cursor = 0;
    for (const auto& rep : replacements)
    {
        if (rep.begin < cursor)
            return std::unexpected(Error::make<Error::Kind::ValidationError>(
                "Symbol \"" + std::string(source_name) +
                "\" rewrite produced overlapping ranges"));
        out.append(node.substr(cursor, rep.begin - cursor));
        out.append(rep.text);
        cursor = rep.end;
    }
    out.append(node.substr(cursor));

    // 自检：改写结果仍必须是一个合法 symbol 节点。
    sexpr::DomBuilder builder;
    auto dom = builder.parse_one(out);
    if (!dom || (*dom)->type() != "symbol")
        return std::unexpected(Error::make<Error::Kind::ValidationError>(
            "Symbol \"" + std::string(source_name) + "\" could not be rewritten safely"));
    return out;
}

/// 顶层 symbol 的 (extends "Parent")；没有继承返回空 optional。
inline Result<std::optional<std::string>> symbol_extends_of(std::string_view node_text,
                                                            std::string_view name)
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
            "Symbol \"" + std::string(name) +
            "\" has an (extends ...) node without a parent name"));
    return std::make_optional(std::string(ext->atom_value()));
}

/// 目标/源文件里的顶层内容。root_close 用来把新节点插到根节点右括号之前。
struct FileState
{
    bool exists{false};
    std::string text;
    std::string digest;
    std::size_t root_close{0};
    std::vector<std::string> symbol_names;
    std::unordered_map<std::string, std::string> parent_of;  // name -> extends parent
};

/// 读取并校验一个 .kicad_sym 文件。role 只用于错误消息（如 "Target library"）。
inline Result<FileState> read_library_file(const fs::path& path, std::string_view role)
{
    FileState state;
    const auto exists = path_exists(path);
    if (!exists)
        return std::unexpected(exists.error());
    state.exists = *exists;
    if (!state.exists)
        return state;

    auto bytes = read_file_bytes(path);
    if (!bytes)
        return std::unexpected(bytes.error());
    state.text = std::move(*bytes);
    state.digest = io::content_digest(state.text);

    const std::string label = std::string(role) + " file " + to_utf8(path);
    const auto first = state.text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            label + " is empty; refusing to overwrite it"));
    if (state.text[first] != '(')
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            label + " does not start with a top-level node"));
    const auto close = sexpr::find_matching_paren(state.text, first);
    if (close == std::string::npos)
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            label + " has unbalanced parentheses"));
    for (std::size_t i = close + 1; i < state.text.size(); ++i)
    {
        if (!is_space(state.text[i]))
            return std::unexpected(Error::make<Error::Kind::InternalError>(
                label + " has trailing content after the root node"));
    }
    state.root_close = close;

    sexpr::DomBuilder builder;
    auto root = builder.build(state.text);
    if (!root || (*root)->children().size() != 1 ||
        (*root)->children().front()->type() != "kicad_symbol_lib")
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            label + " is not a kicad_symbol_lib file"));
    std::unordered_set<std::string> seen;
    for (const auto& child : (*root)->children().front()->children())
    {
        if (child->type() != "symbol")
            continue;
        const std::string name(child->atom_value());
        if (name.empty())
            return std::unexpected(Error::make<Error::Kind::InternalError>(
                label + " has a symbol without a name"));
        if (!seen.insert(name).second)
            return std::unexpected(Error::make<Error::Kind::InternalError>(
                label + " defines symbol \"" + name + "\" more than once"));
        state.symbol_names.push_back(name);
        std::string parent;
        if (const auto* ext = child->find_child("extends"); ext != nullptr && ext->is_atom())
            parent = std::string(ext->atom_value());
        state.parent_of[name] = std::move(parent);
    }

    // 文件本身必须能被解析器接受（extends 闭包完整），否则不能在上面追加/删除。
    auto parsed = parser::SymbolLibParser::parse_buffer(state.text, "");
    if (!parsed)
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            label + " cannot be parsed: " + parsed.error().format_message()));
    return state;
}

inline Result<std::string> file_symbol_node(const FileState& file, std::string_view name,
                                            std::string_view role)
{
    const auto open = sexpr::find_node(file.text, "symbol", name);
    if (!open.has_value())
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            std::string(role) + " no longer defines symbol \"" + std::string(name) +
            "\" (concurrent change?)"));
    const auto close = sexpr::find_matching_paren(file.text, *open);
    if (close == std::string::npos)
        return std::unexpected(Error::make<Error::Kind::InternalError>(
            std::string(role) + " has an unbalanced symbol \"" + std::string(name) + "\""));
    return file.text.substr(*open, close - *open + 1);
}

}  // namespace kforge::services
