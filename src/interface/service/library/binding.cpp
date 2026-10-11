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

SymbolBindingManager::SymbolBindingManager(storage::Database& db) : db_(&db)
{
    auto session = db_->session();  // 三类对象读同一快照
    // Load footprints
    storage::FootprintRepository fp_repo(session);
    if (auto fps = fp_repo.find_all())
    {
        footprints_ = std::move(*fps);
        for (size_t i = 0; i < footprints_.size(); ++i)
        {
            fp_by_name_[footprints_[i].name()] = i;
        }
    }
    kforge::util::log_info{}("BindingManager: {} footprints loaded", footprints_.size());

    // Load 3D models
    storage::Model3DRepository m_repo(session);
    if (auto models = m_repo.find_all())
    {
        models_ = std::move(*models);
        for (size_t i = 0; i < models_.size(); ++i)
        {
            model_by_stem_[models_[i].file_path().stem().string()] = i;
        }
    }
    kforge::util::log_info{}("BindingManager: {} models loaded", models_.size());

    // Load symbols (name search / similarity matching)
    storage::SymbolRepository s_repo(session);
    if (auto syms = s_repo.find_all())
    {
        symbols_ = std::move(*syms);
    }
    kforge::util::log_info{}("BindingManager: {} symbols loaded", symbols_.size());
}

SymbolBindingManager::Binding SymbolBindingManager::get(const std::string& symbol_id) const
{
    Binding b{};
    auto session = db_->session();
    storage::RelationshipRepository rr(session);

    auto fp_opt = rr.find_footprint_for_symbol(symbol_id);
    if (fp_opt && *fp_opt)
    {
        const auto& fp_id = **fp_opt;
        b.fp_id = fp_id;

        if (auto fp = storage::FootprintRepository(session).find_by_id(fp_id))
            b.fp_name = fp->name();

        // Look up linked 3D model
        auto model_ids = rr.find_models_for_footprint(fp_id);
        if (model_ids && !model_ids->empty())
        {
            const auto& m_id = (*model_ids)[0];
            b.model_id = m_id;
            if (auto model = storage::Model3DRepository(session).find_by_id(m_id))
                b.model_name = model->file_path().filename().string();
        }
    }
    return b;
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
    std::vector<core::Model3DRef> model_refs;
    std::vector<std::string> roots;
    auto require = [](const auto& result) {
        if (!result) throw std::runtime_error(result.error().format_message());
    };
    try
    {
        auto session = db_->session();
        storage::SymbolRepository symbols(session);
        storage::FootprintRepository footprints(session);
        auto sym = symbols.find_by_id(symbol_id);
        auto fp = footprints.find_by_id(footprint_id);
        require(sym);
        require(fp);
        auto libs = storage::LibraryRepository(session).find_all();
        require(libs);
        std::string owner;
        for (const auto& lib : *libs)
            if (lib.id == sym->library_id()) { source = lib.file_path; owner = lib.component_library_id; break; }
        if (source.empty() || !std::filesystem::is_regular_file(source))
            throw std::runtime_error("Symbol source file is missing");
        auto locked = source_is_locked(session, source, owner);
        require(locked);
        if (*locked) throw std::runtime_error("Symbol belongs to a locked library");
        std::ifstream in(source, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read symbol source");
        original.assign(std::istreambuf_iterator<char>(in), {});
        if (in.bad()) throw std::runtime_error("Cannot read symbol source");
        in.close();

        symbol_name = sym->name();
        reference = fp->name();
        fp_path = std::filesystem::path(fp->library_path());
        if (reference.find(':') == std::string::npos && fp_path.parent_path().extension() == ".pretty")
            reference = fp_path.parent_path().stem().string() + ":" + reference;
        auto parsed = parser::FootprintParser::parse(fp_path);
        require(parsed);
        model_refs = parsed->models_3d();
        auto groups = storage::ComponentLibraryRepository(session).find_all();
        require(groups);
        for (const auto& group : *groups)
            if (!group.model_3d_path.empty()) roots.push_back(group.model_3d_path);
    }
    catch (const std::exception& error)
    {
        return std::unexpected(
            util::Error::make<util::Error::Kind::ServiceError>(error.what()));
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
            sym->set_footprint(reference);
            sym->set_property("Footprint", reference);
            if (auto updated_row = symbols.update(*sym); !updated_row) return updated_row;
            auto existing = links.find_footprint_for_symbol(symbol_id);
            if (!existing) return std::unexpected(existing.error());
            if (*existing)
            {
                if (auto unlinked = links.unlink_symbol_footprint(symbol_id, **existing); !unlinked)
                    return unlinked;
            }
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
                auto existing_model = models.find_by_path(path.string());
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
    if (auto fp = fr.find_by_id(footprint_id); !fp)
    {
        return std::unexpected(
            util::Error::make<util::Error::Kind::NotFound>("Footprint not found: " + footprint_id));
    }
    storage::Model3DRepository mr(session);
    if (auto model = mr.find_by_id(model_id); !model)
    {
        return std::unexpected(util::Error::make<util::Error::Kind::NotFound>("Model not found: " + model_id));
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

std::vector<SymbolBindingManager::FootprintRef> SymbolBindingManager::search_footprints(
    const std::string& query, int limit, const std::string& anchor) const
{
    std::vector<FootprintRef> result;

    // .pretty library name (parent dir of the .kicad_mod) — callers build "Lib:Name" refs
    auto library_of = [](const core::Footprint& fp) -> std::string
    {
        return std::filesystem::path(fp.library_path()).parent_path().stem().string();
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

std::vector<SymbolBindingManager::ModelRef> SymbolBindingManager::search_models(
    const std::string& query, int limit, const std::string& anchor) const
{
    std::vector<ModelRef> result;

    if (!query.empty())
    {
        // Keyword filter
        auto q = to_lower(query);
        for (const auto& m : models_)
        {
            if (result.size() >= static_cast<size_t>(limit))
                break;
            auto stem = m.file_path().stem().string();
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
            result.push_back({.id = m.id(), .name = m.file_path().stem().string(),
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
        auto stem = m.file_path().stem().string();
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
                  return a.m->file_path().stem().string() < b.m->file_path().stem().string();
              });
    for (auto& s : scored)
    {
        if (result.size() >= static_cast<size_t>(limit))
            break;
        result.push_back({.id = s.m->id(), .name = s.m->file_path().stem().string(),
                          .format = s.m->format(), .score = s.score});
    }
    return result;
}

std::vector<SymbolBindingManager::SymbolRef> SymbolBindingManager::search_symbols(
    const std::string& query, int limit, const std::string& anchor) const
{
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
