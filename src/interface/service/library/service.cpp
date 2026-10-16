#include "interface/service/library/service.h"

#include "classifier/rule_engine.h"
#include "correspondence/checker.h"
#include "core/io/parser/footprint_parser.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "interface/service/import/pipeline.h"
#include "core/io/sexpr/text_util.h"
#include "core/db/database.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include "util/logger.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace kforge::services {
using Kind = util::Error::Kind;

LibraryService::LibraryService(storage::Database* db) : db_(db) {}

// -- Import bridges (delegate to ImportPipeline) --

util::Result<int> LibraryService::import_symbol_library(const std::filesystem::path& path,
                                                         const std::string& comp_lib_id) {
    auto r = ImportPipeline(*db_)
        | symbols_from{path.string(), comp_lib_id}
        | execute;
    return r.symbols;
}

util::Result<int> LibraryService::import_footprint(const std::filesystem::path& path) {
    auto r = ImportPipeline(*db_).operator|(execute);
    return 0;  // footprints_from handles directories, not single files
}

util::Result<LibraryService::ImportStats> LibraryService::import_directory(
    const std::filesystem::path& dir, const std::string& comp_lib_id) {
    auto r = ImportPipeline(*db_)
        | symbols_from{dir.string(), comp_lib_id}
        | execute;
    ImportStats stats;
    stats.symbols = r.symbols;
    stats.footprints = r.footprints;
    return stats;
}

util::Result<int> LibraryService::scan_3d_models(const std::filesystem::path& dir) {
    auto r = ImportPipeline(*db_)
        | models_from{dir.string()}
        | execute;
    return r.models_3d;
}

// ============================================================
// delete_library
// ============================================================

util::Result<LibraryService::DeleteLibResult> LibraryService::delete_library(
    const core::Uuid& lib_id) {
    std::string file_path;
    {
        auto session = db_->session();
        auto libs = storage::LibraryRepository(session).find_all();
        if (libs)
            for (auto& l : *libs)
                if (l.id == lib_id) file_path = l.file_path.string();
        if (file_path.empty())
            return std::unexpected(
                util::Error::make<Kind::NotFound>("Library not found: " + lib_id));
    }

    int removed = 0;
    auto drop_rows = [&](storage::DbSession& session) -> util::Result<void>
    {
        storage::LibraryRepository lr(session);
        storage::SymbolRepository sr(session);
        auto syms = sr.find_by_library(lib_id);
        if (!syms) return std::unexpected(syms.error());
        for (auto& s : *syms)
            if (sr.remove(s.id())) removed++;
        if (auto result = lr.remove(lib_id); !result) return result;
        return {};
    };

    if (!std::filesystem::is_regular_file(file_path)) {
        // 没有源文件：只清数据库行
        auto session = db_->session();
        if (auto dropped = drop_rows(session); !dropped)
            return std::unexpected(dropped.error());
        return DeleteLibResult{removed, ""};
    }

    // 文件删除与行删除同一次变更：任一步失败两边一起回退
    io::SourceMutationService mutations(*db_);
    auto applied = mutations.apply({io::FileChange{.path = file_path, .remove = true}}, drop_rows);
    if (!applied) return std::unexpected(applied.error());
    return DeleteLibResult{removed, file_path};
}

// ============================================================
// delete_symbol — uses sexpr::text_util for clean S-expression removal
// ============================================================

util::Result<void> LibraryService::delete_symbol(const core::Uuid& sym_id) {
    std::string sym_name;
    std::filesystem::path source_file;
    {
        auto session = db_->session();
        storage::SymbolRepository sr(session);
        auto sym = sr.find_by_id(sym_id);
        if (!sym) return std::unexpected(util::Error::make<Kind::NotFound>("Symbol not found"));
        sym_name = sym->name();
        auto libs = storage::LibraryRepository(session).find_all();
        if (libs)
            for (auto& l : *libs) {
                if (l.id != sym->library_id() || l.file_path.empty()) continue;
                source_file = l.file_path;
                break;
            }
    }

    auto remove_row = [&](storage::DbSession& session) -> util::Result<void>
    {
        storage::SymbolRepository sr(session);
        if (!sr.remove(sym_id))
            return std::unexpected(util::Error::make<Kind::DbError>("Failed to remove symbol"));
        return {};
    };

    // 没有源文件（或节点不在文件里）：只删数据库行
    std::string original;
    if (source_file.empty() || !std::filesystem::is_regular_file(source_file)) {
        auto session = db_->session();
        return remove_row(session);
    }
    {
        std::ifstream in(source_file, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto node_start = sexpr::find_node(original, "symbol", sym_name);
    if (!node_start)
    {
        auto session = db_->session();
        return remove_row(session);
    }
    auto node_end = sexpr::find_matching_paren(original, *node_start);
    if (node_end == std::string_view::npos)
    {
        return std::unexpected(util::Error::make<Kind::ParseError>(
            "Malformed symbol node in " + source_file.string()));
    }

    // 文件删除 + 行删除一起提交：失败时文件与库行都回到原状
    std::string updated = sexpr::remove_node(original, *node_start, node_end + 1);
    io::SourceMutationService mutations(*db_);
    return mutations.apply(
        {io::FileChange{.path = source_file,
                        .content = std::move(updated),
                        .expected_digest = io::content_digest(original)}},
        remove_row);
}

// ============================================================
// merge_into_library — S-expression helpers + merge logic
// ============================================================

static bool needs_quote(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s)
        if (c == ' ' || c == '"' || c == '(' || c == ')' || c == '\n' || c == '\t' || c == '\r')
            return true;
    return false;
}

static std::string atom_or_string(const std::string& s) {
    if (needs_quote(s)) return "\"" + s + "\"";
    return s;
}

static std::string fmt2(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

static std::string write_property(const std::string& name, const std::string& val,
                                   double x, double y, bool hide) {
    std::string s = "\t\t\t(property \"" + name + "\" " + atom_or_string(val) + "\n"
                    "\t\t\t\t(at " + fmt2(x) + " " + fmt2(y) + " 0)\n"
                    "\t\t\t\t(effects\n"
                    "\t\t\t\t\t(font (size 1.27 1.27))\n";
    if (hide && name != "Reference" && name != "Value")
        s += "\t\t\t\t\t(hide yes)\n";
    s += "\t\t\t\t)\n\t\t\t)\n";
    return s;
}

util::Result<int> LibraryService::merge_into_library(const std::filesystem::path& source_sym,
                                                      const std::filesystem::path& target_sym) {
    auto src = parser::SymbolLibParser::parse(source_sym);
    if (!src || src->items.empty())
    {
        kforge::util::log_info{}("MERGE: no symbols in source");
        return 0;
    }

    std::string target_text;
    if (std::filesystem::exists(target_sym)) {
        std::ifstream tf(target_sym, std::ios::binary);
        if (!tf.is_open())
            return std::unexpected(util::Error::make<Kind::IoError>("Cannot open: " + target_sym.string()));
        std::stringstream buf;
        buf << tf.rdbuf();
        target_text = buf.str();
    }

    auto last_paren = target_text.find_last_of(')');
    if (last_paren == std::string::npos) {
        target_text = "(kicad_symbol_lib\n  (version 20231120)\n  (generator KiCad_Forge)\n)\n";
        last_paren = target_text.find_last_of(')');
    }

    std::string insertion;
    for (auto& sym : src->items) {
        std::string name = sym.name();
        std::string val = sym.default_value().empty() ? name : sym.default_value();
        std::string ref = sym.reference_prefix().empty() ? "U" : sym.reference_prefix();

        insertion += "\n\t(symbol " + atom_or_string(name) + "\n";
        if (sym.pin_count() > 0) insertion += "\t\t(pin_names (offset 1.016))\n";
        insertion += "\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n";
        insertion += write_property("Reference", ref, 0, 7.62, false);
        insertion += write_property("Value", val, 0, 2.54, false);
        if (!sym.footprint().empty())
            insertion += write_property("Footprint", sym.footprint(), 0, -2.54, true);
        if (!sym.datasheet().empty())
            insertion += write_property("Datasheet", sym.datasheet(), 0, -7.62, true);
        if (!sym.description().empty())
            insertion += write_property("Description", sym.description(), 0, -12.7, true);
        if (!sym.mpn().empty())
            insertion += write_property("MPN", sym.mpn(), 0, -17.78, true);
        if (!sym.Kicad_Forge_ID().empty())
            insertion += write_property("Kicad_Forge_ID", sym.Kicad_Forge_ID(), 0, -22.86, true);
        if (!sym.Pre_Kicad_Forge_ID().empty())
            insertion += write_property("Pre_Kicad_Forge_ID", sym.Pre_Kicad_Forge_ID(), 0, -25.4, true);

        insertion += "\t\t(symbol " + atom_or_string(name + "_0_1") + "\n"
                     "\t\t\t(pin_numbers hide)\n\t\t\t(pin_names hide)\n"
                     "\t\t\t(rectangle (start -5.08 5.08) (end 5.08 -5.08)\n"
                     "\t\t\t\t(stroke (width 0.254) (type default))\n"
                     "\t\t\t\t(fill (type background))\n\t\t\t)\n";

        int n = sym.pin_count();
        if (n == 0) n = 3;
        for (int i = 0; i < n; i++) {
            std::string pin_num, pin_name, etype;
            if (i < (int)sym.pins().size()) {
                auto& p = sym.pins()[i];
                pin_num = p.number;
                pin_name = p.name;
                etype = p.electrical_type;
            }
            if (pin_num.empty() || pin_num == "unspecified") pin_num = std::to_string(i + 1);
            if (pin_name.empty()) pin_name = pin_num;
            if (etype.empty()) etype = "passive";
            double py = 3.81 - i * 2.54;
            bool left = (i < n / 2);
            double px = left ? -7.62 : 7.62;
            double ang = left ? 0.0 : 180.0;
            if (etype.find(' ') == std::string::npos) etype += " line";
            insertion += "\t\t\t(pin " + etype + "\n"
                         "\t\t\t\t(at " + fmt2(px) + " " + fmt2(py) + " " + fmt2(ang) + ")\n"
                         "\t\t\t\t(length 2.54)\n"
                         "\t\t\t\t(name " + atom_or_string(pin_name) + "\n"
                         "\t\t\t\t\t(effects (font (size 1.27 1.27)))\n\t\t\t\t)\n"
                         "\t\t\t\t(number " + atom_or_string(pin_num) + "\n"
                         "\t\t\t\t\t(effects (font (size 1.27 1.27)))\n\t\t\t\t)\n\t\t\t)\n";
        }
        insertion += "\t\t)\n\t)\n";
    }

    target_text.insert(last_paren, insertion);

    // 目标文件的改写同样归统一变更服务：备份、日志、失败回退都有据可查。
    // 目标是新文件时允许不存在；已存在则必须是已登记来源（或调用方先登记）。
    std::error_code exists_error;
    bool existed = std::filesystem::is_regular_file(target_sym, exists_error) && !exists_error;
    io::FileChange change{.path = target_sym,
                          .content = std::move(target_text),
                          .allow_missing = !existed};
    io::SourceMutationService mutations(*db_);
    auto written = mutations.apply({std::move(change)},
                                   [](storage::DbSession&) -> util::Result<void> { return {}; });
    if (!written)
        return std::unexpected(written.error());
    kforge::util::log_info{}("MERGED: {} symbols into {}", src->items.size(), target_sym.string());
    return (int)src->items.size();
}

// ============================================================
// patch_kf_id_to_file — 走统一变更服务落盘（读/改在锁外完成）
// ============================================================

int LibraryService::patch_kf_id_to_file(
    const std::filesystem::path& sym_file,
    const std::unordered_map<std::string, std::string>& name_to_kf_id) {
    if (!std::filesystem::is_regular_file(sym_file)) return 0;
    std::ifstream in(sym_file, std::ios::binary);
    if (!in.is_open()) return 0;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string original = text;
    int patched = sexpr::patch_kf_id_content(text, name_to_kf_id);
    if (patched == 0) return 0;

    io::SourceMutationService mutations(*db_);
    auto written = mutations.apply({io::FileChange{.path = sym_file,
                                                   .content = std::move(text),
                                                   .expected_digest = io::content_digest(original)}},
                                   [](storage::DbSession&) -> util::Result<void> { return {}; });
    if (!written)
    {
        kforge::util::log_error{}("KF ID write failed for {}: {}", sym_file.string(),
                                  written.error().format_message());
        return 0;
    }
    return patched;
}

// ============================================================
// Stats queries
// ============================================================

int LibraryService::symbol_count() const {
    auto session = db_->session();
    storage::SymbolRepository repo(session);
    return repo.count();
}

int LibraryService::footprint_count() const {
    auto session = db_->session();
    storage::FootprintRepository repo(session);
    return repo.count();
}

int LibraryService::model_3d_count() const {
    auto session = db_->session();
    storage::Model3DRepository repo(session);
    return repo.count();
}

}  // namespace kforge::services
