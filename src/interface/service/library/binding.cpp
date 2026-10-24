#include "interface/service/library/binding.h"

#include "core/db/database.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/sexpr/path_util.h"
#include "core/io/sexpr/text_util.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/repo/repositories.h"
#include "correspondence/checker.h"
#include "core/io/source_mutation.h"
#include "util/logger.h"
#include "interface/service/library/source_policy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace kforge::services
{

namespace
{
// Split into lowercase alpha tokens of length >= 3:
// "TQFP-44_10x10mm_P0.8mm" → {"tqfp"}
std::vector<std::string> extract_tokens(const std::string& s)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s)
    {
        if (std::isalpha(static_cast<unsigned char>(c)))
        {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        else
        {
            if (cur.size() >= 3)
            {
                tokens.push_back(cur);
            }
            cur.clear();
        }
    }
    if (cur.size() >= 3)
    {
        tokens.push_back(cur);
    }
    return tokens;
}

std::string to_lower(std::string s)
{
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c)
                           {
                               return std::tolower(c);
                           });
    return s;
}
}  // namespace

util::Result<void> SymbolBindingManager::load_index_locked() const
{
    // 三类对象在同一快照里读完：任一表读失败都原样上报，不改动现有索引 ——
    // 「读失败」绝不能变成「库里一个对象都没有」。
    auto session = db_->session();
    auto fps = storage::FootprintRepository(session).find_all();
    if (!fps) return std::unexpected(fps.error());
    auto models = storage::Model3DRepository(session).find_all();
    if (!models) return std::unexpected(models.error());
    auto syms = storage::SymbolRepository(session).find_all();
    if (!syms) return std::unexpected(syms.error());

    // 全部读成功之后才整份替换：搜索线程不会看到半截索引
    footprints_ = std::move(*fps);
    models_ = std::move(*models);
    symbols_ = std::move(*syms);
    fp_by_name_.clear();
    for (size_t i = 0; i < footprints_.size(); ++i)
    {
        fp_by_name_[footprints_[i].name()] = i;
    }
    model_by_stem_.clear();
    for (size_t i = 0; i < models_.size(); ++i)
    {
        model_by_stem_[path_to_utf8(models_[i].file_path().stem())] = i;
    }
    index_error_.reset();
    kforge::util::log_info{}("BindingManager: {} footprints loaded", footprints_.size());
    kforge::util::log_info{}("BindingManager: {} models loaded", models_.size());
    kforge::util::log_info{}("BindingManager: {} symbols loaded", symbols_.size());
    return {};
}

util::Result<void> SymbolBindingManager::ensure_index() const
{
    {
        std::shared_lock lock(index_mutex_);
        if (!index_error_)
        {
            return {};  // 健康索引照常复用
        }
    }
    std::unique_lock lock(index_mutex_);
    if (!index_error_)
    {
        return {};  // 别的线程刚重建成功
    }
    // 失败只对「这一次」有效：同一个 manager 在故障解除后必须能重新读成功
    return load_index_locked();
}

SymbolBindingManager::SymbolBindingManager(storage::Database& db) : db_(&db)
{
    std::unique_lock lock(index_mutex_);
    if (auto loaded = load_index_locked(); !loaded)
    {
        // 读失败不能当成「库里一个封装都没有」：记下来，搜索时如实上报
        index_error_ = loaded.error();
    }
}

util::Result<SymbolBindingManager::Binding> SymbolBindingManager::get(
    const std::string& symbol_id) const
{
    core::Symbol symbol;
    Snapshot snapshot;
    {
        auto session = db_->session();
        auto found = storage::SymbolRepository(session).find_by_id(symbol_id);
        if (!found)
        {
            // 符号不存在 / 读失败都是错误：空绑定会被前端当成「没绑封装」
            return std::unexpected(found.error());
        }
        symbol = std::move(*found);
        auto loaded = load_snapshot(session);
        if (!loaded)
        {
            return std::unexpected(loaded.error());
        }
        snapshot = std::move(*loaded);
    }
    // 会话已释放：磁盘检查不占库锁
    FileCheckContext files;
    return resolve_symbol(symbol, snapshot, files);
}

util::Result<void> SymbolBindingManager::assign_footprint(const std::string& symbol_id,
                                                          const std::string& footprint_id)
{
    // 读与计算在文件锁之外；文件替换 + DB 变更交给 SourceMutationService 一起做。
    // 它负责：锁内复核来源/锁状态/摘要 → 备份 → prepared 日志 → 替换 → 业务事务。
    std::filesystem::path source;
    std::string original;
    std::string symbol_name;
    std::string reference;
    std::filesystem::path fp_path;
    std::string footprint_digest;
    std::vector<core::Model3DRef> model_refs;
    std::vector<std::string> roots;
    FootprintIdentity expected_identity;
    {
        auto session = db_->session();
        storage::SymbolRepository symbols(session);
        storage::FootprintRepository footprints(session);
        // 读失败原样传播（DbError 必须可检测）；只有真正的「没有这一行」才是 NotFound
        auto sym = symbols.find_by_id(symbol_id);
        if (!sym) return std::unexpected(sym.error());
        auto fp = footprints.find_by_id(footprint_id);
        if (!fp) return std::unexpected(fp.error());
        auto libs = storage::LibraryRepository(session).find_all();
        if (!libs) return std::unexpected(libs.error());
        std::string owner;
        for (const auto& lib : *libs)
            if (lib.id == sym->library_id()) { source = lib.file_path; owner = lib.component_library_id; break; }
        if (source.empty() || !std::filesystem::is_regular_file(source))
            return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                "Symbol source file is missing"));
        auto locked = source_is_locked(session, source, owner);
        if (!locked) return std::unexpected(locked.error());
        if (*locked)
            return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                "Symbol belongs to a locked library"));
        std::ifstream in(source, std::ios::binary);
        if (!in)
            return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                "Cannot read symbol source"));
        original.assign(std::istreambuf_iterator<char>(in), {});
        if (in.bad())
            return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                "Cannot read symbol source"));
        in.close();

        symbol_name = sym->name();
        // 路径文本是 UTF-8 契约：中文路径必须经 char8_t 转换才能查盘/解析
        fp_path = path_from_utf8(fp->library_path());
        reference = fp->name();
        if (reference.find(':') == std::string::npos)
        {
            // 规范引用：footprint_lib_name 优先，旧行为空时回退 .pretty 父目录名
            auto library = fp->footprint_lib_name();
            if (library.empty() && fp_path.parent_path().extension() == ".pretty")
                library = path_to_utf8(fp_path.parent_path().stem());
            if (!library.empty())
                reference = library + ":" + reference;
        }
        // 选中 UUID 的 fresh 复核：行还在不等于文件还在（缺文件/目录替代/来源身份变了
        // 都不能绑），拒绝时源文件、旧关联、模型都保持原样
        {
            FileCheckContext files;
            auto inspected = inspect_footprint_files(*fp, files);
            if (inspected.status != FootprintStatus::Available)
                return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                    "Selected footprint is unavailable: " +
                    std::string(to_string(inspected.status)) + " (" + inspected.reason + ")"));
        }
        // 内容摘要 + 身份快照：加锁后的 fresh 复核要确认「还是同一行、同一份内容」，
        // 否则写下的引用与模型关联都来自已经过期的那一次解析
        std::string footprint_content;
        {
            std::ifstream fp_in(fp_path, std::ios::binary);
            if (!fp_in)
                return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                    "Cannot read the selected footprint"));
            footprint_content.assign(std::istreambuf_iterator<char>(fp_in), {});
            if (fp_in.bad())
                return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
                    "Cannot read the selected footprint"));
        }
        footprint_digest = io::content_digest(footprint_content);
        expected_identity = capture_footprint_identity(*fp);
        // 解析宿主自己读到的文本：parse(path) 内部走窄串打开，中文路径打不开
        auto parsed = parser::FootprintParser::parse_buffer(footprint_content);
        if (!parsed) return std::unexpected(parsed.error());
        model_refs = parsed->models_3d();
        auto groups = storage::ComponentLibraryRepository(session).find_all();
        if (!groups) return std::unexpected(groups.error());
        for (const auto& group : *groups)
            if (!group.model_3d_path.empty()) roots.push_back(group.model_3d_path);
    }

    std::string updated = original;
    if (!sexpr::replace_property_value(updated, "symbol", symbol_name, "Footprint", reference))
    {
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(
            "Footprint property not found in library file"));
    }

    io::SourceMutationService mutations(*db_);
    return mutations.apply(
        {io::FileChange{.path = source,
                        .content = std::move(updated),
                        .expected_digest = io::content_digest(original)}},
        [&](storage::DbSession& session) -> util::Result<void>
        {
            storage::SymbolRepository symbols(session);
            storage::RelationshipRepository links(session);
            storage::Model3DRepository models(session);
            auto sym = symbols.find_by_id(symbol_id);
            if (!sym) return std::unexpected(sym.error());
            // 拿锁之后、写库之前再 fresh 复核一次：从上面读到内容到现在，
            // 复用/选中的封装文件可能已经被删除、替换，或者整行被改名/换源。
            // 身份必须还是当初选中的那一行（同 UUID 不够）：写进源文件的引用必须
            // 对应真实落库的内容，不能拿旧行的解析结果去绑一个已经变了身份的行。
            {
                auto fresh_fp = storage::FootprintRepository(session).find_by_id(footprint_id);
                if (!fresh_fp) return std::unexpected(fresh_fp.error());
                if (!same_footprint_identity(
                        expected_identity, capture_footprint_identity(*fresh_fp)))
                    return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                        "Selected footprint changed before commit: " + footprint_id));
                FileCheckContext files;
                auto inspected = inspect_footprint_files(*fresh_fp, files);
                if (inspected.status != FootprintStatus::Available)
                    return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                        "Footprint is no longer available: " +
                        std::string(to_string(inspected.status)) + " (" + inspected.reason + ")"));
                // 内容也必须还是当初解析的那一份：model_refs 来自上面那次 parse，
                // 文件被换掉之后不能再用旧解析结果写模型关联
                std::string content;
                bool read_ok = false;
                {
                    std::ifstream fresh_in(fp_path, std::ios::binary);
                    if (fresh_in)
                    {
                        content.assign(std::istreambuf_iterator<char>(fresh_in), {});
                        read_ok = !fresh_in.bad();
                    }
                }
                if (!read_ok || io::content_digest(content) != footprint_digest)
                    return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                        "Selected footprint content changed before commit: " + footprint_id));
            }
            sym->set_footprint(reference);
            sym->set_property("Footprint", reference);
            // The footprint reference participates in the identity fingerprint;
            // rebinding is a real version change and must keep the old ID in Pre.
            sym->refresh_Kicad_Forge_ID();
            if (auto updated_row = symbols.update(*sym); !updated_row) return updated_row;
            // 旧关联可能不止一条（历史数据/多次导入）：全部删掉，只留新的那一条
            if (auto unlinked = links.unlink_all_symbol_footprints(symbol_id); !unlinked)
                return unlinked;
            if (auto linked = links.link_symbol_to_footprint(symbol_id, footprint_id, "manual");
                !linked)
                return linked;
            auto old_models = links.find_models_for_footprint(footprint_id);
            if (!old_models) return std::unexpected(old_models.error());
            for (const auto& id : *old_models)
            {
                if (auto unlinked = links.unlink_footprint_model(footprint_id, id); !unlinked)
                    return unlinked;
            }
            for (const auto& model : model_refs)
            {
                auto path = sexpr::resolve_model_path(model.path, fp_path.parent_path(), roots);
                if (path.empty()) continue;
                // 路径文本按 UTF-8 落库：中文目录下的模型才能和后面的读取对上
                auto existing_model = models.find_by_path(path_to_utf8(path));
                std::string id;
                if (existing_model)
                {
                    id = existing_model->id();
                }
                else
                {
                    if (existing_model.error().kind() != util::Error::Kind::NotFound)
                        return std::unexpected(existing_model.error());
                    core::Model3D created;
                    created.set_file_path(path);
                    created.set_format(sexpr::model_format_of(path.extension().string()));
                    created.set_source("footprint_ref");
                    auto inserted = models.insert(created);
                    if (!inserted) return std::unexpected(inserted.error());
                    id = inserted->id();
                }
                if (auto linked = links.link_footprint_to_model(footprint_id, id, "explicit");
                    !linked)
                    return linked;
            }
            return {};
        });
}

util::Result<void> SymbolBindingManager::assign_model(const std::string& footprint_id,
                                                      const std::string& model_id)
{
    auto session = db_->session();

    // Validate BOTH objects before touching any link — an unknown model id
    // must not destroy the footprint's existing model relationships.
    storage::FootprintRepository fr(session);
    auto footprint = fr.find_by_id(footprint_id);
    if (!footprint)
    {
        // 读失败保留 DbError：不能换成「Footprint not found」把故障说成不存在
        return std::unexpected(footprint.error());
    }
    // 失效封装上不允许挂新模型：文件没了/被目录替代/来源身份变了都拒绝，
    // 否则会写下一条指向死封装的模型关联
    {
        FileCheckContext files;
        auto inspected = inspect_footprint_files(*footprint, files);
        if (inspected.status != FootprintStatus::Available)
        {
            return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                "Cannot bind a model to an unavailable footprint: " +
                std::string(to_string(inspected.status)) + " (" + inspected.reason + ")"));
        }
    }
    storage::Model3DRepository mr(session);
    if (auto model = mr.find_by_id(model_id); !model)
    {
        // 同上：NotFound 与 DbError 各自保真
        return std::unexpected(model.error());
    }

    // Swap inside a transaction: any failure rolls back to the previous links.
    storage::RelationshipRepository rr(session);
    if (auto begun = session.begin(); !begun)
    {
        return std::unexpected(begun.error());
    }

    auto rollback = [&](const util::Error& error) -> util::Result<void>
    {
        (void) session.rollback();
        return std::unexpected(error);
    };

    if (auto existing = rr.find_models_for_footprint(footprint_id))
    {
        for (const auto& m_id : *existing)
        {
            if (auto unlink = rr.unlink_footprint_model(footprint_id, m_id); !unlink)
            {
                return rollback(unlink.error());
            }
        }
    }
    else
    {
        // find_models_for_footprint failed (rather than empty) — do not
        // proceed to link on top of unknown state
        return rollback(existing.error());
    }

    if (auto link = rr.link_footprint_to_model(footprint_id, model_id, "manual"); !link)
    {
        return rollback(link.error());
    }
    if (auto committed = session.commit(); !committed)
    {
        return rollback(committed.error());
    }

    kforge::util::log_info{}("Binding: footprint {} → model {}", footprint_id, model_id);
    return {};
}

util::Result<std::vector<SymbolBindingManager::FootprintRef>>
SymbolBindingManager::search_footprints(const std::string& query, int limit,
                                        const std::string& anchor) const
{
    if (auto ready = ensure_index(); !ready)
    {
        return std::unexpected(ready.error());
    }
    std::shared_lock index_lock(index_mutex_);  // 与索引重建互斥，健康索引共享
    std::vector<FootprintRef> result;

    // .pretty library name (parent dir of the .kicad_mod) — callers build "Lib:Name" refs。
    // 库名进的是引用身份，必须按 UTF-8 还原（窄串会把中文库名变成乱码）
    auto library_of = [](const core::Footprint& fp) -> std::string
    {
        auto library = fp.footprint_lib_name();
        if (!library.empty())
        {
            return library;
        }
        return path_to_utf8(path_from_utf8(fp.library_path()).parent_path().stem());
    };

    if (!query.empty())
    {
        // Keyword filter
        auto q = to_lower(query);
        for (const auto& fp : footprints_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            if (to_lower(fp.name()).contains(q))
            {
                result.push_back({.id = fp.id(), .name = fp.name(),
                                  .library = library_of(fp), .score = 1.0,
                                  .pad_count = fp.pad_count()});
            }
        }
        return result;
    }

    if (anchor.empty())
    {
        // No keyword and no anchor — first N rows (legacy default)
        for (const auto& fp : footprints_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            result.push_back({.id = fp.id(), .name = fp.name(),
                              .library = library_of(fp), .score = 1.0,
                              .pad_count = fp.pad_count()});
        }
        return result;
    }

    // Relevance: prefilter by token containment, then rank by name similarity
    auto tokens = extract_tokens(anchor);
    struct Scored
    {
        double score;
        const core::Footprint* fp;
    };
    std::vector<Scored> scored;
    for (const auto& fp : footprints_)
    {
        auto lower = to_lower(fp.name());
        bool hit = tokens.empty();
        for (const auto& t : tokens)
        {
            if (lower.contains(t))
            {
                hit = true;
                break;
            }
        }
        if (!hit)
        {
            continue;
        }
        double s = correspondence::CorrespondenceChecker::name_similarity(anchor, fp.name());
        if (s >= 0.3)
        {
            scored.push_back({s, &fp});
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b)
              {
                  if (a.score != b.score)
                  {
                      return a.score > b.score;
                  }
                  return a.fp->name() < b.fp->name();
              });
    for (auto& s : scored)
    {
        if (result.size() >= static_cast<size_t>(limit))
            break;
        result.push_back({.id = s.fp->id(), .name = s.fp->name(),
                          .library = library_of(*s.fp), .score = s.score,
                          .pad_count = s.fp->pad_count()});
    }
    return result;
}

util::Result<std::vector<SymbolBindingManager::ModelRef>> SymbolBindingManager::search_models(
    const std::string& query, int limit, const std::string& anchor) const
{
    if (auto ready = ensure_index(); !ready)
    {
        return std::unexpected(ready.error());
    }
    std::shared_lock index_lock(index_mutex_);
    std::vector<ModelRef> result;

    if (!query.empty())
    {
        // Keyword filter
        auto q = to_lower(query);
        for (const auto& m : models_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            auto stem = path_to_utf8(m.file_path().stem());
            if (to_lower(stem).contains(q))
            {
                result.push_back({.id = m.id(), .name = stem, .format = m.format(), .score = 1.0});
            }
        }
        return result;
    }

    if (anchor.empty())
    {
        // No keyword and no anchor — first N rows (legacy default)
        for (const auto& m : models_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            result.push_back({.id = m.id(), .name = path_to_utf8(m.file_path().stem()),
                              .format = m.format(), .score = 1.0});
        }
        return result;
    }

    // Relevance: prefilter by token containment, then rank by name similarity
    auto tokens = extract_tokens(anchor);
    struct Scored
    {
        double score;
        const core::Model3D* m;
    };
    std::vector<Scored> scored;
    for (const auto& m : models_)
    {
        auto stem = path_to_utf8(m.file_path().stem());
        auto lower = to_lower(stem);
        bool hit = tokens.empty();
        for (const auto& t : tokens)
        {
            if (lower.contains(t))
            {
                hit = true;
                break;
            }
        }
        if (!hit)
        {
            continue;
        }
        double s = correspondence::CorrespondenceChecker::name_similarity(anchor, stem);
        if (s >= 0.3)
        {
            scored.push_back({s, &m});
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b)
              {
                  if (a.score != b.score)
                  {
                      return a.score > b.score;
                  }
                  return path_to_utf8(a.m->file_path().stem()) <
                         path_to_utf8(b.m->file_path().stem());
              });
    for (auto& s : scored)
    {
        if (result.size() >= static_cast<size_t>(limit))
            break;
        result.push_back({.id = s.m->id(), .name = path_to_utf8(s.m->file_path().stem()),
                          .format = s.m->format(), .score = s.score});
    }
    return result;
}

util::Result<std::vector<SymbolBindingManager::SymbolRef>> SymbolBindingManager::search_symbols(
    const std::string& query, int limit, const std::string& anchor) const
{
    if (auto ready = ensure_index(); !ready)
    {
        return std::unexpected(ready.error());
    }
    std::shared_lock index_lock(index_mutex_);
    std::vector<SymbolRef> result;

    if (!query.empty())
    {
        auto q = to_lower(query);
        for (const auto& sym : symbols_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            if (to_lower(sym.name()).contains(q))
            {
                result.push_back({.id = sym.id(), .name = sym.name(), .score = 1.0});
            }
        }
        return result;
    }

    if (anchor.empty())
    {
        for (const auto& sym : symbols_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            result.push_back({.id = sym.id(), .name = sym.name(), .score = 1.0});
        }
        return result;
    }

    // Relevance: prefilter by token containment, then rank by name similarity
    auto tokens = extract_tokens(anchor);
    struct Scored
    {
        double score;
        const core::Symbol* sym;
    };
    std::vector<Scored> scored;
    for (const auto& sym : symbols_)
    {
        auto lower = to_lower(sym.name());
        bool hit = tokens.empty();
        for (const auto& t : tokens)
        {
            if (lower.contains(t))
            {
                hit = true;
                break;
            }
        }
        if (!hit)
        {
            continue;
        }
        double s = correspondence::CorrespondenceChecker::name_similarity(anchor, sym.name());
        if (s >= 0.3)
        {
            scored.push_back({s, &sym});
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b)
              {
                  if (a.score != b.score)
                  {
                      return a.score > b.score;
                  }
                  return a.sym->name() < b.sym->name();
              });
    for (auto& s : scored)
    {
        if (result.size() >= static_cast<size_t>(limit))
            break;
        result.push_back({.id = s.sym->id(), .name = s.sym->name(), .score = s.score});
    }
    return result;
}

}  // namespace kforge::services
