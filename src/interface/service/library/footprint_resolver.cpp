#include "interface/service/library/footprint_resolver.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>

#include "core/db/db_session.h"
#include "core/db/source_key.h"

namespace kforge::services
{

namespace
{

std::string to_utf8(const std::u8string& text)
{
    return std::string(text.begin(), text.end());
}

/// 「这个路径上没有文件」的系统错误：ENOENT（不存在）与 ENOTDIR
/// （父路径被普通文件替代）。权限、IO 失败不属于这一类，仍是 check_failed。
bool is_missing_error(const std::error_code& error)
{
    return error == std::errc::no_such_file_or_directory ||
           error == std::errc::not_a_directory;
}

/// "Pkg:Name" → "Name"；无前缀时整体就是名字
std::string bare_name_of(const std::string& reference)
{
    auto colon = reference.find(':');
    if (colon == std::string::npos)
    {
        return reference;
    }
    return reference.substr(colon + 1);
}

/// 索引键：KiCad 库名与封装名是大小写敏感的身份（与 ImportPipeline 的精确
/// 字符串匹配一致）。Windows 物理路径的大小写折叠只属于 source_key，不适用于引用身份。
std::string lib_key(const std::string& library, const std::string& name)
{
    std::string key = library;
    key.push_back('\x1f');
    key += name;
    return key;
}

/// .pretty 库名：优先 footprint_lib_name 列，旧行为空时回退父目录名
std::string library_name_of(const core::Footprint& fp)
{
    if (!fp.footprint_lib_name().empty())
    {
        return fp.footprint_lib_name();
    }
    if (fp.library_path().empty())
    {
        return {};
    }
    return to_utf8(path_from_utf8(fp.library_path()).parent_path().stem().u8string());
}

/// 定位到的 DB 行 → 规范引用 "Lib:Name"
std::string canonical_reference(const core::Footprint& fp)
{
    auto library = library_name_of(fp);
    if (library.empty())
    {
        return fp.name();
    }
    return library + ":" + fp.name();
}

/// 引用与 DB 行的匹配规则：名字与库名都必须精确一致（库名带前缀时）
bool reference_matches(const core::Footprint& fp, const Snapshot& snapshot,
                       const std::string& library, const std::string& name)
{
    if (fp.name() != name)
    {
        return false;
    }
    if (library.empty())
    {
        return true;
    }
    return snapshot.library_of(fp) == library;
}

/// 文件级检查：空路径/缺文件/目录替代/来源身份不一致各有原因码。
/// 全部结论来自 FileCheckContext::check 的**一次**结果：这里不再自己 status 一遍。
void inspect_file(const core::Footprint& fp, FileCheckContext& files, Resolution& out)
{
    if (fp.library_path().empty())
    {
        out.status = FootprintStatus::Stale;
        out.reason = fp_reason::kEmptyPath;
        return;
    }
    auto check = files.check(path_from_utf8(fp.library_path()));
    if (check.failed)
    {
        out.status = FootprintStatus::CheckFailed;
        out.reason = fp_reason::kStatFailed;
        return;
    }
    if (!check.exists)
    {
        // 父路径被普通文件替代（POSIX 的 ENOTDIR）同样是「这个路径上没有文件」
        out.status = FootprintStatus::Stale;
        out.reason = fp_reason::kMissingFile;
        return;
    }
    if (!check.regular_file)
    {
        // 目录替代（或其它特殊文件）：路径在，但不是封装文件
        out.status = FootprintStatus::Stale;
        out.reason = fp_reason::kNotRegularFile;
        return;
    }
    // 有来源身份的旧行才比对：身份对不上说明这个路径上的文件换成了别的来源
    if (!fp.source_key().empty() && check.canonical != fp.source_key())
    {
        out.status = FootprintStatus::Stale;
        out.reason = fp_reason::kSourceKeyMismatch;
        return;
    }
    out.status = FootprintStatus::Available;
    out.reason = fp_reason::kOk;
}

/// 模型：只看该封装在 DB 里的关联（不递归扫盘、不猜路径）
void inspect_models(const core::Footprint& fp, const Snapshot& snapshot, FileCheckContext& files,
                    Resolution& out)
{
    auto links = snapshot.model_links.find(fp.id());
    if (links == snapshot.model_links.end() || links->second.empty())
    {
        out.model_status = ModelStatus::Unassigned;
        out.model_reason = fp_reason::kNoModelLink;
        return;
    }
    struct Candidate
    {
        std::string id;
        std::string name;
        std::string sort_key;
    };
    std::vector<Candidate> usable;
    bool check_failed = false;
    for (const auto& model_id : links->second)
    {
        const auto* model = snapshot.model(model_id);
        if (model == nullptr)
        {
            continue;  // 悬空关联：DB 行已不在，不能算「可用模型」
        }
        if (model->file_path().empty())
        {
            continue;
        }
        auto check = files.check(model->file_path());
        if (check.failed)
        {
            check_failed = true;
            continue;
        }
        if (!check.regular_file)
        {
            continue;
        }
        auto name = to_utf8(model->file_path().filename().u8string());
        usable.push_back(Candidate{.id = model->id(),
                                   .name = name,
                                   .sort_key = model->file_path().generic_string() + "\x1f" +
                                               model->id()});
    }
    if (!usable.empty())
    {
        // 多个可用模型时按路径稳定排序取第一个：展示字段确定，不随行序抖动
        std::sort(usable.begin(), usable.end(),
                  [](const Candidate& a, const Candidate& b)
                  {
                      return a.sort_key < b.sort_key;
                  });
        out.model_status = ModelStatus::Available;
        out.model_reason = fp_reason::kOk;
        out.model_id = usable.front().id;
        out.model_name = usable.front().name;
        return;
    }
    if (check_failed)
    {
        out.model_status = ModelStatus::CheckFailed;
        out.model_reason = fp_reason::kModelCheckFailed;
        return;
    }
    out.model_status = ModelStatus::Stale;
    out.model_reason = fp_reason::kModelFilesMissing;
}

void resolve_models_for(const core::Footprint& fp, const Snapshot& snapshot, FileCheckContext& files,
                        Resolution& out)
{
    if (out.status != FootprintStatus::Available)
    {
        // 封装本身不可用，3D 再「有文件」也不能算正常
        auto links = snapshot.model_links.find(fp.id());
        bool has_links = links != snapshot.model_links.end() && !links->second.empty();
        out.model_status = has_links ? ModelStatus::Stale : ModelStatus::Unassigned;
        out.model_reason = has_links ? fp_reason::kFootprintUnavailable : fp_reason::kNoModelLink;
        return;
    }
    inspect_models(fp, snapshot, files, out);
}

}  // namespace

const char* to_string(FootprintStatus status)
{
    switch (status)
    {
    case FootprintStatus::Unassigned:
        return "unassigned";
    case FootprintStatus::NotFound:
        return "not_found";
    case FootprintStatus::Ambiguous:
        return "ambiguous";
    case FootprintStatus::Stale:
        return "stale";
    case FootprintStatus::Available:
        return "available";
    case FootprintStatus::CheckFailed:
        return "check_failed";
    }
    return "check_failed";
}

const char* to_string(ModelStatus status)
{
    switch (status)
    {
    case ModelStatus::Unassigned:
        return "unassigned";
    case ModelStatus::Stale:
        return "stale";
    case ModelStatus::Available:
        return "available";
    case ModelStatus::CheckFailed:
        return "check_failed";
    }
    return "check_failed";
}

// ---------------------------------------------------------------- UTF-8 路径

std::filesystem::path path_from_utf8(const std::string& utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const auto* bytes = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(std::u8string_view(bytes, utf8.size()));
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    return to_utf8(path.u8string());
}

// ---------------------------------------------------------------- 身份

FootprintIdentity capture_footprint_identity(const core::Footprint& footprint)
{
    return FootprintIdentity{
        .id = footprint.id(),
        .name = footprint.name(),
        .footprint_lib_name = footprint.footprint_lib_name(),
        .source_key = footprint.source_key(),
        // 路径按 source_key 规范化语义比较（Windows 折叠大小写）；
        // 纯规范化，不判断存在性 —— 行被改成不存在的路径也算漂移
        .library_path = storage::source_key(path_from_utf8(footprint.library_path())),
    };
}

bool same_footprint_identity(const FootprintIdentity& left, const FootprintIdentity& right)
{
    // 名称/库名是精确身份；路径与来源键按来源身份比较
    return left.id == right.id && left.name == right.name &&
           left.footprint_lib_name == right.footprint_lib_name &&
           left.source_key == right.source_key && left.library_path == right.library_path;
}

// ---------------------------------------------------------------- FileCheckContext

std::size_t FileCheckContext::stat_calls() const
{
    std::lock_guard lock(mutex_);
    return stat_calls_;
}

FileCheckResult FileCheckContext::check(const std::filesystem::path& path, bool fresh)
{
    FileCheckResult result;
    if (path.empty())
    {
        return result;
    }
    const auto input_key = to_utf8(path.u8string());
    if (!fresh)
    {
        // 输入路径命中：连 weakly_canonical 都不必再算
        std::lock_guard lock(mutex_);
        if (auto mapped = canonical_by_input_.find(input_key); mapped != canonical_by_input_.end())
        {
            if (auto cached = cache_.find(mapped->second); cached != cache_.end())
            {
                return cached->second;
            }
        }
    }
    // 去重键用规范化路径：同一个文件用不同写法引用时也只 stat 一次
    auto key = storage::source_key(path);
    if (key.empty())
    {
        key = input_key;
    }
    if (!fresh)
    {
        std::lock_guard lock(mutex_);
        if (auto cached = cache_.find(key); cached != cache_.end())
        {
            canonical_by_input_[input_key] = key;
            return cached->second;
        }
    }
    std::error_code error;
    auto status = std::filesystem::status(path, error);  // 非抛异常重载
    if (error && !is_missing_error(error))
    {
        // 权限/IO 失败是 check_failed，绝不能当成「文件不存在」。
        // ENOENT/ENOTDIR 是「这个路径上没有文件」，不是「查不动」。
        result.failed = true;
    }
    else
    {
        result.exists = !error && status.type() != std::filesystem::file_type::not_found;
        result.regular_file = result.exists && std::filesystem::is_regular_file(status);
        result.directory = result.exists && std::filesystem::is_directory(status);
    }
    result.canonical = key;
    {
        std::lock_guard lock(mutex_);
        canonical_by_input_[input_key] = key;
        // fresh 的结果同样回写：同一次响应里后面的非 fresh 查询看到的是最新状态
        cache_[key] = result;
        ++stat_calls_;
    }
    return result;
}

// ---------------------------------------------------------------- Snapshot

const core::Footprint* Snapshot::footprint(const std::string& id) const
{
    auto found = footprint_index.find(id);
    if (found == footprint_index.end())
    {
        return nullptr;
    }
    return &footprints[found->second];
}

const core::Model3D* Snapshot::model(const std::string& id) const
{
    auto found = model_index.find(id);
    if (found == model_index.end())
    {
        return nullptr;
    }
    return &models[found->second];
}

std::string Snapshot::library_of(const core::Footprint& fp) const
{
    return library_name_of(fp);
}

namespace
{
util::Result<void> load_links(const storage::DbSession& session, Snapshot& snapshot)
{
    storage::RelationshipRepository relationships(session);
    auto link_rows = relationships.find_all_symbol_link_rows();
    if (!link_rows)
    {
        return std::unexpected(link_rows.error());
    }
    snapshot.symbol_links = std::move(*link_rows);
    auto model_links = relationships.find_all_model_links();
    if (!model_links)
    {
        return std::unexpected(model_links.error());
    }
    snapshot.model_links = std::move(*model_links);
    return {};
}

void build_indices(Snapshot& snapshot)
{
    snapshot.footprint_index.reserve(snapshot.footprints.size());
    for (std::size_t i = 0; i < snapshot.footprints.size(); ++i)
    {
        const auto& fp = snapshot.footprints[i];
        snapshot.footprint_index[fp.id()] = i;
        snapshot.by_name[fp.name()].push_back(i);
        auto library = snapshot.library_of(fp);
        if (!library.empty())
        {
            snapshot.by_lib_name[lib_key(library, fp.name())].push_back(i);
        }
    }
    snapshot.model_index.reserve(snapshot.models.size());
    for (std::size_t i = 0; i < snapshot.models.size(); ++i)
    {
        snapshot.model_index[snapshot.models[i].id()] = i;
    }
    snapshot.links_by_symbol.reserve(snapshot.symbol_links.size());
    for (std::size_t i = 0; i < snapshot.symbol_links.size(); ++i)
    {
        snapshot.links_by_symbol[snapshot.symbol_links[i].symbol_id].push_back(i);
    }
}
}  // namespace

util::Result<Snapshot> load_snapshot(const storage::DbSession& session)
{
    Snapshot snapshot;
    auto footprints = storage::FootprintRepository(session).find_all();
    if (!footprints)
    {
        return std::unexpected(footprints.error());
    }
    snapshot.footprints = std::move(*footprints);
    auto models = storage::Model3DRepository(session).find_all();
    if (!models)
    {
        return std::unexpected(models.error());
    }
    snapshot.models = std::move(*models);

    if (auto loaded = load_links(session, snapshot); !loaded)
    {
        return std::unexpected(loaded.error());
    }
    build_indices(snapshot);
    return snapshot;
}

util::Result<Snapshot> load_snapshot_light(const storage::DbSession& session)
{
    Snapshot snapshot;
    auto footprints = storage::FootprintRepository(session).find_all_for_index();
    if (!footprints)
    {
        return std::unexpected(footprints.error());
    }
    snapshot.footprints = std::move(*footprints);
    auto models = storage::Model3DRepository(session).find_all_for_index();
    if (!models)
    {
        return std::unexpected(models.error());
    }
    snapshot.models = std::move(*models);

    if (auto loaded = load_links(session, snapshot); !loaded)
    {
        return std::unexpected(loaded.error());
    }
    build_indices(snapshot);
    return snapshot;
}

// ---------------------------------------------------------------- resolve

Resolution resolve_symbol(const core::Symbol& symbol, const Snapshot& snapshot,
                          FileCheckContext& files)
{
    Resolution out;
    out.reference = symbol.footprint();
    if (out.reference.empty())
    {
        // 用户根本没写引用：不拿 DB 里的陈旧关联反向「恢复」它
        out.status = FootprintStatus::Unassigned;
        out.reason = fp_reason::kNoReference;
        out.model_status = ModelStatus::Unassigned;
        out.model_reason = fp_reason::kNoModelLink;
        return out;
    }

    auto library = storage::footprint_lib_name_of(out.reference);
    auto name = bare_name_of(out.reference);
    if (name.empty())
    {
        out.status = FootprintStatus::NotFound;
        out.reason = fp_reason::kNoMatch;
        return out;
    }

    // 1) 先聚合该符号的全部关联（不取首条），只认与当前引用匹配的行
    std::set<std::string> matched;
    if (auto found = snapshot.links_by_symbol.find(symbol.id());
        found != snapshot.links_by_symbol.end())
    {
        for (auto row_index : found->second)
        {
            const auto& row = snapshot.symbol_links[row_index];
            const auto* fp = snapshot.footprint(row.footprint_id);
            if (fp == nullptr)
            {
                continue;  // 悬空关联：不参与定位，也不在这里删
            }
            if (reference_matches(*fp, snapshot, library, name))
            {
                matched.insert(fp->id());
            }
        }
    }
    // 一个匹配 UUID 就足以消除「同名跨来源」的歧义；两个不同的匹配 UUID 只能报歧义，
    // 不能按 link_type 或行序挑一个
    if (matched.size() > 1)
    {
        out.status = FootprintStatus::Ambiguous;
        out.reason = fp_reason::kMultipleMatches;
        return out;
    }

    const core::Footprint* resolved = nullptr;
    if (matched.size() == 1)
    {
        resolved = snapshot.footprint(*matched.begin());
    }
    else
    {
        // 2) 没有可用关联时才按引用在库里定位：带前缀只认指定库，无前缀只认裸名唯一
        const std::vector<std::size_t>* candidates = nullptr;
        if (!library.empty())
        {
            if (auto found = snapshot.by_lib_name.find(lib_key(library, name));
                found != snapshot.by_lib_name.end())
            {
                candidates = &found->second;
            }
        }
        else if (auto found = snapshot.by_name.find(name); found != snapshot.by_name.end())
        {
            candidates = &found->second;
        }
        if (candidates == nullptr || candidates->empty())
        {
            out.status = FootprintStatus::NotFound;
            out.reason = fp_reason::kNoMatch;
            return out;
        }
        if (candidates->size() > 1)
        {
            out.status = FootprintStatus::Ambiguous;
            out.reason = fp_reason::kMultipleMatches;
            return out;
        }
        resolved = &snapshot.footprints[candidates->front()];
    }

    out.footprint_id = resolved->id();
    out.footprint_name = resolved->name();
    out.resolved_reference = canonical_reference(*resolved);
    inspect_file(*resolved, files, out);
    resolve_models_for(*resolved, snapshot, files, out);
    return out;
}

Resolution inspect_footprint(const core::Footprint& footprint, const Snapshot& snapshot,
                             FileCheckContext& files, bool with_model)
{
    Resolution out;
    out.reference = footprint.name();
    out.footprint_id = footprint.id();
    out.footprint_name = footprint.name();
    out.resolved_reference = canonical_reference(footprint);
    inspect_file(footprint, files, out);
    if (with_model)
    {
        resolve_models_for(footprint, snapshot, files, out);
    }
    else if (out.status != FootprintStatus::Available)
    {
        out.model_status = ModelStatus::Stale;
        out.model_reason = fp_reason::kFootprintUnavailable;
    }
    return out;
}

Resolution inspect_footprint_files(const core::Footprint& footprint, FileCheckContext& files)
{
    Resolution out;
    out.reference = footprint.name();
    out.footprint_id = footprint.id();
    out.footprint_name = footprint.name();
    out.resolved_reference = canonical_reference(footprint);
    inspect_file(footprint, files, out);
    if (out.status != FootprintStatus::Available)
    {
        out.model_status = ModelStatus::Stale;
        out.model_reason = fp_reason::kFootprintUnavailable;
    }
    return out;
}

}  // namespace kforge::services
