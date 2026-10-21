#include "interface/api/staged_import.h"

#include "classifier/type_matching.h"
#include "core/db/database.h"
#include "core/db/source_key.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/sexpr/text_util.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include "interface/service/library/footprint_resolver.h"
#include "plugin/plugin_manager.h"
#include "util/logger.h"
#include "util/platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <sqlite3.h>
#include <string_view>
#include <unordered_map>

namespace kforge::api
{

namespace fs = std::filesystem;
using Kind = util::Error::Kind;

namespace
{

/// 标识符也在数据库/文件锁之外生成（暂存目录名、来源映射行 id）：
/// 随机引擎与分布在每个线程各自一份，不能是共享 static —— 并发请求会在
/// 引擎内部状态上数据竞争。
std::string make_identifier()
{
    thread_local std::mt19937_64 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, 15);
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

util::Error io_error(std::string message)
{
    return util::Error::make<Kind::IoError>(std::move(message));
}

/// 组件库包的符号路径没配具体文件时，包内默认的符号库文件名
constexpr const char* kDefaultSymbolFile = "LCSC.kicad_sym";

std::string ascii_lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// 组件库包的符号落点（用户澄清：选的是元件库包，最终写进它的符号库）：
///   * 目录（含明确待创建目录）→ 其下 LCSC.kicad_sym；
///   * .kicad_sym 常规文件 / 明确待创建文件 → 该文件本身；
///   * 其他普通文件 → 拒绝（绝不能把一个符号目录直接当成 .kicad_sym 文件）。
util::Result<fs::path> component_symbol_target(const std::string& symbol_path)
{
    fs::path configured = services::path_from_utf8(symbol_path);
    std::error_code error;
    bool exists = fs::exists(configured, error);
    if (error)
        return std::unexpected(
            io_error("Cannot inspect the component library symbol path: " + symbol_path + ": " +
                     error.message()));
    const std::string lower = ascii_lower(symbol_path);
    const bool looks_like_symbol_file = ends_with(lower, ".kicad_sym");
    if (!exists)
        return looks_like_symbol_file ? configured : configured / kDefaultSymbolFile;

    auto status = fs::status(configured, error);
    if (error)
        return std::unexpected(
            io_error("Cannot inspect the component library symbol path: " + symbol_path + ": " +
                     error.message()));
    if (fs::is_directory(status))
        return configured / kDefaultSymbolFile;
    if (fs::is_regular_file(status) && looks_like_symbol_file)
        return configured;
    return std::unexpected(invalid(
        "Component library symbol path must be a directory or a .kicad_sym file: " + symbol_path));
}

/// 组件库包的封装落点目录：
///   * 配置的就是（或明确将成为）一个 .pretty 目录 → 该目录本身；
///   * 普通根目录 → 其下 <符号文件 stem>.pretty。
/// 返回的是**目录**，封装文件名由调用方按导入的封装名拼。
util::Result<fs::path> component_footprint_target_dir(const std::string& footprint_path,
                                                      const std::string& symbol_stem)
{
    fs::path configured = services::path_from_utf8(footprint_path);
    // 尾部分隔符会让 filename() 变成空：'…/Pkgs.pretty/' 会被误判成普通根目录，
    // 于是又套一层 <stem>.pretty（'Pkgs.pretty/LCSC.pretty'）。先去掉尾部的空分量，
    // 但根路径（'/'、'C:/'）本身没有相对分量，不会被剥成空路径。
    while (configured.has_relative_path() && configured.filename().empty())
        configured = configured.parent_path();
    std::error_code error;
    bool exists = fs::exists(configured, error);
    if (error)
        return std::unexpected(
            io_error("Cannot inspect the component library footprint path: " + footprint_path +
                     ": " + error.message()));
    if (exists)
    {
        auto status = fs::status(configured, error);
        if (error)
            return std::unexpected(
                io_error("Cannot inspect the component library footprint path: " + footprint_path +
                         ": " + error.message()));
        if (!fs::is_directory(status))
            return std::unexpected(invalid(
                "Component library footprint path must be a directory: " + footprint_path));
    }
    if (ends_with(ascii_lower(configured.filename().string()), ".pretty"))
        return configured;
    return configured / services::path_from_utf8(symbol_stem + ".pretty");
}

/// 操作 ID 是外部输入，且历史上被直接拼进文件系统路径。
/// 只接受一小段无路径语义的字符：路径分隔符、父目录分量、绝对路径、
/// 盘符/根名、首尾点全部在字符集这一步就被挡掉。
bool is_safe_operation_id(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id)
    {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

/// Windows 的保留设备名按「第一个点之前的主干」判定：
/// CON.foo / NUL.txt / com1.kicad_mod 同样是设备名，只看整个字符串会漏掉。
bool is_reserved_device_name(const std::string& name)
{
    static const char* names[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                  "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                  "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    auto dot = name.find('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    if (stem.empty())
        return false;
    std::string upper;
    upper.reserve(stem.size());
    for (char c : stem)
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const char* reserved : names)
        if (upper == reserved)
            return true;
    return false;
}

/// 清单里的名字必须是一个合法、单一的 KiCad 名字：
/// 空、路径分量（. / ..）、分隔符、Windows 非法字符/保留名、尾点尾空格全部拒绝。
/// 名字只当显示与属性值用，绝不参与磁盘路径拼接。
std::string validate_single_name(const std::string& name, const char* what)
{
    if (name.empty())
        return std::string("Empty ") + what + " name in manifest";
    if (name == "." || name == "..")
        return std::string("Invalid ") + what + " name in manifest";
    if (name.size() > 255)
        return std::string(what) + " name is too long";
    for (char c : name)
    {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f)
            return std::string(what) + " name contains a control character";
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return std::string(what) + " name contains a path or reserved character";
    }
    if (name.back() == '.' || name.back() == ' ')
        return std::string(what) + " name ends with a dot or space";
    if (is_reserved_device_name(name))
        return std::string(what) + " name is a reserved device name";
    return {};
}

/// 公共协议只接受 generic 相对路径，逐分量校验使 Windows/Linux 判定一致。
util::Result<void> validate_symbol_relative_path(const std::string& relative)
{
    if (relative.empty() || relative.front() == '/' || relative.back() == '/')
        return std::unexpected(invalid("symbol_library_choice.path must be a relative file path"));
    std::size_t start = 0;
    while (start < relative.size())
    {
        auto end = relative.find('/', start);
        const auto part = relative.substr(start, end == std::string::npos ? end : end - start);
        if (auto problem = validate_single_name(part, "symbol library path component");
            !problem.empty())
            return std::unexpected(invalid(std::move(problem)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!ends_with(ascii_lower(relative), ".kicad_sym"))
        return std::unexpected(invalid("symbol_library_choice.path must name a .kicad_sym file"));
    return {};
}

util::Result<std::string> normalize_symbol_library_name(std::string name)
{
    constexpr std::string_view extension = ".kicad_sym";
    if (ends_with(ascii_lower(name), std::string(extension)))
        name.resize(name.size() - extension.size());
    if (ends_with(ascii_lower(name), std::string(extension)))
        return std::unexpected(invalid("symbol library name must not repeat the .kicad_sym suffix"));
    if (auto problem = validate_single_name(name, "symbol library"); !problem.empty())
        return std::unexpected(invalid(std::move(problem)));
    if (name.size() + extension.size() > 255)
        return std::unexpected(invalid("symbol library name is too long"));
    return name;
}

bool missing_entry(const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory;
}

/// symlink_status alone misses Windows junctions; reparse attributes cover both.
util::Result<bool> is_redirected(const fs::path& path, const fs::file_status& status)
{
    if (fs::is_symlink(status)) return true;
#ifdef _WIN32
    if (status.type() != fs::file_type::not_found)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return std::unexpected(io_error("Cannot inspect symbol library attributes: " +
                                             services::path_to_utf8(path)));
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
#endif
    return false;
}

/// 检查完整祖先链，不能让包内路径经过目录链接跳到另一个物理来源。
util::Result<fs::file_status> inspect_symbol_path(const fs::path& path)
{
    std::error_code error;
    const auto absolute = fs::absolute(path, error).lexically_normal();
    if (error)
        return std::unexpected(io_error("Cannot resolve symbol library path: " + error.message()));
    fs::path current = absolute.root_path();
    auto status = fs::symlink_status(current, error);
    if (error)
        return std::unexpected(io_error("Cannot inspect symbol library root: " + error.message()));
    for (const auto& part : absolute.relative_path())
    {
        if (!fs::is_directory(status))
            return std::unexpected(invalid("Symbol library parent is not a directory: " +
                                           services::path_to_utf8(current)));
        current /= part;
        status = fs::symlink_status(current, error);
        if (status.type() == fs::file_type::not_found && (!error || missing_entry(error)))
            return fs::file_status(fs::file_type::not_found);
        if (error)
            return std::unexpected(io_error("Cannot inspect symbol library path: " +
                                             services::path_to_utf8(current) + ": " +
                                             error.message()));
        auto redirected = is_redirected(current, status);
        if (!redirected) return std::unexpected(redirected.error());
        if (*redirected)
            return std::unexpected(invalid("Symbol library path contains a link or redirection: " +
                                           services::path_to_utf8(current)));
    }
    return status;
}

struct ComponentSymbolRoot
{
    fs::path path;
    bool single_file;
};

util::Result<ComponentSymbolRoot> component_symbol_root(const std::string& configured)
{
    if (configured.empty())
        return std::unexpected(invalid("Component library has no symbol path"));
    const auto path = services::path_from_utf8(configured);
    auto status = inspect_symbol_path(path);
    if (!status) return std::unexpected(status.error());
    const bool file_extension = ends_with(ascii_lower(configured), ".kicad_sym");
    if (fs::is_directory(*status)) return ComponentSymbolRoot{path, false};
    if (status->type() == fs::file_type::not_found)
        return ComponentSymbolRoot{path, file_extension};
    if (fs::is_regular_file(*status) && file_extension)
        return ComponentSymbolRoot{path, true};
    return std::unexpected(invalid(
        "Component library symbol path must be a directory or a .kicad_sym file: " + configured));
}

util::Result<fs::path> selected_symbol_target(const std::string& configured,
                                             const StagedImporter::Request& request)
{
    auto root = component_symbol_root(configured);
    if (!root) return std::unexpected(root.error());
    fs::path target;
    if (request.symbol_library_mode == "existing")
    {
        if (auto valid = validate_symbol_relative_path(request.symbol_library_path); !valid)
            return std::unexpected(valid.error());
        auto relative = services::path_from_utf8(request.symbol_library_path);
        if (root->single_file)
        {
            if (relative.has_parent_path() ||
                storage::source_key(root->path.parent_path() / relative) !=
                    storage::source_key(root->path))
                return std::unexpected(invalid(
                    "A single-file component library only accepts its configured symbol file"));
            target = root->path;
        }
        else
            target = root->path / relative;
    }
    else if (request.symbol_library_mode == "new")
    {
        if (root->single_file)
            return std::unexpected(invalid("Cannot create a child library for a single-file symbol configuration"));
        auto name = normalize_symbol_library_name(request.symbol_library_name);
        if (!name) return std::unexpected(name.error());
        target = root->path / services::path_from_utf8(*name + ".kicad_sym");
    }
    else
        return std::unexpected(invalid("Unknown symbol library mode: " + request.symbol_library_mode));
    if (!storage::path_belongs_to(target, root->path))
        return std::unexpected(invalid("Symbol library target escapes its component library"));
    auto status = inspect_symbol_path(target);
    if (!status) return std::unexpected(status.error());
    return target;
}

util::Result<void> check_selected_symbol_state(const fs::path& path,
                                               const std::string& mode, bool after_write = false)
{
    auto status = inspect_symbol_path(path);
    if (!status) return std::unexpected(status.error());
    if (mode == "new" && !after_write)
    {
        if (status->type() != fs::file_type::not_found)
            return std::unexpected(invalid("Symbol library already exists: " +
                                           services::path_to_utf8(path)));
    }
    else if (!fs::is_regular_file(*status))
        return std::unexpected(invalid("Selected symbol library is missing or is not a regular file: " +
                                       services::path_to_utf8(path)));
    return {};
}

using ComponentLibrary = storage::ComponentLibraryRepository::ComponentLibrary;

util::Result<ComponentLibrary> writable_component(const storage::DbSession& session,
                                                  const std::string& id)
{
    auto libraries = storage::ComponentLibraryRepository(session).find_all();
    if (!libraries) return std::unexpected(libraries.error());
    for (const auto& library : *libraries)
    {
        if (library.id != id) continue;
        if (!library.enabled)
            return std::unexpected(invalid("Component library is disabled: " + id));
        if (library.locked || library.is_protected)
            return std::unexpected(invalid("Component library is locked or protected: " + id));
        return library;
    }
    return std::unexpected(util::Error::make<Kind::NotFound>("Component library not found: " + id));
}

util::Result<void> check_symbol_library_owner(const storage::DbSession& session,
                                              const fs::path& target, const std::string& owner)
{
    auto registered = storage::LibraryRepository(session).find_by_source_key(storage::source_key(target));
    if (!registered && registered.error().kind() != Kind::NotFound)
        return std::unexpected(registered.error());
    if (registered && !registered->component_library_id.empty() &&
        registered->component_library_id != owner)
        return std::unexpected(invalid("Symbol library is owned by another component library: " +
                                       services::path_to_utf8(target)));
    return {};
}

/// 清单里的相对路径必须真的落在暂存目录内。
/// 判定顺序：先按「路径分量」拒绝 root/绝对路径/.. —— 合法名字里含 ".." 字串
/// （如 a..b.kicad_mod）不能被字符串包含误杀；再做规范化后的归属检查。
util::Result<fs::path> resolve_staged_path(const fs::path& staging, const std::string& relative)
{
    if (relative.empty())
        return std::unexpected(invalid("Empty file path in manifest"));
    fs::path relative_path(relative);
    if (relative_path.is_absolute() || relative_path.has_root_name() ||
        relative_path.has_root_directory())
        return std::unexpected(invalid("Manifest path must be relative: " + relative));
    for (const auto& component : relative_path)
    {
        if (component == "..")
            return std::unexpected(invalid("Manifest path escapes staging dir: " + relative));
    }
    fs::path candidate = staging / relative_path;
    std::error_code error;
    if (!fs::is_regular_file(candidate, error) || error)
        return std::unexpected(invalid("Staged file is not a regular file: " + relative));
    auto canonical = fs::weakly_canonical(candidate, error);
    auto root = fs::weakly_canonical(staging, error);
    if (error || canonical == root || !storage::path_belongs_to(canonical, root))
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

    std::vector<std::pair<std::string, std::string>> missing;
    for (const auto& [name, node] : nodes)
    {
        if (auto start = sexpr::find_node(merged, "symbol", name))
        {
            auto end = sexpr::find_matching_paren(merged, *start);
            if (end == std::string_view::npos)
                return {};
            // 原位替换（而不是删掉再插回去）：同一份产物重跑一次得到逐字节
            // 相同的库文件，基于最终宿主输出的摘要才有意义
            merged.replace(*start, end - *start + 1, node);
        }
        else
        {
            missing.emplace_back(name, node);
        }
    }

    if (!missing.empty())
    {
        auto last_paren = merged.find_last_of(')');
        if (last_paren == std::string::npos)
            return {};
        std::string insertion;
        for (const auto& [name, node] : missing)
        {
            insertion += "\n";
            insertion += node;
            insertion += "\n";
        }
        merged.insert(last_paren, insertion);
    }
    return merged;
}

/// 摘要的边界：只覆盖**最终宿主输出**里属于这次导入的部分 ——
/// 导入的那个符号节点、最终封装文本、封装落点与所选封装的身份
/// （reuse 用 UUID，new 用目标来源键）。
/// 结构化 JSON 给出无歧义的字段边界；不 hash 整个库文本，否则同库里
/// 任何一个无关符号被改动都会把本来源误判成「要重新导入」。
/// 只对原始插件文本去重则会把「换了复用封装」当成重复导入。
std::string host_output_digest(const std::string& symbol_node,
                               const std::string& footprint_content,
                               const std::string& footprint_identity,
                               const std::string& footprint_path)
{
    nlohmann::json boundary;
    boundary["version"] = 1;
    boundary["symbol"] = symbol_node;
    boundary["footprint"] = footprint_content;
    boundary["footprint_identity"] = footprint_identity;
    boundary["footprint_path"] = footprint_path;
    return io::content_digest(boundary.dump());
}

const core::Symbol* find_symbol(const core::Library<core::Symbol>& lib, const std::string& name)
{
    for (const auto& item : lib.items)
        if (item.name() == name)
            return &item;
    return nullptr;
}

/// 节点内第一个直接子节点里、类型名整体匹配的那一个的位置（找不到返回 npos）。
/// 直接子节点 = 深度 1 到 2 的那层；字符串字面量整体跳过。
std::size_t first_child_of_type(std::string_view text, std::size_t node_start,
                                std::size_t node_end, std::string_view type)
{
    int depth = 0;
    bool in_string = false;
    for (std::size_t index = node_start; index < node_end; ++index)
    {
        char c = text[index];
        if (in_string)
        {
            if (c == '\\')
                ++index;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
        }
        else if (c == '(')
        {
            ++depth;
            if (depth == 2 && index + 1 + type.size() < text.size() &&
                text.compare(index + 1, type.size(), type) == 0)
            {
                char next = text[index + 1 + type.size()];
                if (next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == '"')
                    return index;
            }
        }
        else if (c == ')')
        {
            --depth;
            if (depth == 0)
                break;
        }
    }
    return std::string_view::npos;
}

/// 属性值按 KiCad 字符串字面量转义（与 replace_property_value 的写法一致）
std::string quoted_value(const std::string& value)
{
    std::string result = "\"";
    for (char c : value)
    {
        if (c == '\\' || c == '"')
            result += '\\';
        result += c;
    }
    result += '"';
    return result;
}

/// 宿主改写最终符号的 Footprint 属性：有就替换；没有就按 KiCad 的节点结构
/// 插到第一条 property（或第一个子单元）之前。插入之后必须用仓库既有解析器
/// 复核这个符号真的带上了引用 —— 绝不能出现「文件里没有引用、数据库里有绑定」。
bool set_footprint_property(std::string& text, const std::string& symbol_name,
                            const std::string& reference)
{
    if (sexpr::replace_property_value(text, "symbol", symbol_name, "Footprint", reference))
        return true;
    auto start = sexpr::find_node(text, "symbol", symbol_name);
    if (!start)
        return false;
    auto end = sexpr::find_matching_paren(text, *start);
    if (end == std::string_view::npos)
        return false;
    std::size_t insert_at = first_child_of_type(text, *start, end, "property");
    if (insert_at == std::string_view::npos)
        insert_at = first_child_of_type(text, *start, end, "symbol");
    if (insert_at == std::string_view::npos)
        insert_at = end;
    text = sexpr::insert_property(text, insert_at, "Footprint", quoted_value(reference));
    // 复核：补出来的节点要能被仓库既有解析器读回同一个引用
    static const char* wrapper_head = "(kicad_symbol_lib (version 20231120) (generator KiCad_Forge)\n";
    auto reparsed = parser::SymbolLibParser::parse_buffer(wrapper_head + text + "\n)\n");
    if (!reparsed)
        return false;
    const core::Symbol* node = find_symbol(*reparsed, symbol_name);
    return node != nullptr && node->footprint() == reference;
}

/// 取最终宿主输出里这个符号的节点文本（摘要与 no-op 比对都以它为准）
std::string final_symbol_node(const std::string& library_text, const std::string& name)
{
    auto start = sexpr::find_node(library_text, "symbol", name);
    if (!start)
        return {};
    auto end = sexpr::find_matching_paren(library_text, *start);
    if (end == std::string_view::npos)
        return {};
    return library_text.substr(*start, end - *start + 1);
}

/// 宿主自己维护、不属于来源元数据的 property：源文件没有提供时可以从旧行保留。
/// 白名单只有 value_range（由分类/规则界面写进库，来源文件从不携带）；
/// _pins_json / _pin_count 是仓储从 pins() 派生的存储键，永远不从来源或旧行复制。
constexpr const char* kHostPropertyKeys[] = {"value_range"};

/// 复制的列文本：sqlite3_column_text 的指针只在 finalize 之前有效，
/// finalize 之后再读就是已释放内存（真实复现：JSON 里冒出 0xD3 的坏字节）。
std::string column_text_copy(sqlite3_stmt* stmt, int index)
{
    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    return text ? std::string(text) : std::string();
}

/// JSON 字段取字符串：类型错误在取用之前变成具体消息
/// （value() 对类型不符会抛异常，落到路由外层的 catch 就只剩 "Execute error"）。
util::Result<std::string> optional_json_string(const nlohmann::json& object, const char* key)
{
    if (!object.contains(key))
        return std::string();
    if (!object[key].is_string())
        return std::unexpected(invalid(std::string(key) + " must be a string"));
    return object[key].get<std::string>();
}

util::Result<std::string> choice_json_string(const nlohmann::json& choice, const char* key)
{
    if (!choice.contains(key))
        return std::string();
    if (!choice[key].is_string())
        return std::unexpected(
            invalid(std::string("footprint_choice.") + key + " must be a string"));
    return choice[key].get<std::string>();
}

}  // namespace

util::Result<std::string> canonical_lcsc_external_id(const std::string& value)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };

    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && space(static_cast<unsigned char>(value[begin])))
        ++begin;
    while (end > begin && space(static_cast<unsigned char>(value[end - 1])))
        --end;
    std::string text = value.substr(begin, end - begin);
    for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (!text.empty() && std::all_of(text.begin(), text.end(), digit))
        text = "C" + text;
    const bool canonical =
        text.size() >= 2 && text[0] == 'C' && std::all_of(text.begin() + 1, text.end(), digit);
    if (!canonical)
        return std::unexpected(invalid("Invalid LCSC part number: " + value));
    return text;
}

util::Result<StagedRequestFields> parse_staged_request(const nlohmann::json& body,
                                                       const std::string& plugin_id,
                                                       const std::string& action)
{
    StagedRequestFields parsed;
    StagedImporter::Request& request = parsed.request;
    request.plugin_id = plugin_id;
    request.action = action;

    for (const char* key :
         {"lcsc_id", "source", "operation_id", "provider", "target_library_id", "target_library"})
    {
        if (body.contains(key) && !body[key].is_string())
            return std::unexpected(invalid(std::string(key) + " must be a string"));
    }
    // provider 的类型检查先于取用：先 value() 的话类型不符会抛异常，
    // 具体消息就被外层 catch 吞成 "Execute error"
    request.provider = body.contains("provider") ? body["provider"].get<std::string>() : "LCSC";
    request.external_id = body.contains("lcsc_id") ? body["lcsc_id"].get<std::string>()
                                                   : body.value("source", std::string());
    request.target_library_id = body.value("target_library_id", std::string());
    request.target_library_path = body.value("target_library", std::string());
    request.operation_id = body.value("operation_id", std::string());

    if (body.contains("target_component_library_id"))
    {
        if (!body["target_component_library_id"].is_string())
            return std::unexpected(invalid("target_component_library_id must be a string"));
        request.target_component_library_id =
            body["target_component_library_id"].get<std::string>();
        if (request.target_component_library_id.empty())
            return std::unexpected(invalid("target_component_library_id must not be empty"));
    }

    if (body.contains("symbol_library_choice"))
    {
        if (request.target_component_library_id.empty() || !request.target_library_id.empty() ||
            !request.target_library_path.empty())
            return std::unexpected(invalid(
                "symbol_library_choice requires only target_component_library_id"));
        const auto& symbol_choice = body["symbol_library_choice"];
        if (!symbol_choice.is_object())
            return std::unexpected(invalid("symbol_library_choice must be an object"));
        for (const char* field : {"mode", "path", "name"})
            if (symbol_choice.contains(field) && !symbol_choice[field].is_string())
                return std::unexpected(invalid(std::string("symbol_library_choice.") + field +
                                               " must be a string"));
        request.symbol_library_mode = symbol_choice.value("mode", std::string());
        if (request.symbol_library_mode == "existing")
        {
            if (symbol_choice.contains("name"))
                return std::unexpected(invalid("symbol_library_choice.name is only valid in new mode"));
            request.symbol_library_path = symbol_choice.value("path", std::string());
            if (auto valid = validate_symbol_relative_path(request.symbol_library_path); !valid)
                return std::unexpected(valid.error());
        }
        else if (request.symbol_library_mode == "new")
        {
            if (symbol_choice.contains("path"))
                return std::unexpected(invalid("symbol_library_choice.path is only valid in existing mode"));
            auto name = normalize_symbol_library_name(symbol_choice.value("name", std::string()));
            if (!name) return std::unexpected(name.error());
            request.symbol_library_name = symbol_choice["name"].get<std::string>();
        }
        else
            return std::unexpected(invalid("symbol_library_choice.mode must be \"existing\" or \"new\""));
    }

    if (body.contains("footprint_choice") && !body["footprint_choice"].is_object())
        return std::unexpected(invalid("footprint_choice must be an object"));
    const nlohmann::json choice = body.contains("footprint_choice")
                                      ? body["footprint_choice"]
                                      : nlohmann::json::object();

    // 显式封装 UUID 的三个位置：footprint_choice.id（README/API 文档示例）、
    // footprint_choice.footprint_id、顶层 footprint_id（旧前端）。三者等价，
    // 同时给出且不一致说明请求自相矛盾，必须拒绝而不是任选一个。
    auto top_level = optional_json_string(body, "footprint_id");
    if (!top_level)
        return std::unexpected(top_level.error());
    auto nested_id = choice_json_string(choice, "id");
    if (!nested_id)
        return std::unexpected(nested_id.error());
    auto nested_footprint_id = choice_json_string(choice, "footprint_id");
    if (!nested_footprint_id)
        return std::unexpected(nested_footprint_id.error());

    std::string explicit_id;
    for (const std::string* candidate : {&*top_level, &*nested_id, &*nested_footprint_id})
    {
        if (candidate->empty())
            continue;
        if (!explicit_id.empty() && *candidate != explicit_id)
            return std::unexpected(invalid("Conflicting footprint ids: footprint_choice.id, "
                                           "footprint_choice.footprint_id and footprint_id must "
                                           "agree"));
        explicit_id = *candidate;
    }

    if (!request.target_component_library_id.empty())
    {
        // 元件库包模式：必须显式选择 —— 没有 footprint_choice、没有 mode、
        // 非法 mode、reuse 没有候选 UUID 都是拒绝，不默认 top1 复用，
        // 也不把「没选」当成新建
        if (!body.contains("footprint_choice"))
            return std::unexpected(
                invalid("footprint_choice is required for a component library target"));
        if (!choice.contains("mode") || !choice["mode"].is_string())
            return std::unexpected(invalid("footprint_choice.mode must be \"new\" or \"reuse\""));
        request.footprint_mode = choice["mode"].get<std::string>();
        if (request.footprint_mode != "new" && request.footprint_mode != "reuse")
            return std::unexpected(invalid("Unknown footprint mode: " + request.footprint_mode));
        // new 不允许携带任何旧 UUID：不能把复用候选悄悄带进新建
        if (!explicit_id.empty() && request.footprint_mode == "new")
            return std::unexpected(invalid("footprint_id is only valid in reuse mode"));
        if (request.footprint_mode == "reuse" && explicit_id.empty())
            return std::unexpected(invalid("reuse requires the chosen footprint id"));
        request.footprint_id = explicit_id;
    }
    else
    {
        // 旧符号库入口：mode 默认 "new"，library/name 的唯一解析在调用方查库完成
        if (choice.contains("mode") && !choice["mode"].is_string())
            return std::unexpected(invalid("footprint_choice.mode must be a string"));
        request.footprint_mode = choice.value("mode", std::string("new"));
        request.footprint_id = explicit_id;
        auto library = choice_json_string(choice, "library");
        if (!library)
            return std::unexpected(library.error());
        auto name = choice_json_string(choice, "name");
        if (!name)
            return std::unexpected(name.error());
        parsed.legacy_library = *library;
        parsed.legacy_name = *name;
    }
    return parsed;
}

util::Result<nlohmann::json> list_component_symbol_libraries(storage::Database& db,
                                                           const std::string& component_id)
{
    ComponentLibrary component;
    std::unordered_map<std::string, core::LibraryMeta> registered;
    {
        auto session = db.session();
        auto group = writable_component(session, component_id);
        if (!group) return std::unexpected(group.error());
        component = *group;
        auto rows = storage::LibraryRepository(session).find_all();
        if (!rows) return std::unexpected(rows.error());
        for (const auto& row : *rows)
        {
            if (!row.source_key.empty()) registered.emplace(row.source_key, row);
        }
    }  // 磁盘遍历期间不占用数据库独占会话。
    auto root = component_symbol_root(component.symbol_path);
    if (!root) return std::unexpected(root.error());
    if (io::is_official_kicad_source(fs::path(storage::source_key(root->path))))
        return std::unexpected(invalid("Target library lives in an official KiCad directory"));
    nlohmann::json result{{"ok", true}, {"libraries", nlohmann::json::array()},
                          {"can_create", !root->single_file}};
#ifdef _WIN32
    result["case_sensitive"] = false;
#else
    result["case_sensitive"] = true;
#endif
    auto append = [&](const fs::path& file) -> util::Result<void>
    {
        auto inspected = inspect_symbol_path(file);
        if (!inspected) return std::unexpected(inspected.error());
        if (!fs::is_regular_file(*inspected))
            return std::unexpected(invalid("Symbol library changed while listing: " +
                                           services::path_to_utf8(file)));
        const auto key = storage::source_key(file);
        if (key.empty())
            return std::unexpected(invalid("Cannot normalize symbol library path"));
        const auto found = registered.find(key);
        if (found != registered.end() && !found->second.component_library_id.empty() &&
            found->second.component_library_id != component_id)
            return std::unexpected(invalid("Symbol library is owned by another component library: " +
                                           services::path_to_utf8(file)));
        const auto relative = root->single_file ? file.filename() : file.lexically_relative(root->path);
        const auto relative_utf8 = relative.generic_u8string();
        result["libraries"].push_back({
            {"name", services::path_to_utf8(file.stem())},
            {"path", std::string(relative_utf8.begin(), relative_utf8.end())},
            {"id", found == registered.end() ? std::string() : found->second.id}});
        return {};
    };
    auto status = inspect_symbol_path(root->path);
    if (!status) return std::unexpected(status.error());
    if (status->type() == fs::file_type::not_found) return result;
    if (root->single_file)
    {
        if (auto added = append(root->path); !added) return std::unexpected(added.error());
    }
    else
    {
        std::error_code error;
        fs::recursive_directory_iterator current(root->path, error), end;
        if (error) return std::unexpected(io_error("Cannot list symbol libraries: " + error.message()));
        while (current != end)
        {
            const auto file = current->path();
            const auto entry = fs::symlink_status(file, error);
            if (error) return std::unexpected(io_error("Cannot inspect symbol library entry: " + error.message()));
            auto redirected = is_redirected(file, entry);
            if (!redirected) return std::unexpected(redirected.error());
            if (*redirected)
                current.disable_recursion_pending();
            else if (fs::is_regular_file(entry) &&
                     ascii_lower(services::path_to_utf8(file.extension())) == ".kicad_sym")
            {
                if (auto added = append(file); !added) return std::unexpected(added.error());
            }
            current.increment(error);
            if (error) return std::unexpected(io_error("Cannot list symbol libraries: " + error.message()));
        }
    }
    auto& libraries = result["libraries"];
    std::sort(libraries.begin(), libraries.end(), [](const auto& a, const auto& b)
              { return a.at("path").template get<std::string>() <
                       b.at("path").template get<std::string>(); });
    return result;
}

util::Result<nlohmann::json> StagedImporter::run(const Request& raw_request,
                                                const RunHooks& hooks)
{
    // Phase-4 task hooks: cancellation is only honoured while staging; the
    // commit gate below shares the task lock with requestCancel().
    const auto cancelled = [&hooks]() { return hooks.cancelled && hooks.cancelled(); };
    const auto cancelled_error = []
    {
        return util::Error::make<Kind::PluginError>("Task cancelled during staging");
    };

    // 本地副本：编号规范化只影响本次执行，不改调用方拿着的请求对象
    Request request = raw_request;
    if (request.external_id.empty())
        return std::unexpected(invalid("Missing external id"));
    if (!request.operation_id.empty() && !is_safe_operation_id(request.operation_id))
        return std::unexpected(invalid("Invalid operation id"));
    if (cancelled())
        return std::unexpected(cancelled_error());

    // ---- 1. 解析目标库：必须是已登记、可写的用户库 ----
    // 两种模式互斥：旧模式给符号库（libraries 行）UUID/路径；新模式给元件库包
    // （component_libraries 行，符号库 + 封装库的组合）UUID。
    const bool component_mode = !request.target_component_library_id.empty();
    const bool selected_child = !request.symbol_library_mode.empty();
    if ((!component_mode && (selected_child || !request.symbol_library_path.empty() ||
                             !request.symbol_library_name.empty())) ||
        (!selected_child && (!request.symbol_library_path.empty() ||
                             !request.symbol_library_name.empty())))
        return std::unexpected(invalid("symbol_library_choice requires a component library target and mode"));
    if ((request.symbol_library_mode == "existing" && !request.symbol_library_name.empty()) ||
        (request.symbol_library_mode == "new" && !request.symbol_library_path.empty()))
        return std::unexpected(invalid("Conflicting symbol library choice fields"));
    // 真实 LCSC 插件 + 元件库包模式：编号在任何幂等/来源逻辑之前规范化，
    // 否则 ' c123 ' 与 C123 会算成两个来源（第二次会被判异源）。其余插件、
    // 以及旧符号库入口的请求摘要一律保持原样。
    const bool canonical_lcsc = component_mode && request.plugin_id == "com.kicad_forge.lcsc_import";
    if (canonical_lcsc)
    {
        auto canonical = canonical_lcsc_external_id(request.external_id);
        if (!canonical)
            return std::unexpected(canonical.error());
        request.external_id = *canonical;
    }
    if (component_mode &&
        (!request.target_library_id.empty() || !request.target_library_path.empty()))
        return std::unexpected(invalid(
            "Specify either target_component_library_id or target_library_id, not both"));

    std::string target_symbol_path;
    std::string target_footprint_root;
    std::string target_library_id;  // 符号库 UUID；新模式首次导入时为空（空包还没登记行）
    std::string target_owner;       // 目标组件库 UUID：文件变更的真实归属
    std::string target_identity;    // 摘要里的目标身份（新模式不依赖符号库 UUID）
    fs::path component_footprint_dir;   // 新模式 new 的封装落点目录
    std::string component_footprint_prefix;
    std::string target_footprint_configured;  // 新模式：包的封装路径（授权/归属基准）
    std::string target_symbol_configured;
    if (component_mode)
    {
        std::string component_id = request.target_component_library_id;
        std::string symbol_path;
        std::string footprint_path;
        {
            auto session = db_->session();
            auto groups = storage::ComponentLibraryRepository(session).find_all();
            if (!groups)
                return std::unexpected(groups.error());  // 查询失败不能当「查无此包」
            bool found = false;
            for (const auto& group : *groups)
            {
                if (group.id != component_id)
                    continue;
                found = true;
                if (!group.enabled)
                    return std::unexpected(invalid("Component library is disabled: " + component_id));
                if (group.locked || group.is_protected)
                    return std::unexpected(
                        invalid("Component library is locked or protected: " + component_id));
                symbol_path = group.symbol_path;
                footprint_path = group.footprint_path;
                break;
            }
            if (!found)
                return std::unexpected(invalid("Component library not found: " + component_id));
        }
        if (symbol_path.empty() || footprint_path.empty())
            return std::unexpected(
                invalid("Component library has no symbol or footprint path: " + component_id));
        target_symbol_configured = symbol_path;
        auto symbol_target = selected_child ? selected_symbol_target(symbol_path, request)
                                            : component_symbol_target(symbol_path);
        if (!symbol_target)
            return std::unexpected(symbol_target.error());
        const std::string symbol_key = storage::source_key(*symbol_target);
        if (symbol_key.empty())
            return std::unexpected(invalid("Cannot normalize the component library symbol path: " +
                                           symbol_path));
        target_symbol_path = services::path_to_utf8(*symbol_target);
        target_owner = component_id;
        // 摘要身份 = 包 ID + 规范目标路径：首次（还没有符号库 UUID）与之后
        // （已登记、有了 UUID）必须算出同一个身份，同一个 operation_id 才能回放。
        target_identity = "component:" + component_id + "|" + symbol_key;

        // 已登记的符号库行：核对真实归属 —— 别人（别的元件库包）的行不能抢
        auto session = db_->session();
        auto registered =
            storage::LibraryRepository(session).find_by_source_key(symbol_key);
        if (registered)
        {
            target_library_id = registered->id;
            if (!registered->component_library_id.empty() &&
                registered->component_library_id != component_id)
                return std::unexpected(invalid(
                    "Symbol library is owned by another component library: " + target_symbol_path));
        }
        else if (registered.error().kind() != Kind::NotFound)
        {
            return std::unexpected(registered.error());
        }
        // 没有已登记行也照常继续：空包首次导入在提交成功后才登记 LibraryMeta

        if (io::is_official_kicad_source(fs::path(symbol_key)) ||
            io::is_official_kicad_source(fs::path(storage::source_key(footprint_path))))
            return std::unexpected(invalid("Target library lives in an official KiCad directory"));

        // 封装落点目录由包的封装路径决定（与是否已有封装文件无关，摘要才稳定）
        target_footprint_configured = footprint_path;
        auto footprint_dir = component_footprint_target_dir(
            footprint_path, services::path_to_utf8(services::path_from_utf8(target_symbol_path).stem()));
        if (!footprint_dir)
            return std::unexpected(footprint_dir.error());
        component_footprint_dir = *footprint_dir;
        component_footprint_prefix = services::path_to_utf8(component_footprint_dir.filename().stem());
        const std::string footprint_dir_key = storage::source_key(component_footprint_dir);
        if (footprint_dir_key.empty())
            return std::unexpected(
                invalid("Cannot normalize the component library footprint path: " + footprint_path));
        target_identity += "|" + footprint_dir_key;
    }
    else
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
            // 正向确认：目录已登记且未被锁定/保护，并且真的配了 root
            writable = !group.locked && !group.is_protected && !group.symbol_path.empty() &&
                       !group.footprint_path.empty();
            target_footprint_root = group.footprint_path;
            target_owner = group.id;
            break;
        }
        if (!writable)
            return std::unexpected(invalid("Target library is locked, protected or incomplete"));

        target_symbol_path = services::path_to_utf8(target.file_path);
        target_library_id = target.id;
        if (io::is_official_kicad_source(fs::path(storage::source_key(target_symbol_path))))
            return std::unexpected(invalid("Target library lives in an official KiCad directory"));
        if (io::is_official_kicad_source(fs::path(storage::source_key(target_footprint_root))))
            return std::unexpected(invalid("Target library root lives in an official KiCad directory"));
        target_identity = target_library_id;  // 旧摘要身份保持不变
    }

    // 封装模式必须明确：组件库模式（新请求）没有默认值 —— 空值就是「没做选择」，
    // 既不能默认 top1 复用，也不能默默新建；旧符号库协议保留 "new" 的默认语义。
    std::string footprint_mode = request.footprint_mode;
    if (footprint_mode.empty())
    {
        if (component_mode)
            return std::unexpected(
                invalid("A component library target requires an explicit footprint mode"));
        footprint_mode = "new";
    }

    // reuse 只接受已登记的封装 UUID。这里先只做「请求字段」层面的验证 ——
    // 行本身要等幂等回放判定之后才查：历史成功回放不能被「这一行后来被删了」挡住。
    if (footprint_mode == "reuse" && request.footprint_id.empty())
        return std::unexpected(invalid("reuse requires a footprint id"));
    if (footprint_mode != "reuse" && footprint_mode != "new")
        return std::unexpected(invalid("Unknown footprint mode: " + footprint_mode));

    // ---- 2. 幂等：同 ID 同内容返回原结果，同 ID 不同内容拒绝 ----
    // 身份由 target_identity 决定：旧模式 = 符号库 UUID（原摘要逐字节不变）；
    // 新模式 = 包 ID + 规范目标路径（首次还没有符号库 UUID 时也算得出一致的身份）。
    const std::string request_digest = io::content_digest(
        request.plugin_id + "|" + request.action + "|" + request.provider + "|" +
        request.external_id + "|" + target_identity + "|" + footprint_mode + "|" +
        request.footprint_id);
    const std::string operation_id =
        request.operation_id.empty() ? make_identifier() : request.operation_id;
    {
        auto session = db_->session();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT request_digest, result_json FROM import_operations WHERE id = ?1";
        // 读不出来就必须失败：把查询错误当「没有旧记录」会放行一次本应回放的请求
        if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            return std::unexpected(util::Error::make<Kind::DbError>(
                "Cannot read operation log: " + std::string(sqlite3_errmsg(session.handle()))));
        sqlite3_bind_text(stmt, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(stmt);
        bool recorded_before = false;
        std::string stored_digest;
        std::string stored_result;
        if (step == SQLITE_ROW)
        {
            // 列指针在 finalize 之后失效：先拷成 std::string
            stored_digest = column_text_copy(stmt, 0);
            stored_result = column_text_copy(stmt, 1);
            recorded_before = true;
            step = sqlite3_step(stmt);  // 主键唯一：不应再有一行
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE)
            return std::unexpected(util::Error::make<Kind::DbError>(
                "Cannot read operation log: " + std::string(sqlite3_errmsg(session.handle()))));
        if (recorded_before)
        {
            if (stored_digest != request_digest)
                return std::unexpected(invalid("Operation id reused with different request"));
            try
            {
                auto json = nlohmann::json::parse(stored_result.empty() ? "{}" : stored_result);
                json["replayed"] = true;
                return json;
            }
            catch (const std::exception& error)
            {
                return std::unexpected(util::Error::make<Kind::DbError>(
                    std::string("Stored operation result is not JSON: ") + error.what()));
            }
        }
    }

    // new 的存在性检查必须晚于历史成功回放，否则网络未知后的同 operation 重试
    // 会因为首次导入已经创建了文件而失败。existing 的真实文件检查同样只约束新操作。
    if (selected_child)
        if (auto valid = check_selected_symbol_state(
                services::path_from_utf8(target_symbol_path), request.symbol_library_mode); !valid)
            return std::unexpected(valid.error());

    // 历史成功回放不依赖后来修改的配置；新操作先领取健康快照。
    // 租约保持到文件和数据库提交/回滚结束，保护在途旧规则引用的类型。
    auto& matching_store = matching_store_ ? *matching_store_
                                          : classifier::TypeMatchingStore::instance();
    auto matching_snapshot = matching_store.acquire_business_snapshot();
    if (!matching_snapshot)
        return std::unexpected(matching_snapshot.error());

    // ---- 2.5 新写入前的 fresh 复核 ----
    // 历史成功回放已在上面返回（不为修状态重复导入）；走到这里一定是新写入：
    // 复用的封装必须此刻仍然真实存在（行 + 文件 + 身份），否则整次拒绝。
    core::Footprint reused;
    services::FootprintIdentity reused_identity;
    if (footprint_mode == "reuse")
    {
        auto session = db_->session();
        auto found = storage::FootprintRepository(session).find_by_id(request.footprint_id);
        if (!found)
        {
            // 读失败是 DbError，不能包装成「封装不存在」
            if (found.error().kind() != Kind::NotFound)
                return std::unexpected(found.error());
            return std::unexpected(invalid("Footprint not found: " + request.footprint_id));
        }
        reused = *found;
        // 身份快照：提交前要确认还是用户选中的那一行（同 UUID 改名/换源也算漂移）
        reused_identity = services::capture_footprint_identity(reused);
        services::FileCheckContext files;  // 全新上下文 = fresh，不看任何缓存
        auto inspected = services::inspect_footprint_files(reused, files);
        if (inspected.status != services::FootprintStatus::Available)
            return std::unexpected(invalid(
                "Reused footprint is unavailable: " +
                std::string(services::to_string(inspected.status)) + " (" + inspected.reason + ")"));
    }

    // ---- 3. 私有暂存目录 + 插件执行（不持有数据库会话与源文件锁）----    // 目录名永远由宿主生成：外部 operation_id 不是路径的一部分，
    // 同一个 operation_id 的并发请求也因此不会共用同一个暂存目录。
    // 无论成功失败都用 RAII 清掉，不留一次性目录。
    if (hooks.on_phase)
        hooks.on_phase("staging");
    if (cancelled())
        return std::unexpected(cancelled_error());
    fs::path staging_root = db_->path().parent_path() / "staging";
    fs::path staging = staging_root / make_identifier();
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
        fs::create_directories(staging_root, error);
        if (error)
            return std::unexpected(util::Error::make<Kind::IoError>(
                "Cannot create staging root: " + error.message()));
        auto canonical_root = fs::weakly_canonical(staging_root, error);
        auto canonical_staging = fs::weakly_canonical(staging, error);
        if (error || canonical_staging == canonical_root ||
            !storage::path_belongs_to(canonical_staging, canonical_root))
            return std::unexpected(invalid("Staging directory escapes its staging root"));
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
    args["footprint_choice"] = footprint_mode == "reuse"
                                   ? nlohmann::json{{"mode", "reuse"}}
                                   : nlohmann::json{{"mode", "new"}};

    auto executed = plugins_->execute(request.plugin_id, request.action, args.dump(),
                                      hooks.cancelled);
    if (!executed)
        return std::unexpected(executed.error());
    if (cancelled())
        return std::unexpected(cancelled_error());
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

    // ---- 4. 校验清单与产物（全部通过之前不碰目标文件与数据库）----
    if (cancelled())
        return std::unexpected(cancelled_error());
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
    // 清单编号必须就是宿主规范后的编号：插件回传原始写法或别的编号，
    // 说明产物不属于这次请求，不能拿去覆盖目标库。
    if (canonical_lcsc)
    {
        const std::string manifest_id = manifest.value("lcsc_id", std::string());
        if (manifest_id != request.external_id)
            return std::unexpected(invalid("Manifest lcsc_id must equal the canonical request id (" +
                                           request.external_id + "): " + manifest_id));
    }

    // 每个 provider/external_id 当前只支持一个符号：清单必须恰好 1 项，
    // 多项直接 ValidationError，绝不做「只取第一个」的静默降级。
    auto entries = manifest.value("symbols", nlohmann::json::array());
    if (!entries.is_array() || entries.size() != 1)
        return std::unexpected(invalid("Manifest must list exactly one symbol"));

    struct StagedSymbol
    {
        std::string name;
        std::string node;
    };
    StagedSymbol staged_symbol;
    {
        const auto& entry = entries.front();
        auto name = entry.value("name", "");
        auto file = entry.value("file", "");
        if (auto problem = validate_single_name(name, "symbol"); !problem.empty())
            return std::unexpected(invalid(problem));
        auto path = resolve_staged_path(staging, file);
        if (!path)
            return std::unexpected(path.error());
        if (path->extension() != ".kicad_sym")
            return std::unexpected(invalid("Symbol file must be .kicad_sym"));

        std::string text;
        if (!read_file(*path, text))
            return std::unexpected(invalid("Cannot read staged symbol file: " + file));
        // 一个清单项对应文件里唯一一个符号节点，且名字必须对得上：
        // 后面的 Footprint 属性改写就是按这个名字定位的
        auto nodes = sexpr::top_level_nodes(text, "symbol");
        if (nodes.size() != 1 || !sexpr::find_node(text, "symbol", name))
            return std::unexpected(invalid("Staged file must contain exactly the declared symbol: " +
                                           file));
        // 解析结果必须能读回来，并且真的带着声明的那个符号（元数据入库的依据）
        auto parsed = parser::SymbolLibParser::parse(*path);
        if (!parsed || parsed->items.empty() || find_symbol(*parsed, name) == nullptr)
            return std::unexpected(invalid("Staged symbol library does not parse: " + file));
        staged_symbol.name = name;
        staged_symbol.node = nodes.front();
    }

    // 封装：清单名字先过合法名检查（拒绝路径分量/非法字符/保留名/尾点空格），
    // 文件路径先拒绝 root/.. 再检查规范化归属
    std::string staged_footprint_content;
    std::string staged_footprint_name;
    core::Footprint staged_footprint;
    if (manifest.contains("footprint") && manifest["footprint"].is_object())
    {
        auto footprint = manifest["footprint"];
        staged_footprint_name = footprint.value("name", "");
        if (auto problem = validate_single_name(staged_footprint_name, "footprint"); !problem.empty())
            return std::unexpected(invalid(problem));
        auto path = resolve_staged_path(staging, footprint.value("file", ""));
        if (!path)
            return std::unexpected(path.error());
        if (path->extension() != ".kicad_mod")
            return std::unexpected(invalid("Footprint file must be .kicad_mod"));
        auto parsed = parser::FootprintParser::parse(*path);
        if (!parsed)
            return std::unexpected(invalid("Staged footprint does not parse"));
        // 清单名与文件根节点名必须一致：入库用清单名、落盘用文件内容，
        // 两者不一致就是「文件里是 A、数据库里是 B」，只能整体拒绝
        if (parsed->name() != staged_footprint_name)
            return std::unexpected(invalid("Staged footprint name does not match the manifest: " +
                                           parsed->name() + " != " + staged_footprint_name));
        staged_footprint = *parsed;  // 完整 parsed 模型：pads/3D/属性都要入库
        if (!read_file(*path, staged_footprint_content))
            return std::unexpected(invalid("Cannot read staged footprint"));
    }
    if (footprint_mode == "reuse" && !staged_footprint_content.empty())
        return std::unexpected(invalid("reuse must not produce a new footprint"));
    if (footprint_mode == "new" && staged_footprint_content.empty())
        return std::unexpected(invalid("new footprint mode requires a .kicad_mod"));

    // ---- 5. 目标文件内容：合并符号 + 决定封装落点（宿主说了算）----
    if (cancelled())
        return std::unexpected(cancelled_error());
    if (selected_child)
    {
        auto fresh_target = selected_symbol_target(target_symbol_configured, request);
        if (!fresh_target) return std::unexpected(fresh_target.error());
        if (storage::source_key(*fresh_target) !=
            storage::source_key(services::path_from_utf8(target_symbol_path)))
            return std::unexpected(invalid("Symbol library target changed during plugin execution"));
        if (auto valid = check_selected_symbol_state(*fresh_target, request.symbol_library_mode); !valid)
            return std::unexpected(valid.error());
    }
    std::string target_text;
    std::error_code exists_error;
    bool target_exists = fs::is_regular_file(services::path_from_utf8(target_symbol_path), exists_error) && !exists_error;
    if (target_exists && !read_file(services::path_from_utf8(target_symbol_path), target_text))
        return std::unexpected(io_error("Cannot read target symbol library: " + target_symbol_path));

    std::string library_stem = services::path_to_utf8(services::path_from_utf8(target_symbol_path).stem());
    std::string footprint_reference;
    fs::path final_footprint_path;
    std::string footprint_identity;
    if (footprint_mode == "reuse")
    {
        // 引用写法与读取界面一致：footprint_lib_name 优先，旧行为空时回退 .pretty
        // 父目录名；路径文本是 UTF-8 契约，中文库名不能走窄串转换
        auto library_name = reused.footprint_lib_name();
        if (library_name.empty())
            library_name = services::path_to_utf8(
                services::path_from_utf8(reused.library_path()).parent_path().stem());
        footprint_reference = library_name + ":" + reused.name();
        footprint_identity = reused.id();  // 所选封装 UUID 进摘要：A→B 必然不同
    }
    else if (component_mode)
    {
        // 组件库模式：落点目录在解析目标时按包的封装路径定好（.pretty 目录本身，
        // 或普通根目录下的 <符号文件 stem>.pretty），引用前缀就是该目录的 stem
        std::error_code canonical_error;
        auto canonical_root = fs::weakly_canonical(services::path_from_utf8(target_footprint_configured), canonical_error);
        auto canonical_dir = fs::weakly_canonical(component_footprint_dir, canonical_error);
        if (canonical_error)
            return std::unexpected(invalid("Cannot normalize footprint root"));
        final_footprint_path = canonical_dir / services::path_from_utf8(staged_footprint_name + ".kicad_mod");
        // 最终封装目标规范化后必须严格落在组件库包的封装路径内
        if (final_footprint_path == canonical_root ||
            !storage::path_belongs_to(final_footprint_path, canonical_root))
            return std::unexpected(invalid("Footprint target escapes the library footprint root"));
        footprint_reference = component_footprint_prefix + ":" + staged_footprint_name;
        footprint_identity = storage::source_key(final_footprint_path);
        if (footprint_identity.empty())
            return std::unexpected(invalid("Cannot normalize footprint target path"));
    }
    else
    {
        fs::path pretty = services::path_from_utf8(target_footprint_root) /
                          services::path_from_utf8(library_stem + ".pretty");
        std::error_code canonical_error;
        auto canonical_root = fs::weakly_canonical(target_footprint_root, canonical_error);
        auto canonical_pretty = fs::weakly_canonical(pretty, canonical_error);
        if (canonical_error)
            return std::unexpected(invalid("Cannot normalize footprint root"));
        final_footprint_path = canonical_pretty / (staged_footprint_name + ".kicad_mod");
        // 最终封装目标规范化后必须严格落在目标组件库的 footprint root 内
        if (final_footprint_path == canonical_root ||
            !storage::path_belongs_to(final_footprint_path, canonical_root))
            return std::unexpected(invalid("Footprint target escapes the library footprint root"));
        footprint_reference = library_stem + ":" + staged_footprint_name;
        footprint_identity = storage::source_key(final_footprint_path);
        if (footprint_identity.empty())
            return std::unexpected(invalid("Cannot normalize footprint target path"));
    }

    // 封装在数据库/引用里的库名：新模式用落点 .pretty 的 stem（= 引用前缀），
    // 旧模式就是目标符号文件 stem
    const std::string footprint_library_name =
        (component_mode && !final_footprint_path.empty()) ? component_footprint_prefix
                                                          : library_stem;

    std::vector<std::pair<std::string, std::string>> nodes;
    {
        std::string text = staged_symbol.node;
        // 改写结果必须检查：符号里本来没有 Footprint 属性时要么安全补上、
        // 要么明确拒绝 —— 不能文件里没有引用而数据库里却有关联
        if (!set_footprint_property(text, staged_symbol.name, footprint_reference))
            return std::unexpected(invalid("Cannot set the Footprint property on the staged symbol: " +
                                           staged_symbol.name));
        nodes.emplace_back(staged_symbol.name, std::move(text));
    }
    bool created_new = false;
    std::string merged = merge_symbol_nodes(target_text, nodes, created_new);
    if (merged.empty())
        return std::unexpected(invalid("Target library file is malformed"));

    // 宿主改写完之后**再解析一遍**最终文件内容，用它入库：
    // 文件与数据库说的是同一件事，也不另写第二套解析器。
    core::Symbol final_symbol;
    {
        auto reparsed = parser::SymbolLibParser::parse_buffer(merged, library_stem);
        if (!reparsed)
            return std::unexpected(invalid("Merged library does not parse: " +
                                           reparsed.error().format_message()));
        const core::Symbol* node = find_symbol(*reparsed, staged_symbol.name);
        if (node == nullptr)
            return std::unexpected(invalid("Merged library lost the imported symbol"));
        final_symbol = *node;
    }

    // 正则在数据库会话之外执行，且只使用宿主最终写入的真实来源字段。
    const auto type_decision =
        (*matching_snapshot)->evaluate(classifier::MatchSample::from_symbol(final_symbol));

    // 摘要的边界是最终宿主输出：只取导入的那个符号节点 + 最终封装内容/身份/落点，
    // 同库里别的符号被改写不会牵动这一条来源
    const std::string final_node = final_symbol_node(merged, staged_symbol.name);
    if (final_node.empty())
        return std::unexpected(invalid("Merged library lost the imported symbol"));
    const std::string content_digest =
        host_output_digest(final_node, staged_footprint_content, footprint_identity,
                           final_footprint_path.empty() ? std::string()
                                                        : final_footprint_path.string());

    // ---- 6. 先看来源映射：同源且最终宿主输出一致 = no-op ----
    // 取消 / 开始提交的原子裁决：两者共用同一把任务锁（TaskService）。取消
    // 先发生则这里一定返回 false，绝不进入任何文件/数据库写入；提交先发生则
    // 之后的 cancel 请求返回 not_cancelable。
    if (cancelled())
        return std::unexpected(cancelled_error());
    if (hooks.try_enter_commit && !hooks.try_enter_commit())
        return std::unexpected(cancelled_error());
    if (hooks.on_phase)
        hooks.on_phase("committing");
    std::string footprint_id = footprint_mode == "reuse" ? request.footprint_id : "";
    if (footprint_id.empty() && !final_footprint_path.empty())
    {
        // new 模式下第一遍导入已经建过行：no-op 也要能报出真实 UUID
        auto session = db_->session();
        auto key = storage::existing_source_key(final_footprint_path);
        if (!key.empty())
        {
            auto existing = storage::FootprintRepository(session).find_by_source_key(key);
            if (existing)
                footprint_id = existing->id();
            else if (existing.error().kind() != Kind::NotFound)
                return std::unexpected(existing.error());  // 读失败不能当成「还没有行」
        }
    }
    // 组件库模式下符号库行还没登记（空包首次导入）时不可能有来源映射：
    // import_sources 的目标列就是符号库 UUID。已登记行时照常查（查询失败照旧上报）。
    const bool lookup_sources = !component_mode || !target_library_id.empty();
    if (lookup_sources)
    {
        auto session = db_->session();
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT symbol_id, symbol_name, content_digest FROM import_sources "
            "WHERE target_library_id=?1 AND provider=?2 AND external_id=?3";
        // 查询失败 = 不知道有没有旧来源，绝不能按「没有」继续往下写：
        // 那样异源同名保护就被一次查询错误绕过去了
        if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
            return std::unexpected(util::Error::make<Kind::DbError>(
                "Cannot read import sources: " + std::string(sqlite3_errmsg(session.handle()))));
        sqlite3_bind_text(stmt, 1, target_library_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, request.provider.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, request.external_id.c_str(), -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(stmt);
        bool mapped = false;
        std::string mapped_symbol_id;
        std::string mapped_symbol_name;
        std::string previous_digest;
        if (step == SQLITE_ROW)
        {
            // 列指针必须在 finalize 之前拷出来：finalize 之后是已释放内存
            mapped_symbol_id = column_text_copy(stmt, 0);
            mapped_symbol_name = column_text_copy(stmt, 1);
            previous_digest = column_text_copy(stmt, 2);
            mapped = true;
            step = sqlite3_step(stmt);  // 唯一约束下只可能有一行
        }
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE)
            return std::unexpected(util::Error::make<Kind::DbError>(
                "Cannot read import sources: " + std::string(sqlite3_errmsg(session.handle()))));

        // no-op 的前置条件：同源、最终宿主输出一致，而且上次写下的东西还在
        //   * 目标库里这个符号的节点必须还是宿主写下的那一份（被外部改写/删除就不算）
        //   * 新模式下封装文件必须还在、内容与本次产物一致
        //   * 数据库里的符号行与封装关联必须还在
        bool intact = false;
        auto check_intact = [&]() -> util::Result<bool>
        {
            if (!mapped || previous_digest != content_digest)
                return false;
            if (!target_exists)
                return false;
            if (final_symbol_node(target_text, staged_symbol.name) != final_node)
                return false;
            if (!final_footprint_path.empty())
            {
                std::string on_disk;
                if (!read_file(final_footprint_path, on_disk))
                    return false;
                if (io::content_digest(on_disk) != io::content_digest(staged_footprint_content))
                    return false;
            }
            if (footprint_id.empty())
                return false;
            auto symbol_row = storage::SymbolRepository(session).find_by_id(mapped_symbol_id);
            if (!symbol_row)
            {
                // 行不存在 = 符号已被删掉，不能算 no-op；查询错误照旧上报
                if (symbol_row.error().kind() == Kind::NotFound)
                    return false;
                return std::unexpected(symbol_row.error());
            }
            auto link = storage::RelationshipRepository(session).find_symbol_link_rows(
                mapped_symbol_id);
            if (!link)
                return std::unexpected(link.error());
            // 完整关联必须只有目标 UUID 一条：只看首条会把「多出一条旧关联」当 no-op
            return link->size() == 1 && link->front().footprint_id == footprint_id;
        };
        auto intact_result = check_intact();
        if (!intact_result)
            return std::unexpected(intact_result.error());
        intact = *intact_result;

        if (intact)
        {
            // no-op 与成功返回同一组字段：调用方不用为「没重跑」写第二套解析
            nlohmann::json result;
            result["ok"] = true;
            result["noop"] = true;
            result["operation_id"] = operation_id;
            result["lcsc_id"] = request.external_id;
            result["symbol_id"] = mapped_symbol_id;
            result["symbol_name"] = mapped_symbol_name;
            result["footprint_id"] = footprint_id;
            result["footprint_reference"] = footprint_reference;
            result["mode"] = footprint_mode;
            result["created_library"] = false;
            result["warnings"] = nlohmann::json::array();
            // 可回放：同一个 operation_id 再来一次拿到同样的结果。
            // 复用当前 session：同一线程不能再开第二个（会死锁）
            sqlite3_stmt* insert = nullptr;
            const char* insert_sql =
                "INSERT INTO import_operations(id,request_digest,result_json) VALUES(?1,?2,?3)";
            if (sqlite3_prepare_v2(session.handle(), insert_sql, -1, &insert, nullptr) != SQLITE_OK)
                return std::unexpected(util::Error::make<Kind::DbError>(
                    "Cannot record operation: " +
                    std::string(sqlite3_errmsg(session.handle()))));
            std::string payload = result.dump();
            sqlite3_bind_text(insert, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 2, request_digest.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(insert, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
            bool recorded = sqlite3_step(insert) == SQLITE_DONE;
            sqlite3_finalize(insert);
            if (!recorded)
                return std::unexpected(
                    util::Error::make<Kind::DbError>("Cannot record operation result"));
            return result;
        }
    }

    // ---- 7. 一次变更提交全部文件与数据库改动 ----
    std::vector<io::FileChange> changes;
    // 存在性按「宿主刚读到的状态」钉死：已存在就钉摘要，不存在就断言必须不存在。
    // 只给 allow_missing 会把空摘要（不校验）留给取锁前别人新建的同名文件，
    // 那正是「文件里是别人的内容、数据库里记着我们的导入」的来源。
    changes.push_back(io::FileChange{.path = services::path_from_utf8(target_symbol_path),
                                     .content = merged,
                                     .expected_digest = target_exists ? io::content_digest(target_text)
                                                                      : std::string(),
                                     .allow_missing = !target_exists,
                                     .expected_missing = !target_exists,
                                     .owner = target_owner});
    if (!final_footprint_path.empty())
    {
        std::error_code footprint_error;
        bool footprint_exists =
            fs::is_regular_file(final_footprint_path, footprint_error) && !footprint_error;
        std::string existing_footprint;
        if (footprint_exists && !read_file(final_footprint_path, existing_footprint))
            return std::unexpected(
                invalid("Cannot read the existing footprint: " + final_footprint_path.string()));
        changes.push_back(io::FileChange{
            .path = final_footprint_path,
            .content = staged_footprint_content,
            .expected_digest = footprint_exists ? io::content_digest(existing_footprint)
                                                : std::string(),
            .allow_missing = !footprint_exists,
            .expected_missing = !footprint_exists,
            .owner = target_owner});
    }

    std::string symbol_id;
    std::string symbol_name = staged_symbol.name;
    nlohmann::json last_result;

    io::SourceMutationService mutations(*db_);
    if (selected_child)
        if (auto valid = check_selected_symbol_state(
                services::path_from_utf8(target_symbol_path), request.symbol_library_mode); !valid)
            return std::unexpected(valid.error());
    auto applied = mutations.apply(
        changes, [&](storage::DbSession& session) -> util::Result<void>
        {
            if (selected_child)
            {
                auto component = writable_component(session, target_owner);
                if (!component) return std::unexpected(component.error());
                if (component->symbol_path != target_symbol_configured ||
                    component->footprint_path != target_footprint_configured)
                    return std::unexpected(invalid("Component library configuration changed before commit"));
                auto fresh_target = selected_symbol_target(component->symbol_path, request);
                if (!fresh_target) return std::unexpected(fresh_target.error());
                if (storage::source_key(*fresh_target) !=
                    storage::source_key(services::path_from_utf8(target_symbol_path)))
                    return std::unexpected(invalid("Symbol library target changed before commit"));
                if (auto valid = check_selected_symbol_state(*fresh_target, request.symbol_library_mode,
                                                              true); !valid)
                    return std::unexpected(valid.error());
                if (auto owner = check_symbol_library_owner(session, *fresh_target, target_owner); !owner)
                    return std::unexpected(owner.error());
                if (auto allowed = io::authorize_source_write(session, *fresh_target, target_owner); !allowed)
                    return std::unexpected(allowed.error());
            }
            // 最开头就用传入的 session 复核复用行的身份与文件：
            // 插件执行期间同 UUID 被改名/换源/挪到另一份真实文件时，写下的引用
            // 与关联都来自旧行 —— 整次拒绝（宿主文件与事务一起回滚）。
            if (footprint_mode == "reuse")
            {
                auto fresh = storage::FootprintRepository(session).find_by_id(footprint_id);
                if (!fresh)
                {
                    if (fresh.error().kind() != Kind::NotFound)
                        return std::unexpected(fresh.error());
                    return std::unexpected(invalid(
                        "Reused footprint disappeared before commit: " + footprint_id));
                }
                if (!services::same_footprint_identity(
                        reused_identity, services::capture_footprint_identity(*fresh)))
                    return std::unexpected(invalid(
                        "Reused footprint changed before commit: " + footprint_id));
                services::FileCheckContext files;
                auto inspected = services::inspect_footprint_files(*fresh, files);
                if (inspected.status != services::FootprintStatus::Available)
                    return std::unexpected(invalid(
                        "Reused footprint disappeared before commit: " +
                        std::string(services::to_string(inspected.status)) + " (" +
                        inspected.reason + ")"));
            }

            // 目标库行（新建的 .kicad_sym 在这里登记）
            core::LibraryMeta meta;
            meta.name = library_stem;
            meta.file_path = services::path_from_utf8(target_symbol_path);
            meta.component_library_id = target_owner;
            meta.description = "Imported from " + request.provider;
            auto library = storage::LibraryRepository(session).upsert_by_source_key(meta);
            if (!library) return std::unexpected(library.error());
            // 组件库模式空包首次导入：符号库 UUID 正是这一刻才有的。
            // 后续符号行、来源映射、操作记录全部用这个真实 UUID。
            const std::string& source_library_id = library->id;

            if (!final_footprint_path.empty())
            {
                // 身份是来源路径：同一个 .kicad_mod 二次导入要复用原行，
                // 不能靠 INSERT OR IGNORE（它被忽略后会返回一个并不存在的 id）
                storage::FootprintRepository footprints_repo(session);
                auto key = storage::existing_source_key(final_footprint_path);
                core::Footprint footprint = staged_footprint;  // 完整 parsed 模型
                footprint.set_name(staged_footprint_name);
                footprint.set_library_path(final_footprint_path.string());
                footprint.set_source_key(key);
                footprint.set_footprint_lib_name(footprint_library_name);
                if (footprint.description().empty())
                    footprint.set_description("Imported from " + request.provider);
                auto existing = footprints_repo.find_by_source_key(key);
                // 查询失败不能当成「新封装」：那会把别人的行按插入重来一遍
                if (!existing && existing.error().kind() != Kind::NotFound)
                    return std::unexpected(existing.error());
                if (existing)
                {
                    footprint.set_id(existing->id());
                    // 库里已有的分类等非源信息保留
                    if (footprint.package_type_id.empty())
                        footprint.package_type_id = existing->package_type_id;
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
            auto rows = symbols_repo.find_by_library(library->id);
            // 读失败不能当「库里没有同名符号」：那会在既有同名符号之上直接插第二行
            if (!rows)
                return std::unexpected(rows.error());
            for (const auto& candidate : *rows)
            {
                if (candidate.name() == symbol_name)
                {
                    row = candidate;
                    existing_symbol = true;
                    break;
                }
            }
            // 源文件提供 pin/Value/MPN/Description/Datasheet/自定义 properties 的更新；
            // 库里已有的类型 ID / value_range 等非源信息原样保留（白名单见 kHostPropertyKeys）
            row.set_name(symbol_name);
            row.set_library_id(library->id);
            row.set_default_value(final_symbol.default_value());
            row.set_footprint(footprint_reference);
            row.set_datasheet(final_symbol.datasheet());
            row.set_description(final_symbol.description());
            row.set_mpn(final_symbol.mpn());
            if (!final_symbol.reference_prefix().empty())
                row.set_reference_prefix(final_symbol.reference_prefix());
            row.set_power(final_symbol.is_power());
            row.pins() = final_symbol.pins();
            // 源属性整体替换（不是在旧行上逐项叠加）：V2 删掉的来源键
            // （vendor_tag / manufacturer / ki_keywords …）必须跟着消失，
            // 旧行里任何 source key 都不能留下；只有白名单里的宿主字段
            // 可以在源文件没提供时从旧行保留。
            std::unordered_map<std::string, std::string> source_properties;
            for (const auto& [key, value] : final_symbol.properties())
            {
                if (key == "_pins_json" || key == "_pin_count")
                    continue;  // 派生存储键由仓储从 pins() 生成，不算来源元数据
                source_properties[key] = value;
            }
            for (const char* host_key : kHostPropertyKeys)
            {
                if (source_properties.count(host_key) != 0)
                    continue;  // 源文件显式给了就以源为准
                if (auto kept = row.property(host_key))
                    source_properties.emplace(host_key, *kept);
            }
            row.properties() = std::move(source_properties);
            nlohmann::json type_warnings = nlohmann::json::array();
            if (row.component_type_id.empty() || row.component_type_id == "Unknown")
            {
                if (type_decision.matched())
                    row.component_type_id = type_decision.type_id;
                else if (!existing_symbol)
                    row.component_type_id = "Unknown";

                using classifier::MatchStatus;
                switch (type_decision.status)
                {
                    case MatchStatus::Conflict:
                    case MatchStatus::InputTooLong:
                    case MatchStatus::EvaluationLimit:
                    case MatchStatus::EvaluationError:
                        type_warnings.push_back(
                            std::string("Component type matching ") +
                            classifier::match_status_name(type_decision.status) + ": " +
                            type_decision.reason);
                        break;
                    default:
                        break;
                }
            }
            if (row.package_type_id.empty())
                row.package_type_id = final_symbol.package_type_id;
            if (!row.value_range && final_symbol.value_range)
                row.value_range = final_symbol.value_range;
            // 产物里显式给出的 KF/Pre 原样落到行专用字段（文件/数据库一致）；
            // 没有 incoming KF 时按当前身份重算，并把旧值留在 Pre —— 只有身份
            // （名称/引脚签名/封装引用）真的变了才推进版本，改描述/值不换 ID。
            const std::string previous_kf = row.Kicad_Forge_ID();
            if (!final_symbol.Kicad_Forge_ID().empty())
            {
                if (!previous_kf.empty() &&
                    previous_kf != final_symbol.Kicad_Forge_ID())
                    row.set_Pre_Kicad_Forge_ID(previous_kf);
                row.set_Kicad_Forge_ID(final_symbol.Kicad_Forge_ID());
            }
            else
            {
                row.refresh_Kicad_Forge_ID();
            }
            if (!final_symbol.Pre_Kicad_Forge_ID().empty())
                row.set_Pre_Kicad_Forge_ID(final_symbol.Pre_Kicad_Forge_ID());
            if (existing_symbol)
            {
                // 同名符号若已属于别的来源，不能悄悄改写 —— 报冲突让用户决定
                sqlite3_stmt* owner_stmt = nullptr;
                const char* owner_sql =
                    "SELECT external_id FROM import_sources "
                    "WHERE target_library_id=?1 AND provider=?2 AND symbol_id=?3";
                // 查询失败不能当「没有冲突」：那正好绕过异源同名保护
                if (sqlite3_prepare_v2(session.handle(), owner_sql, -1, &owner_stmt, nullptr) !=
                    SQLITE_OK)
                    return std::unexpected(util::Error::make<Kind::DbError>(
                        "Cannot check the symbol source: " +
                        std::string(sqlite3_errmsg(session.handle()))));
                sqlite3_bind_text(owner_stmt, 1, source_library_id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(owner_stmt, 2, request.provider.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(owner_stmt, 3, row.id().c_str(), -1, SQLITE_TRANSIENT);
                int owner_step = sqlite3_step(owner_stmt);
                bool conflict = owner_step == SQLITE_ROW &&
                                column_text_copy(owner_stmt, 0) != request.external_id;
                sqlite3_finalize(owner_stmt);
                if (owner_step != SQLITE_ROW && owner_step != SQLITE_DONE)
                    return std::unexpected(util::Error::make<Kind::DbError>(
                        "Cannot check the symbol source: " +
                        std::string(sqlite3_errmsg(session.handle()))));
                if (conflict)
                    return std::unexpected(invalid(
                        "Symbol name already imported from a different source: " + symbol_name));
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
            // 旧关联可能不止一条：全部删掉，只留新的那一条
            // （复用行的 fresh 复核已在回调最开头做完）
            if (auto unlinked = links.unlink_all_symbol_footprints(symbol_id); !unlinked)
                return std::unexpected(unlinked.error());
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
            sqlite3_bind_text(stmt, 2, source_library_id.c_str(), -1, SQLITE_TRANSIENT);
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

            // 幂等结果与业务改动同一事务：要么一起成功，要么一起回滚，
            // 不允许「文件与业务已提交、操作记录静默丢掉」
            nlohmann::json result;
            result["ok"] = true;
            result["noop"] = false;
            result["operation_id"] = operation_id;
            result["lcsc_id"] = request.external_id;
            result["symbol_id"] = symbol_id;
            result["symbol_name"] = symbol_name;
            result["footprint_id"] = footprint_id;
            result["footprint_reference"] = footprint_reference;
            result["mode"] = footprint_mode;
            result["created_library"] = created_new;
            result["warnings"] = std::move(type_warnings);
            sqlite3_stmt* op_stmt = nullptr;
            if (sqlite3_prepare_v2(session.handle(),
                                   "INSERT INTO import_operations(id,request_digest,result_json) "
                                   "VALUES(?1,?2,?3)",
                                   -1, &op_stmt, nullptr) != SQLITE_OK)
                return std::unexpected(
                    util::Error::make<Kind::DbError>(sqlite3_errmsg(session.handle())));
            std::string payload = result.dump();
            sqlite3_bind_text(op_stmt, 1, operation_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(op_stmt, 2, request_digest.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(op_stmt, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
            bool recorded = sqlite3_step(op_stmt) == SQLITE_DONE;
            sqlite3_finalize(op_stmt);
            if (!recorded)
                return std::unexpected(
                    util::Error::make<Kind::DbError>("Cannot record operation result"));

            last_result = std::move(result);
            return {};
        });
    if (!applied)
        return std::unexpected(applied.error());

    return last_result;  // 暂存目录由 StagingScope 清理
}

}  // namespace kforge::api
