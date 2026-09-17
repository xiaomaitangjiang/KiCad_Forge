#include "interface/service/library/binding.h"

#include "core/io/parser/footprint_parser.h"
#include "core/io/sexpr/path_util.h"
#include "core/io/sexpr/text_util.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/repo/repositories.h"
#include "correspondence/checker.h"
#include "util/logger.h"

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

SymbolBindingManager::SymbolBindingManager(sqlite3* db) : db_(db)
{
    // Load footprints
    storage::FootprintRepository fp_repo(db_);
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
    storage::Model3DRepository m_repo(db_);
    if (auto models = m_repo.find_all())
    {
        models_ = std::move(*models);
        for (size_t i = 0; i < models_.size(); ++i)
        {
            model_by_stem_[models_[i].file_path().stem().string()] = i;
        }
    }
    kforge::util::log_info{}("BindingManager: {} models loaded", models_.size());
}

SymbolBindingManager::Binding SymbolBindingManager::get(const std::string& symbol_id) const
{
    Binding b{};
    storage::RelationshipRepository rr(db_);

    auto fp_opt = rr.find_footprint_for_symbol(symbol_id);
    if (fp_opt && *fp_opt)
    {
        const auto& fp_id = **fp_opt;
        b.fp_id = fp_id;

        // Look up footprint name from in-memory cache
        for (const auto& fp : footprints_)
        {
            if (fp.id() == fp_id)
            {
                b.fp_name = fp.name();
                break;
            }
        }

        // Look up linked 3D model
        auto model_ids = rr.find_models_for_footprint(fp_id);
        if (model_ids && !model_ids->empty())
        {
            const auto& m_id = (*model_ids)[0];
            b.model_id = m_id;
            for (const auto& m : models_)
            {
                if (m.id() == m_id)
                {
                    b.model_name = m.file_path().filename().string();
                    break;
                }
            }
        }
    }
    return b;
}

util::Result<void> SymbolBindingManager::assign_footprint(const std::string& symbol_id,
                                                          const std::string& footprint_id)
{
    storage::FootprintRepository fpr(db_);
    std::string fp_name;
    if (auto fp = fpr.find_by_id(footprint_id))
    {
        fp_name = fp->name();
    }

    // 1. The .kicad_sym file is the source of truth: update its
    //    (property "Footprint" ...) FIRST; on failure return without
    //    touching the DB so file and DB cannot diverge.
    {
        storage::SymbolRepository sr(db_);
        auto sym = sr.find_by_id(symbol_id);
        if (sym && !fp_name.empty())
        {
            storage::LibraryRepository lr(db_);
            if (auto libs = lr.find_all())
            {
                for (auto& l : *libs)
                {
                    if (l.id != sym->library_id() || l.file_path.empty())
                    {
                        continue;
                    }
                    if (!std::filesystem::exists(l.file_path))
                    {
                        break;  // library file gone — nothing to sync
                    }
                    std::ifstream f(l.file_path, std::ios::binary);
                    std::string text((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
                    f.close();
                    if (!sexpr::replace_property_value(text, "symbol", sym->name(), "Footprint",
                                                       fp_name))
                    {
                        return std::unexpected(util::Error::make<util::Error::Kind::ValidationError>(
                            "Footprint property not found in library file"));
                    }
                    std::ofstream out(l.file_path, std::ios::binary);
                    out << text;
                    if (!out)
                    {
                        return std::unexpected(util::Error::make<util::Error::Kind::IoError>(
                            "Failed to write library file"));
                    }
                    break;
                }
            }
        }
    }

    storage::RelationshipRepository rr(db_);

    // 2. DB link
    auto existing = rr.find_footprint_for_symbol(symbol_id);
    if (existing && *existing)
    {
        if (auto unlink = rr.unlink_symbol_footprint(symbol_id, **existing); !unlink)
        {
            kforge::util::log_error{}("Binding: unlink failed: {}",
                                      util::error_formatter(unlink.error()));
        }
    }
    if (auto link = rr.link_symbol_to_footprint(symbol_id, footprint_id, "manual"); !link)
    {
        kforge::util::log_error{}("Binding: link failed: {}",
                                  util::error_formatter(link.error()));
    }

    // 3. Auto-link the new footprint's own 3D models (same resolution as the
    //    import pipeline: absolute / relative-to-footprint / ${VAR} roots).
    if (auto fp = fpr.find_by_id(footprint_id); fp && !fp->library_path().empty())
    {
        auto parsed = parser::FootprintParser::parse(fp->library_path());
        if (parsed)
        {
            // Clear old model links — the new footprint's refs take over
            if (auto olds = rr.find_models_for_footprint(footprint_id))
            {
                for (auto& mid : *olds)
                {
                    auto _ = rr.unlink_footprint_model(footprint_id, mid);
                }
            }
            // Model roots for ${KICADx_3DMODEL_DIR} resolution
            std::vector<std::string> roots;
            {
                storage::ComponentLibraryRepository clr(db_);
                if (auto cls = clr.find_all())
                {
                    for (auto& cl : *cls)
                    {
                        if (!cl.model_3d_path.empty())
                        {
                            roots.emplace_back(cl.model_3d_path);
                        }
                    }
                }
            }
            storage::Model3DRepository mrepo(db_);
            for (auto& m : parsed->models_3d())
            {
                if (m.path.empty())
                {
                    continue;
                }
                auto resolved =
                    sexpr::resolve_model_path(m.path, std::filesystem::path(fp->library_path()).parent_path(),
                                              roots);
                if (resolved.empty())
                {
                    continue;  // unresolvable reference — stays unlinked
                }
                std::string model_id;
                if (auto existing = mrepo.find_by_path(resolved.string()))
                {
                    model_id = existing->id();  // Uuid == std::string
                }
                else
                {
                    core::Model3D nm;
                    nm.set_file_path(resolved);
                    nm.set_format(sexpr::model_format_of(resolved.extension().string()));
                    nm.set_description(resolved.stem().string());
                    nm.set_source("footprint_ref");
                    if (auto nins = mrepo.insert(nm); nins)
                    {
                        model_id = nins->id();  // Uuid == std::string
                    }
                }
                if (!model_id.empty())
                {
                    auto _ = rr.link_footprint_to_model(footprint_id, model_id, "explicit");
                }
            }
        }
    }

    kforge::util::log_info{}("Binding: symbol {} → footprint {}", symbol_id, footprint_id);
    return {};
}

void SymbolBindingManager::assign_model(const std::string& footprint_id,
                                        const std::string& model_id)
{
    storage::RelationshipRepository rr(db_);

    // Unlink existing models for this footprint
    auto existing = rr.find_models_for_footprint(footprint_id);
    if (existing)
    {
        for (const auto& m_id : *existing)
        {
            if (auto unlink = rr.unlink_footprint_model(footprint_id, m_id); !unlink)
            {
                kforge::util::log_error{}("Binding: unlink model failed: {}",
                                          util::error_formatter(unlink.error()));
            }
        }
    }
    if (auto link = rr.link_footprint_to_model(footprint_id, model_id, "manual"); !link)
    {
        kforge::util::log_error{}("Binding: link model failed: {}",
                                  util::error_formatter(link.error()));
    }
    kforge::util::log_info{}("Binding: footprint {} → model {}", footprint_id, model_id);
}

std::vector<SymbolBindingManager::FootprintRef> SymbolBindingManager::search_footprints(
    const std::string& query, int limit, const std::string& anchor) const
{
    std::vector<FootprintRef> result;

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
                result.push_back({.id = m.id(), .name = stem, .format = m.format()});
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
                              .format = m.format()});
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
                          .format = s.m->format()});
    }
    return result;
}

}  // namespace kforge::services
