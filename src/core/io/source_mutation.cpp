#include "core/io/source_mutation.h"

#include "core/db/database.h"
#include "core/db/db_session.h"
#include "core/db/source_key.h"
#include "core/repo/repositories.h"
#define XXH_INLINE_ALL  // 与 symbol.cpp 同一用法：头文件内联实现，不额外加链接单元
#include "third_party/xxhash.h"
#include "util/logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <sqlite3.h>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kforge::io
{

namespace fs = std::filesystem;

namespace
{

// ---- 进程内文件锁：按规范路径共享同一把锁，取锁顺序固定（小 → 大） ----

std::mutex g_registry_mutex;
std::map<std::string, std::weak_ptr<std::mutex>>& lock_registry()
{
    static std::map<std::string, std::weak_ptr<std::mutex>> registry;
    return registry;
}

std::vector<std::shared_ptr<std::mutex>> acquire_locks(const std::vector<std::string>& keys)
{
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    std::vector<std::shared_ptr<std::mutex>> locks;
    {
        // 只在建表/查找时持登记表锁；真正的等待在登记表之外做，
        // 避免一次慢操作把所有文件操作都堵在登记表上
        std::lock_guard registry_lock(g_registry_mutex);
        for (const auto& key : sorted)
        {
            auto& slot = lock_registry()[key];
            auto lock = slot.lock();
            if (!lock)
            {
                lock = std::make_shared<std::mutex>();
                slot = lock;
            }
            locks.push_back(std::move(lock));
        }
    }
    // 已按规范路径排序 → 全局取锁顺序一致，不会互相等待
    for (auto& lock : locks)
        lock->lock();
    return locks;
}

void release_locks(std::vector<std::shared_ptr<std::mutex>>& locks)
{
    for (auto it = locks.rbegin(); it != locks.rend(); ++it)
        (*it)->unlock();
}

/// 变更日志 id 在文件锁之外生成（并发路径各有各的锁）：随机引擎必须在
/// 每个线程各自一份，共享 static 会在引擎内部状态上数据竞争。
std::string make_operation_id()
{
    thread_local std::mt19937_64 gen(std::random_device{}());
    thread_local std::uniform_int_distribution<int> dist(0, 15);
    static const char hex[] = "0123456789abcdef";
    std::string id;
    id.reserve(16);
    for (int i = 0; i < 16; ++i)
        id += hex[dist(gen)];
    return id;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return in.good() || in.eof();
}

/// 同目录唯一临时文件；调用方负责清理
bool write_temp(const fs::path& temp, const std::string& content, std::string& error)
{
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        error = "cannot create temp file: " + temp.string();
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out)
    {
        error = "cannot write temp file: " + temp.string();
        return false;
    }
    return true;
}

/// 平台替换：Windows 走 ReplaceFileW（保留属性、可带备份），其余回落到同卷改名
bool replace_target(const fs::path& target, const fs::path& replacement, std::string& error)
{
#ifdef _WIN32
    if (ReplaceFileW(target.wstring().c_str(), replacement.wstring().c_str(), nullptr,
                     REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr))
    {
        return true;
    }
    // 目标不存在或 ReplaceFileW 不支持该卷时回落到改名
#endif
    std::error_code ec;
    fs::rename(replacement, target, ec);
    if (ec)
    {
        error = "replace failed: " + ec.message();
        return false;
    }
    return true;
}

/// 新建目标必须在发布瞬间仍然不存在；预检与发布之间可能有其他进程抢占。
/// 任意目录项已占位都整体失败，绝不回退到会覆盖目标的 replace/rename。
bool publish_new_target(const fs::path& target, const fs::path& replacement, std::string& error)
{
    std::string reason;
#ifdef _WIN32
    if (MoveFileW(replacement.c_str(), target.c_str()))
        return true;  // MoveFileW 不带替换标志，已有目标会原子拒绝。
    // system_category().message() 在中文 Windows/MinGW 可能返回 CP936，
    // 不能直接拼进 UTF-8 JSON 错误；稳定文本与数字码仍能准确定位失败。
    reason = "Windows error " + std::to_string(GetLastError());
#else
    // 临时文件与目标在同一目录：标准库硬链接以单次文件系统操作排他发布。
    // 成功后临时名字交给现有统一清理；清理失败不能把已发布项当成未发布。
    std::error_code ec;
    fs::create_hard_link(replacement, target, ec);
    if (!ec)
        return true;
    reason = ec.message();
#endif
    const auto target_utf8 = target.u8string();
    error = "cannot publish new source without replacing an existing target: " +
            std::string(target_utf8.begin(), target_utf8.end()) + ": " + reason;
    return false;
}

struct PlannedFile
{
    fs::path path;
    std::string key;      // 规范路径
    fs::path temp;
    fs::path backup;
    bool existed = false;
    bool remove = false;
    std::string old_digest;
    std::string new_digest;
};

std::string plan_to_json(const std::string& operation, const std::vector<PlannedFile>& files)
{
    nlohmann::json array = nlohmann::json::array();
    for (const auto& file : files)
    {
        nlohmann::json item;
        item["path"] = file.path.string();
        item["temp"] = file.temp.string();
        item["backup"] = file.backup.string();
        item["existed"] = file.existed;
        item["remove"] = file.remove;
        item["old_digest"] = file.old_digest;
        item["new_digest"] = file.new_digest;
        array.push_back(std::move(item));
    }
    nlohmann::json root;
    root["operation"] = operation;
    root["files"] = std::move(array);
    return root.dump();
}

void remove_quietly(const fs::path& path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

/// 候选文件的结构自检：括号深度不能变负、收尾必须归零（跳过字符串与转义）。
/// 只对 KiCad S-expression 文件生效 —— 写坏源文件比写不进去严重得多。
bool structurally_sane(const fs::path& path, const std::string& text)
{
    auto extension = path.extension().string();
    if (extension.rfind(".kicad", 0) != 0)
        return true;
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (in_string)
        {
            if (c == '\\')
            {
                ++i;
            }
            else if (c == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '(')
            ++depth;
        else if (c == ')')
        {
            --depth;
            if (depth < 0)
                return false;
        }
    }
    return depth == 0 && !in_string;
}

/// 根路径列表非空即算「配置了 root」
bool has_root(const std::string& root)
{
    return !root.empty();
}

/// 源文件的种类：决定它该落在组件库的哪一个 root 下 ——
/// 符号进符号 root、封装进封装 root、3D 模型进模型 root，三者不能互相顶替。
enum class SourceKind
{
    Symbol,
    Footprint,
    Model,
    Unknown,
};

SourceKind source_kind_of(const fs::path& path)
{
    std::string extension = path.extension().string();
    for (char& c : extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".kicad_sym")
        return SourceKind::Symbol;
    if (extension == ".kicad_mod")
        return SourceKind::Footprint;
    if (extension == ".step" || extension == ".stp" || extension == ".wrl" || extension == ".iges")
        return SourceKind::Model;
    return SourceKind::Unknown;
}

const char* source_kind_name(SourceKind kind)
{
    switch (kind)
    {
    case SourceKind::Symbol: return "symbol";
    case SourceKind::Footprint: return "footprint";
    case SourceKind::Model: return "3d-model";
    default: return "unknown";
    }
}

/// 该种类对应的 root（种类不明时没有对应 root）
const std::string& root_for_kind(
    const storage::ComponentLibraryRepository::ComponentLibrary& library, SourceKind kind)
{
    switch (kind)
    {
    case SourceKind::Symbol: return library.symbol_path;
    case SourceKind::Footprint: return library.footprint_path;
    case SourceKind::Model: return library.model_3d_path;
    default: break;
    }
    static const std::string empty;
    return empty;
}

/// 按 id 取组件库行；查询失败按错误上报，不当作「没有这个库」
util::Result<std::optional<storage::ComponentLibraryRepository::ComponentLibrary>>
find_component_library(const storage::DbSession& session, const std::string& id)
{
    auto libraries = storage::ComponentLibraryRepository(session).find_all();
    if (!libraries)
        return std::unexpected(libraries.error());
    for (const auto& library : *libraries)
        if (library.id == id)
            return std::optional<storage::ComponentLibraryRepository::ComponentLibrary>{library};
    return std::nullopt;
}

}  // namespace

bool is_official_kicad_source(const fs::path& path)
{
    // 逐组件比较：<任意>/share/kicad/... —— 字符串包含会误伤名字里带 share 的目录
    bool share = false;
    for (const auto& component : path)
    {
        if (share && component == "kicad")
            return true;
        share = component == "share";
    }
    return false;
}

util::Result<bool> source_write_blocked(const storage::DbSession& session, const fs::path& path,
                                        const std::string& owner)
{
    using Kind = util::Error::Kind;
    auto normalized = fs::path(storage::source_key(path));
    if (normalized.empty())
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Cannot normalize source path: " + path.string()));
    if (is_official_kicad_source(normalized))
        return true;

    auto libraries = storage::ComponentLibraryRepository(session).find_all();
    if (!libraries)
        return std::unexpected(libraries.error());
    for (const auto& lib : *libraries)
    {
        if (!(lib.locked || lib.is_protected))
            continue;
        if ((!owner.empty() && lib.id == owner) ||
            storage::path_belongs_to(normalized, lib.symbol_path) ||
            storage::path_belongs_to(normalized, lib.footprint_path) ||
            storage::path_belongs_to(normalized, lib.model_3d_path))
            return true;
    }
    return false;
}

util::Result<void> authorize_source_write(const storage::DbSession& session, const fs::path& path,
                                          const std::string& owner)
{
    using Kind = util::Error::Kind;
    auto blocked = source_write_blocked(session, path, owner);
    if (!blocked)
        return std::unexpected(blocked.error());
    if (*blocked)
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Source is locked, protected or lives in an official KiCad directory: " +
            path.string()));

    auto key = storage::source_key(path);
    if (key.empty())
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Cannot normalize source path: " + path.string()));

    const SourceKind kind = source_kind_of(path);
    if (kind == SourceKind::Unknown)
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Unsupported source file kind: " + path.string()));

    // ---- 已登记来源：精确 source_key 命中之后还要核对真实归属 ----
    // 「命中就放行」会绕过真实 owner 的锁/保护与 root 归属：
    // 锁定的组件库、空 root、归属失效、文件已被移出 root 都要在这里拦住。
    bool registered_library = false;
    std::string registered_owner;
    {
        storage::LibraryRepository libraries(session);
        auto row = libraries.find_by_source_key(key);
        if (row)
        {
            registered_library = true;
            registered_owner = row->component_library_id;
        }
        else if (row.error().kind() != Kind::NotFound)
        {
            return std::unexpected(row.error());  // 查询失败不能当允许
        }
    }
    bool registered_footprint = false;
    if (!registered_library)
    {
        storage::FootprintRepository footprints(session);
        auto row = footprints.find_by_source_key(key);
        if (row)
        {
            registered_footprint = true;
            // 封装的归属不在行上：由「哪个组件库的对应 root 真的包含它」判定
        }
        else if (row.error().kind() != Kind::NotFound)
        {
            return std::unexpected(row.error());
        }
    }

    if (registered_library)
    {
        // 调用方显式给的 owner 必须先自证：组件库行真的存在。旧登记行的归属为空时
        // 更不能无条件忽略它 —— 那样任何 ghost / 别人的 UUID 都能冒充归属；
        // 此时唯一能证明归属的是「这个 owner 对应种类的 root 真的包含该文件」。
        if (!owner.empty())
        {
            auto claimed = find_component_library(session, owner);
            if (!claimed)
                return std::unexpected(claimed.error());
            if (!claimed->has_value())
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Caller owner no longer exists: " + path.string()));
            if (!registered_owner.empty() && owner != registered_owner)
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Source is registered to another library: " + path.string()));
            if (registered_owner.empty())
            {
                const std::string& claimed_root = root_for_kind(**claimed, kind);
                if (claimed_root.empty() || !storage::path_belongs_to(path, claimed_root))
                    return std::unexpected(util::Error::make<Kind::ValidationError>(
                        "Source is not claimed by the owning library: " + path.string()));
            }
        }
        // 旧记录（登记归属为空）：只有「已登记的这个精确文件」保持兼容；
        // 同目录里未登记的新文件仍走下面的未登记分支，必须落在可写 root 内
        if (registered_owner.empty())
            return {};
        // 真实 owner 必须存在、没被锁定/保护
        auto group = find_component_library(session, registered_owner);
        if (!group)
            return std::unexpected(group.error());
        if (!group->has_value())
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Registered owner no longer exists: " + path.string()));
        const auto& library = **group;
        if (library.locked || library.is_protected)
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Source owner is locked or protected: " + path.string()));
        const std::string& root = root_for_kind(library, kind);
        // 老记录还没配 root：已登记的这个精确文件保持兼容 —— 它上面这条登记
        // 就是归属证据；新文件没有这条登记，仍必须在未登记分支里找到可写 root
        if (root.empty())
            return {};
        if (!storage::path_belongs_to(path, root))
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Registered source is outside its library " + std::string(source_kind_name(kind)) +
                " root: " + path.string()));
        return {};
    }

    if (registered_footprint)
    {
        // 归属由 root 判定：调用方给的 owner 必须就是包含它的那个组件库，
        // 登记的封装不能让伪 owner 假冒归属；省略 owner 只在唯一归属时兼容
        auto groups = storage::ComponentLibraryRepository(session).find_all();
        if (!groups)
            return std::unexpected(groups.error());
        std::vector<std::string> claimed;
        for (const auto& group : *groups)
        {
            if (!owner.empty() && group.id != owner)
                continue;
            const std::string& root = root_for_kind(group, kind);
            if (root.empty() || !storage::path_belongs_to(path, root))
                continue;
            if (group.locked || group.is_protected)
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Source root is locked or protected: " + path.string()));
            claimed.push_back(group.id);
        }
        if (claimed.empty())
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                owner.empty() ? "Registered source is outside every writable library root: " +
                                    path.string()
                              : "Source is not claimed by the owning library: " + path.string()));
        if (owner.empty() && claimed.size() > 1)
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Source is claimed by several libraries and no owner was given: " + path.string()));
        return {};
    }

    // ---- 未登记：必须落在明确登记的可写 root 之内，且 root 的种类与文件种类对应 ----
    auto libraries = storage::ComponentLibraryRepository(session).find_all();
    if (!libraries)
        return std::unexpected(libraries.error());
    std::vector<std::string> owners;
    bool any_root = false;
    for (const auto& lib : *libraries)
    {
        const std::string* roots[3] = {&lib.symbol_path, &lib.footprint_path, &lib.model_3d_path};
        for (const auto* root : roots)
            if (has_root(*root))
                any_root = true;
        // 只有对应种类的 root 才算归属：符号 root 不是封装 root
        const std::string& root = root_for_kind(lib, kind);
        if (!root.empty() && storage::path_belongs_to(path, root))
        {
            if (std::find(owners.begin(), owners.end(), lib.id) == owners.end())
                owners.push_back(lib.id);
        }
    }
    if (!any_root)
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "No writable library root is configured; refusing to write " + path.string()));
    if (owners.empty())
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Source is not a registered writable source: " + path.string()));
    if (!owner.empty())
    {
        if (std::find(owners.begin(), owners.end(), owner) == owners.end())
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Source is not claimed by the owning library: " + path.string()));
        return {};
    }
    if (owners.size() > 1)
        return std::unexpected(util::Error::make<Kind::ValidationError>(
            "Source is claimed by several libraries and no owner was given: " + path.string()));
    return {};
}

std::string content_digest(const std::string& bytes)
{
    XXH64_hash_t hash = XXH64(bytes.data(), bytes.size(), 0);
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(hash));
    return buffer;
}

util::Result<void> SourceMutationService::apply(
    const std::vector<FileChange>& changes,
    const std::function<util::Result<void>(storage::DbSession&)>& business)
{
    using Kind = util::Error::Kind;
    if (changes.empty())
        return {};

    // ---- 1. 取锁（排序，避免互相等待）----
    std::vector<std::string> keys;
    keys.reserve(changes.size());
    for (const auto& change : changes)
    {
        auto key = storage::source_key(change.path);
        if (key.empty())
            return std::unexpected(util::Error::make<Kind::ValidationError>(
                "Cannot normalize source path: " + change.path.string()));
        keys.push_back(key);
    }
    auto locks = acquire_locks(keys);
    auto unlock = [&]
    {
        release_locks(locks);
    };

    const std::string operation = make_operation_id();
    std::vector<PlannedFile> planned;
    planned.reserve(changes.size());

    // ---- 2. 锁内复核：已登记来源、存在性、摘要 ----
    {
        auto session = db_->session();
        for (std::size_t i = 0; i < changes.size(); ++i)
        {
            const auto& change = changes[i];
            PlannedFile file;
            file.path = change.path;
            file.key = keys[i];
            file.remove = change.remove;
            file.temp = change.path;
            file.temp += "." + operation + ".tmp";
            file.backup = change.path;
            file.backup += "." + operation + ".bak";

            std::error_code ec;
            // 「目标出现了」按任意类型判定：目录、设备、不可读目标同样算出现 ——
            // 只看 is_regular_file 会把它们当成「不存在的新文件」，一路走到替换阶段。
            // 用 symlink_status 而不是 exists：exists 跟随链接，悬空符号链接
            // （本身是一个已经占位的目录项）会被报成不存在。
            const fs::file_status entry = fs::symlink_status(change.path, ec);
            if (entry.type() == fs::file_type::not_found)
                ec.clear();  // 确实没有目录项：不是错误
            if (ec)
            {
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(
                    "Cannot inspect source: " + change.path.string() + ": " + ec.message()));
            }
            const bool exists_any = entry.type() != fs::file_type::not_found;
            // 断言「必须不存在」的位置出现了东西（新建的文件、目录、不可读目标都算）：
            // 这是取锁之前别人的产物，空摘要（不校验）下会被当成可覆盖的新文件，
            // 只能整体拒绝 —— 等走到替换阶段才发现就已经太晚了。
            // 判定必须先于下面的 is_regular_file：后者跟随符号链接，悬空链接在那里
            // 只会拿到 ENOENT，把「已占位的目录项」误报成「读不到的来源」（IoError）。
            if (exists_any && change.expected_missing)
            {
                unlock();
                return std::unexpected(util::Error::make<Kind::ValidationError>(
                    "Source appeared since it was read: " + change.path.string()));
            }
            file.existed = exists_any && fs::is_regular_file(change.path, ec) && !ec;
            if (ec)
            {
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(
                    "Cannot inspect source: " + change.path.string() + ": " + ec.message()));
            }
            if (file.existed)
            {
                std::string original;
                if (!read_file(change.path, original))
                {
                    unlock();
                    return std::unexpected(util::Error::make<Kind::IoError>(
                        "Cannot read source: " + change.path.string()));
                }
                file.old_digest = content_digest(original);
                if (!change.expected_digest.empty() &&
                    change.expected_digest != file.old_digest)
                {
                    unlock();
                    return std::unexpected(util::Error::make<Kind::ValidationError>(
                        "Source changed since it was read: " + change.path.string()));
                }
            }
            else if (!change.allow_missing && !change.expected_missing)
            {
                unlock();
                return std::unexpected(
                    util::Error::make<Kind::NotFound>("Source missing: " + change.path.string()));
            }

            // 授权：官方/锁定/保护根、来源登记、root 归属（核心侧唯一实现）。
            // allow_missing 只管文件在不在，绝不进入授权判定 —— 未登记的路径
            // 不能因为「允许新建」就绕过 root 与 owner 检查。expected_missing
            // 同理：它只是更强的一条存在性断言，不豁免授权。
            if (auto allowed = authorize_source_write(session, change.path, change.owner); !allowed)
            {
                unlock();
                return std::unexpected(allowed.error());
            }

            planned.push_back(std::move(file));
        }
    }

    // ---- 3. 写临时文件与备份 ----
    auto cleanup_temp_backups = [&]
    {
        for (auto& file : planned)
        {
            remove_quietly(file.temp);
            remove_quietly(file.backup);
        }
    };

    std::string error;
    for (std::size_t i = 0; i < planned.size(); ++i)
    {
        auto& file = planned[i];
        if (!file.remove)
        {
            if (!file.existed)
            {
                // 新文件的目录（如某个 .pretty）由本次操作按需建立
                std::error_code error;
                fs::create_directories(file.path.parent_path(), error);
            }
            if (!structurally_sane(file.path, changes[i].content))
            {
                error = "candidate content is malformed: " + file.path.string();
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::ValidationError>(error));
            }
            if (!write_temp(file.temp, changes[i].content, error))
            {
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(error));
            }
            file.new_digest = content_digest(changes[i].content);
        }
        if (file.existed)
        {
            std::error_code ec;
            fs::copy_file(file.path, file.backup, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                cleanup_temp_backups();
                unlock();
                return std::unexpected(util::Error::make<Kind::IoError>(
                    "Cannot back up source: " + file.path.string() + ": " + ec.message()));
            }
        }
    }

    // ---- 4. 短事务落 prepared 日志 ----
    {
        auto session = db_->session();
        if (auto begun = session.begin(); !begun)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(begun.error());
        }
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT INTO file_mutations(id,state,plan_json) VALUES(?1,'prepared',?2)";
        if (sqlite3_prepare_v2(session.handle(), sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(util::Error::make<Kind::DbError>(sqlite3_errmsg(session.handle())));
        }
        std::string plan = plan_to_json(operation, planned);
        sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, plan.c_str(), -1, SQLITE_TRANSIENT);
        bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!inserted)
        {
            (void) session.rollback();
            cleanup_temp_backups();
            unlock();
            return std::unexpected(util::Error::make<Kind::DbError>("Cannot write mutation log"));
        }
        if (auto committed = session.commit(); !committed)
        {
            cleanup_temp_backups();
            unlock();
            return std::unexpected(committed.error());
        }
    }

    // ---- 5. 替换目标文件 ----
    std::vector<PlannedFile*> replaced;
    for (auto& file : planned)
    {
        if (file.remove)
        {
            if (file.existed)
            {
                std::error_code ec;
                fs::remove(file.path, ec);
                if (ec)
                {
                    error = "delete failed: " + ec.message();
                    break;
                }
                replaced.push_back(&file);
            }
            continue;
        }
        const bool published = file.existed ? replace_target(file.path, file.temp, error)
                                            : publish_new_target(file.path, file.temp, error);
        if (!published)
            break;
        replaced.push_back(&file);
    }

    // ---- 6. 业务事务 + applied，同一事务提交；失败则回滚 DB 并还原文件 ----
    auto restore_files = [&]
    {
        for (auto* file : replaced)
        {
            if (file->existed)
            {
                std::error_code ec;
                fs::copy_file(file->backup, file->path, fs::copy_options::overwrite_existing, ec);
                if (ec)
                    kforge::util::log_error{}("Restore failed for {}: {}", file->path.string(),
                                              ec.message());
            }
            else
            {
                remove_quietly(file->path);
            }
        }
    };
    // 用调用方已有的 session —— 同一线程不能再开第二个（不可重入）
    auto drop_log = [&](storage::DbSession& session)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(), "DELETE FROM file_mutations WHERE id = ?1", -1,
                               &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    };
    auto drop_log_short = [&]
    {
        auto session = db_->session();
        drop_log(session);
    };

    if (!error.empty())
    {
        restore_files();
        drop_log_short();
        cleanup_temp_backups();
        unlock();
        return std::unexpected(util::Error::make<Kind::IoError>(error));
    }

    {
        auto session = db_->session();
        if (auto begun = session.begin(); !begun)
        {
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(begun.error());
        }
        if (auto done = business(session); !done)
        {
            (void) session.rollback();  // 先撤业务写入，再在自动提交下清日志
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(done.error());
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(session.handle(),
                               "UPDATE file_mutations SET state='applied' WHERE id=?1", -1, &stmt,
                               nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        if (auto committed = session.commit(); !committed)
        {
            (void) session.rollback();
            restore_files();
            drop_log(session);
            cleanup_temp_backups();
            unlock();
            return std::unexpected(committed.error());
        }
    }

    cleanup_temp_backups();
    // 数据变了：让按版本失效的缓存（绑定索引等）跟着刷新
    db_->bump_data_version();
    unlock();
    return {};
}

RecoveryReport SourceMutationService::recover()
{
    RecoveryReport report;
    auto session = db_->session();

    struct Row
    {
        std::string id;
        std::string state;
        std::string plan;
    };
    std::vector<Row> rows;
    sqlite3_stmt* stmt = nullptr;
    int read_rc = sqlite3_prepare_v2(
        session.handle(),
        "SELECT id, state, plan_json FROM file_mutations ORDER BY created_at", -1, &stmt, nullptr);
    if (read_rc == SQLITE_OK)
    {
        while ((read_rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* plan = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            rows.push_back({id ? id : "", state ? state : "", plan ? plan : ""});
        }
    }
    sqlite3_finalize(stmt);
    if (read_rc != SQLITE_DONE)
    {
        // 半截的变更清单不是完整依据：任何目标、备份或日志都不能按它处理
        report.conflicts++;
        report.messages.push_back(std::string("mutation log: cannot read pending changes: ") +
                                  sqlite3_errmsg(session.handle()));
        kforge::util::log_error{}("Source recovery: {}", report.messages.back());
        return report;
    }

    // 摘要必须是 content_digest() 的输出（16 位十六进制）；空串只允许出现在
    // 「本来就没有对应内容」的位置（新建项没有 old、删除项没有 new）
    auto digest_well_formed = [](const std::string& digest)
    {
        if (digest.empty())
            return true;
        if (digest.size() != 16)
            return false;
        for (char c : digest)
        {
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return false;
        }
        return true;
    };

    // 计划里的字段必须按声明的类型取：写坏的日志（path 是数字、remove 是字符串，
    // 或干脆少写了字段）只能报冲突，不能靠 nlohmann 抛 type_error 逃出恢复流程。
    // 这里也不替日志补默认值：apply() 落盘的每一条都写全这七个字段（plan_to_json），
    // 缺失/null 只说明日志本身不可信 —— 尤其是 existed，把它当成默认的 false
    // 会被读成「本次新建的半成品」，进而删掉已经落地的目标、清掉 backup/temp 与日志。
    auto read_string_field = [](const nlohmann::json& entry, const char* key, std::string& out)
    {
        auto it = entry.find(key);
        if (it == entry.end() || !it->is_string())
            return false;
        out = it->get<std::string>();
        return true;
    };
    auto read_bool_field = [](const nlohmann::json& entry, const char* key, bool& out)
    {
        auto it = entry.find(key);
        if (it == entry.end() || !it->is_boolean())
            return false;
        out = it->get<bool>();
        return true;
    };

    // 读目标：区分「不存在」与「存在但读不出来」——后者同样不能动文件
    enum class Probe
    {
        Missing,
        Ok,
        Unreadable
    };
    auto probe_file = [](const fs::path& path, std::string& out)
    {
        std::error_code ec;
        bool exists = fs::exists(path, ec);
        if (ec)
            return Probe::Unreadable;
        if (!exists)
            return Probe::Missing;
        if (!read_file(path, out))
            return Probe::Unreadable;
        return Probe::Ok;
    };

    struct Item
    {
        fs::path path;
        fs::path temp;
        fs::path backup;
        bool existed = false;
        bool remove = false;
        std::string old_digest;
        std::string new_digest;
    };

    auto conflict = [&](const std::string& id, const std::string& message)
    {
        report.conflicts++;
        report.messages.push_back("mutation " + id + ": " + message);
    };

    for (const auto& row : rows)
    {
        nlohmann::json plan;
        try
        {
            plan = nlohmann::json::parse(row.plan);
        }
        catch (const std::exception& e)
        {
            conflict(row.id, std::string("unreadable plan (") + e.what() + ")");
            continue;
        }

        // 坏计划/未知状态：记录与证据原样保留，只报冲突
        if (row.state != "prepared" && row.state != "applied")
        {
            conflict(row.id, "unknown state '" + row.state + "'");
            continue;
        }
        if (!plan.contains("files") || !plan["files"].is_array())
        {
            conflict(row.id, "plan has no file list");
            continue;
        }

        std::vector<Item> items;
        bool malformed = false;
        try
        {
            for (auto& entry : plan["files"])
            {
                if (!entry.is_object())
                {
                    malformed = true;
                    break;
                }
                Item item;
                std::string path;
                std::string temp;
                std::string backup;
                std::string old_digest;
                std::string new_digest;
                if (!read_string_field(entry, "path", path) ||
                    !read_string_field(entry, "temp", temp) ||
                    !read_string_field(entry, "backup", backup) ||
                    !read_bool_field(entry, "existed", item.existed) ||
                    !read_bool_field(entry, "remove", item.remove) ||
                    !read_string_field(entry, "old_digest", old_digest) ||
                    !read_string_field(entry, "new_digest", new_digest))
                {
                    malformed = true;
                    break;
                }
                item.path = fs::path(path);
                item.temp = fs::path(temp);
                item.backup = fs::path(backup);
                item.old_digest = std::move(old_digest);
                item.new_digest = std::move(new_digest);
                // producer 不变量（plan_to_json 只会写出这四种组合）：
                //   existed ↔ 有旧摘要（false 的那一项没有旧内容），
                //   remove  ↔ 没有新摘要（删除项从不落新内容）。
                // 只检查一半会让「existed=false + 有旧摘要」这种自相矛盾的计划
                // 被当成新建项处理 —— 矛盾就是坏计划，不许挑一半当真去动文件。
                if (item.path.empty() || item.temp.empty() || item.backup.empty() ||
                    !digest_well_formed(item.old_digest) || !digest_well_formed(item.new_digest) ||
                    (item.existed && item.old_digest.empty()) ||
                    (!item.existed && !item.old_digest.empty()) ||
                    (!item.remove && item.new_digest.empty()) ||
                    (item.remove && !item.new_digest.empty()))
                {
                    malformed = true;
                    break;
                }
                items.push_back(std::move(item));
            }
        }
        catch (const std::exception&)
        {
            malformed = true;  // 兜底：任何解析异常都只算「这一行是坏计划」
        }
        if (malformed || items.empty())
        {
            conflict(row.id, "malformed plan (missing, mistyped or self-contradictory file entry)");
            continue;
        }

        // ---- 1. 整组预检：只看不动，任何一项冲突就整组保留 ----
        enum class Action
        {
            Noop,           // 目标已是应有状态，无需改动
            Restore,        // 用已验证的备份还原
            RemoveCreated,  // 删掉本次新建出来的文件
        };
        struct Step
        {
            const Item* item = nullptr;
            Action action = Action::Noop;
        };
        std::vector<Step> steps;
        bool blocked = false;
        for (auto& item : items)
        {
            std::string current;
            Probe state = probe_file(item.path, current);
            std::string current_digest =
                state == Probe::Ok ? content_digest(current) : std::string();
            if (state == Probe::Unreadable)
            {
                conflict(row.id, "cannot read " + item.path.string());
                blocked = true;
                break;
            }

            if (row.state == "prepared")
            {
                if (item.remove)
                {
                    if (state == Probe::Missing)
                    {
                        std::string backup_bytes;
                        Probe backup_state = probe_file(item.backup, backup_bytes);
                        if (backup_state != Probe::Ok ||
                            content_digest(backup_bytes) != item.old_digest)
                        {
                            conflict(row.id, "cannot restore " + item.path.string() +
                                                 ": no verified backup");
                            blocked = true;
                            break;
                        }
                        steps.push_back({&item, Action::Restore});
                    }
                    else if (current_digest == item.old_digest)
                    {
                        steps.push_back({&item, Action::Noop});  // 删除并未发生
                    }
                    else
                    {
                        conflict(row.id, item.path.string() +
                                             " was rebuilt externally; content kept");
                        blocked = true;
                        break;
                    }
                }
                else if (item.existed)
                {
                    if (state == Probe::Ok && current_digest == item.new_digest)
                    {
                        std::string backup_bytes;
                        Probe backup_state = probe_file(item.backup, backup_bytes);
                        if (backup_state != Probe::Ok ||
                            content_digest(backup_bytes) != item.old_digest)
                        {
                            conflict(row.id, "cannot restore " + item.path.string() +
                                                 ": no verified backup");
                            blocked = true;
                            break;
                        }
                        steps.push_back({&item, Action::Restore});
                    }
                    else if (state == Probe::Ok && current_digest == item.old_digest)
                    {
                        steps.push_back({&item, Action::Noop});  // 替换并未发生
                    }
                    else
                    {
                        conflict(row.id, item.path.string() +
                                             " was edited externally; content kept");
                        blocked = true;
                        break;
                    }
                }
                else
                {
                    // 本次新建：只回收摘要等于 new 的文件。外部内容既不是
                    // 本次写的、也不能证明能丢 —— 冲突并整组保留证据
                    if (state == Probe::Missing)
                        steps.push_back({&item, Action::Noop});
                    else if (current_digest == item.new_digest)
                        steps.push_back({&item, Action::RemoveCreated});
                    else
                    {
                        conflict(row.id, item.path.string() +
                                             " already exists with unknown content; kept");
                        blocked = true;
                        break;
                    }
                }
            }
            else  // applied
            {
                if (item.remove)
                {
                    if (state != Probe::Missing)
                    {
                        conflict(row.id, item.path.string() +
                                             " was rebuilt after the recorded delete; kept");
                        blocked = true;
                        break;
                    }
                    steps.push_back({&item, Action::Noop});
                }
                else
                {
                    if (state != Probe::Ok)
                    {
                        conflict(row.id, item.path.string() +
                                             " is missing but was recorded as applied");
                        blocked = true;
                        break;
                    }
                    if (current_digest != item.new_digest)
                    {
                        conflict(row.id, item.path.string() +
                                             " differs from the recorded result; kept");
                        blocked = true;
                        break;
                    }
                    steps.push_back({&item, Action::Noop});
                }
            }
        }
        if (blocked || steps.size() != items.size())
            continue;  // 整组保留 backup/temp/log 与目标

        // ---- 2. 全部预检通过后才动手；任何 IO 失败同样整组保留 ----
        bool io_failed = false;
        for (auto& step : steps)
        {
            if (step.action == Action::Restore)
            {
                std::error_code ec;
                fs::copy_file(step.item->backup, step.item->path,
                              fs::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    conflict(row.id, "restore failed for " + step.item->path.string() + ": " +
                                         ec.message());
                    io_failed = true;
                    break;
                }
                report.restored++;
            }
            else if (step.action == Action::RemoveCreated)
            {
                std::error_code ec;
                fs::remove(step.item->path, ec);
                if (ec)
                {
                    conflict(row.id, "cannot remove half-written " + step.item->path.string() +
                                         ": " + ec.message());
                    io_failed = true;
                    break;
                }
                report.restored++;
            }
        }
        if (io_failed)
            continue;

        for (auto& step : steps)
        {
            if (row.state == "applied")
                report.finalized++;
        }

        // ---- 3. 日志行删除成功才清理备份与临时文件 ----
        sqlite3_stmt* drop = nullptr;
        if (sqlite3_prepare_v2(session.handle(), "DELETE FROM file_mutations WHERE id = ?1", -1,
                               &drop, nullptr) != SQLITE_OK)
        {
            conflict(row.id, std::string("cannot clear mutation log: ") +
                                 sqlite3_errmsg(session.handle()));
            continue;
        }
        sqlite3_bind_text(drop, 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
        bool dropped = sqlite3_step(drop) == SQLITE_DONE;
        sqlite3_finalize(drop);
        if (!dropped)
        {
            conflict(row.id, std::string("cannot clear mutation log: ") +
                                 sqlite3_errmsg(session.handle()));
            continue;
        }

        for (auto& item : items)
        {
            remove_quietly(item.backup);
            remove_quietly(item.temp);
        }
    }

    for (const auto& message : report.messages)
        kforge::util::log_error{}("Source recovery: {}", message);
    if (report.restored > 0 || report.finalized > 0 || report.conflicts > 0)
        kforge::util::log_info{}("Source recovery: {} restored, {} finalized, {} conflicts",
                                 report.restored, report.finalized, report.conflicts);
    return report;
}

}  // namespace kforge::io
