#include "interface/service/library/binding.h"

#include "core/io/parser/footprint_parser.h"
#include "core/io/sexpr/path_util.h"
#include "core/io/sexpr/text_util.h"
#include "core/model/footprint.h"
#include "core/model/model_3d.h"
#include "core/repo/repositories.h"
#include "correspondence/checker.h"
#include "util/logger.h"
#include "util/file_write.h"
#include "interface/service/library/source_policy.h"
#include <sqlite3.h>
#include <mutex>

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

        if (auto fp = storage::FootprintRepository(db_).find_by_id(fp_id))
            b.fp_name = fp->name();

        // Look up linked 3D model
        auto model_ids = rr.find_models_for_footprint(fp_id);
        if (model_ids && !model_ids->empty())
        {
            const auto& m_id = (*model_ids)[0];
            b.model_id = m_id;
            if (auto model = storage::Model3DRepository(db_).find_by_id(m_id))
                b.model_name = model->file_path().filename().string();
        }
    }
    return b;
}

util::Result<void> SymbolBindingManager::assign_footprint(const std::string& symbol_id,
                                                          const std::string& footprint_id)
{
    static std::mutex binding_mutex;
    std::lock_guard lock(binding_mutex);
    bool transaction = false, file_changed = false;
    std::filesystem::path source;
    std::string original;
    auto exec = [&](const char* sql) {
        if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
    };
    auto require = [](const auto& result) {
        if (!result) throw std::runtime_error(result.error().format_message());
    };
    try
    {
        storage::SymbolRepository symbols(db_);
        storage::FootprintRepository footprints(db_);
        storage::RelationshipRepository links(db_);
        storage::Model3DRepository models(db_);
        auto sym = symbols.find_by_id(symbol_id);
        auto fp = footprints.find_by_id(footprint_id);
        require(sym);
        require(fp);
        auto libs = storage::LibraryRepository(db_).find_all();
        require(libs);
        std::string owner;
        for (const auto& lib : *libs)
            if (lib.id == sym->library_id()) { source = lib.file_path; owner = lib.component_library_id; break; }
        if (source.empty() || !std::filesystem::is_regular_file(source))
            throw std::runtime_error("Symbol source file is missing");
        auto locked = source_is_locked(db_, source, owner);
        require(locked);
        if (*locked) throw std::runtime_error("Symbol belongs to a locked library");
        std::ifstream in(source, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read symbol source");
        original.assign(std::istreambuf_iterator<char>(in), {});
        if (in.bad()) throw std::runtime_error("Cannot read symbol source");
        in.close();
        std::string updated = original;
        auto reference = fp->name();
        auto fp_path = std::filesystem::path(fp->library_path());
        if (reference.find(':') == std::string::npos && fp_path.parent_path().extension() == ".pretty")
            reference = fp_path.parent_path().stem().string() + ":" + reference;
        if (!sexpr::replace_property_value(updated, "symbol", sym->name(), "Footprint", reference))
            throw std::runtime_error("Footprint property not found in library file");
        auto parsed = parser::FootprintParser::parse(fp_path);
        require(parsed);
        auto groups = storage::ComponentLibraryRepository(db_).find_all();
        require(groups);
        std::vector<std::string> roots;
        for (const auto& group : *groups)
            if (!group.model_3d_path.empty()) roots.push_back(group.model_3d_path);

        exec("SAVEPOINT binding_update");
        transaction = true;
        sym->set_footprint(reference);
        sym->set_property("Footprint", reference);
        require(symbols.update(*sym));
        auto existing = links.find_footprint_for_symbol(symbol_id);
        require(existing);
        if (*existing) require(links.unlink_symbol_footprint(symbol_id, **existing));
        require(links.link_symbol_to_footprint(symbol_id, footprint_id, "manual"));
        auto old_models = links.find_models_for_footprint(footprint_id);
        require(old_models);
        for (const auto& id : *old_models) require(links.unlink_footprint_model(footprint_id, id));
        for (const auto& model : parsed->models_3d())
        {
            auto path = sexpr::resolve_model_path(model.path, fp_path.parent_path(), roots);
            if (path.empty()) continue;
            auto existing_model = models.find_by_path(path.string());
            std::string id;
            if (existing_model) id = existing_model->id();
            else
            {
                if (existing_model.error().kind() != util::Error::Kind::NotFound)
                    require(existing_model);
                core::Model3D created;
                created.set_file_path(path);
                created.set_format(sexpr::model_format_of(path.extension().string()));
                created.set_source("footprint_ref");
                auto inserted = models.insert(created);
                require(inserted);
                id = inserted->id();
            }
            require(links.link_footprint_to_model(footprint_id, id, "explicit"));
        }
        util::replace_file(source, updated);
        file_changed = true;
        exec("RELEASE binding_update");
        transaction = false;
        return {};
    }
    catch (const std::exception& error)
    {
        std::string message = error.what();
        if (transaction)
        {
            if (sqlite3_exec(db_, "ROLLBACK TO binding_update", nullptr, nullptr, nullptr) != SQLITE_OK)
                message += "; database rollback failed: " + std::string(sqlite3_errmsg(db_));
            sqlite3_exec(db_, "RELEASE binding_update", nullptr, nullptr, nullptr);
        }
        if (file_changed)
        {
            try { util::replace_file(source, original); }
            catch (const std::exception& restore_error) { message += "; source restore failed: " + std::string(restore_error.what()); }
        }
        return std::unexpected(util::Error::make<util::Error::Kind::ServiceError>(message));
    }
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
