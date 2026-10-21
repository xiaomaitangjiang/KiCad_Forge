// 宿主编辑服务 S1（对象复制：只读导出）、S2（对象粘贴）与 S3（剪切/移动）回归测试。
// 夹具全部是临时目录 + 合成 .kicad_sym，不读用户库、不碰真实数据。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include "core/db/database.h"
#include "core/io/parser/symbol_lib_parser.h"
#include "core/io/source_mutation.h"
#include "core/repo/repositories.h"
#include "interface/service/editing/export.h"
#include "interface/service/editing/history.h"
#include "interface/service/editing/move.h"
#include "interface/service/editing/paste.h"
#include "interface/service/library/service.h"
#include "support/test_environment.h"

namespace
{
namespace fs = std::filesystem;
using namespace kforge;

std::string read_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), {}};
}

void write_bytes(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    REQUIRE(out.good());
}

// ---------------------------------------------------------------
// 合成库内容：简单符号（多单元 + 图形 + 未知节点 + 隐藏属性）
// ---------------------------------------------------------------
constexpr std::string_view kSimpleSymbol = R"SYM((symbol "R_Complex"
  (pin_names (offset 0.254))
  (exclude_from_sim no)
  (in_bom yes)
  (on_board yes)
  (property "Reference" "R" (at 2.032 0 90) (effects (font (size 1.27 1.27))))
  (property "Value" "10k" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Footprint" "" (at -1.778 0 90) (effects (font (size 1.27 1.27)) hide))
  (property "Datasheet" "~" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
  (property "ki_keywords" "resistor" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
  (symbol "R_Complex_0_1"
    (rectangle (start -2.54 1.016) (end 2.54 -1.016)
      (stroke (width 0.254) (type default)) (fill (type none)))
    (polyline (pts (xy -2.54 0) (xy 2.54 0))
      (stroke (width 0) (type default)) (fill (type none)))
    (kforge_unknown_node (foo "bar")))
  (symbol "R_Complex_1_1"
    (pin passive line (at -5.08 0 0) (length 2.54)
      (name "~" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin passive line (at 5.08 0 180) (length 2.54)
      (name "~" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27))))))))SYM";

// 派生链：Base ← Derived ← GrandChild，用来验证 extends 闭包与顺序。
constexpr std::string_view kBaseSymbol = R"SYM((symbol "OpAmp_Base"
  (pin_names (offset 0.127))
  (exclude_from_sim no)
  (in_bom yes)
  (on_board yes)
  (property "Reference" "U" (at 0 7.62 0) (effects (font (size 1.27 1.27))))
  (symbol "OpAmp_Base_0_1"
    (rectangle (start -5.08 5.08) (end 5.08 -5.08)
      (stroke (width 0.254) (type default)) (fill (type background))))
  (symbol "OpAmp_Base_1_1"
    (pin input line (at -7.62 2.54 0) (length 2.54)
      (name "+" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))
    (pin input line (at -7.62 -2.54 0) (length 2.54)
      (name "-" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27))))))))SYM";

constexpr std::string_view kDerivedSymbol = R"SYM((symbol "OpAmp_Derived"
  (extends "OpAmp_Base")
  (property "Value" "LM358" (at 0 0 0) (effects (font (size 1.27 1.27))))))SYM";

constexpr std::string_view kGrandChildSymbol = R"SYM((symbol "OpAmp_GrandChild"
  (extends "OpAmp_Derived")
  (property "Value" "LM324" (at 0 0 0) (effects (font (size 1.27 1.27))))))SYM";

std::string library_text()
{
    return std::string("(kicad_symbol_lib (version 20231120) (generator kforge-tests)\n") +
           std::string(kSimpleSymbol) + "\n" + std::string(kBaseSymbol) + "\n" +
           std::string(kDerivedSymbol) + "\n" + std::string(kGrandChildSymbol) + "\n)\n";
}

const core::Symbol* find_saved(const std::vector<core::Symbol>& rows, std::string_view name)
{
    for (const auto& row : rows)
    {
        if (row.name() == name)
            return &row;
    }
    FAIL("fixture symbol missing: " << std::string(name));
    return nullptr;
}

struct EditingExportFixture
{
    test_support::TempDirectory temp;
    test_support::RestoreDefaultLogger restore;
    std::unique_ptr<storage::Database> db;
    fs::path file = temp.path() / "libs" / "editing-test.kicad_sym";
    std::string library_id;
    std::vector<core::Symbol> saved_symbols;

    EditingExportFixture()
    {
        spdlog::set_default_logger(std::make_shared<spdlog::logger>(
            "editing-test", std::make_shared<spdlog::sinks::null_sink_mt>()));
        auto opened = storage::Database::open(temp.path() / "test.db");
        REQUIRE(opened.has_value());
        db = std::move(*opened);
        write_bytes(file, library_text());
        import_library();
    }

    void import_library()
    {
        auto parsed = parser::SymbolLibParser::parse(file);
        REQUIRE_MESSAGE(parsed.has_value(), parsed.error().format_message());
        REQUIRE(parsed->items.size() == 4);
        core::LibraryMeta meta;
        meta.name = "editing-test";
        meta.file_path = file;
        auto session = db->session();
        auto lib = storage::LibraryRepository(session).insert(meta);
        REQUIRE(lib.has_value());
        library_id = lib->id;
        storage::SymbolRepository repo(session);
        for (auto& sym : parsed->items)
        {
            sym.set_library_id(library_id);
            auto row = repo.insert(sym);
            REQUIRE(row.has_value());
            saved_symbols.push_back(*row);
        }
    }
};

// ---------------------------------------------------------------
// S2 粘贴夹具：另一个目标库 + 可写/只读归属，源内容取自上面的导出结果。
// ---------------------------------------------------------------
std::string path_text(const fs::path& path)
{
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

struct EditingPasteFixture : EditingExportFixture
{
    fs::path target_file = temp.path() / "libs" / "paste-target.kicad_sym";
    std::string target_owner_id;
    std::string target_library_id;

    EditingPasteFixture()
    {
        write_bytes(target_file,
                    "(kicad_symbol_lib (version 20231120) (generator kforge-tests)\n)\n");
        target_owner_id = create_component_library("paste-owner", temp.path() / "libs");
        target_library_id = register_target_library("paste-target", target_file, target_owner_id);
    }

    std::string target_digest() const
    {
        return io::content_digest(read_bytes(target_file));
    }

    std::string register_target_library(const std::string& name, const fs::path& path,
                                        const std::string& owner = {})
    {
        auto session = db->session();
        core::LibraryMeta meta;
        meta.name = name;
        meta.file_path = path;
        meta.component_library_id = owner;
        auto lib = storage::LibraryRepository(session).insert(meta);
        REQUIRE(lib.has_value());
        return lib->id;
    }

    std::string create_component_library(const std::string& name, const fs::path& root,
                                         bool locked = false)
    {
        auto session = db->session();
        storage::ComponentLibraryRepository::ComponentLibrary owner;
        owner.name = name;
        owner.symbol_path = path_text(root);
        owner.locked = locked;
        auto row = storage::ComponentLibraryRepository(session).insert(owner);
        REQUIRE(row.has_value());
        return row->id;
    }

    std::vector<core::Symbol> rows_for(const std::string& library_id)
    {
        auto session = db->session();
        auto rows = storage::SymbolRepository(session).find_by_library(library_id);
        REQUIRE(rows.has_value());
        return *rows;
    }

    std::vector<core::Symbol> target_rows()
    {
        return rows_for(target_library_id);
    }

    std::vector<core::Symbol> parse_file(const fs::path& path)
    {
        auto parsed = parser::SymbolLibParser::parse(path);
        REQUIRE_MESSAGE(parsed.has_value(), parsed.error().format_message());
        return parsed->items;
    }

    services::PasteItem paste_item(std::string_view name)
    {
        const core::Symbol* symbol = find_saved(saved_symbols, name);
        REQUIRE(symbol != nullptr);
        auto exported = services::export_symbols(*db, {symbol->id()});
        REQUIRE(exported.has_value());
        for (const auto& entry : exported->items)
        {
            if (!entry.dependency && entry.name == name)
            {
                return services::PasteItem{entry.name, entry.sexpr, false};
            }
        }
        FAIL("exported item missing: " << std::string(name));
        return {};
    }

    std::vector<services::PasteItem> dependency_items(std::string_view name)
    {
        const core::Symbol* symbol = find_saved(saved_symbols, name);
        REQUIRE(symbol != nullptr);
        auto exported = services::export_symbols(*db, {symbol->id()});
        REQUIRE(exported.has_value());
        std::vector<services::PasteItem> items;
        for (const auto& entry : exported->items)
        {
            items.push_back(services::PasteItem{entry.name, entry.sexpr, entry.dependency});
        }
        return items;
    }

    services::PasteRequest paste_request(std::string operation_id,
                                         std::vector<services::PasteItem> items)
    {
        services::PasteRequest request;
        request.target_library_id = target_library_id;
        request.operation_id = std::move(operation_id);
        request.target_digest = target_digest();
        request.items = std::move(items);
        return request;
    }
};

// ---------------------------------------------------------------
// S3 剪切/移动夹具：源库 editing-test，目标库 paste-target；token 与幂等状态每个用例重置。
// ---------------------------------------------------------------
struct EditingMoveFixture : EditingPasteFixture
{
    EditingMoveFixture() { services::testing::reset_editing_state(); }

    std::string symbol_id(std::string_view name) const
    {
        const core::Symbol* symbol = find_saved(saved_symbols, name);
        REQUIRE(symbol != nullptr);
        return symbol->id();
    }

    std::vector<core::Symbol> import_into(const std::string& target_library_id,
                                          const fs::path& path)
    {
        auto parsed = parse_file(path);
        auto session = db->session();
        storage::SymbolRepository repo(session);
        std::vector<core::Symbol> rows;
        for (auto& symbol : parsed)
        {
            symbol.set_library_id(target_library_id);
            auto row = repo.insert(symbol);
            REQUIRE(row.has_value());
            rows.push_back(*row);
        }
        return rows;
    }

    services::CutResult cut_or_fail(std::vector<std::string> ids)
    {
        auto cut = services::cut_symbols(*db, ids);
        REQUIRE_MESSAGE(cut.has_value(), cut.error().format_message());
        return *cut;
    }

    services::MoveRequest move_request(std::string token, std::string operation_id,
                                       bool dry_run = false)
    {
        services::MoveRequest request;
        request.token = std::move(token);
        request.target_library_id = target_library_id;
        request.operation_id = std::move(operation_id);
        request.target_digest = target_digest();
        request.dry_run = dry_run;
        return request;
    }
};

}  // namespace

TEST_CASE_FIXTURE(EditingExportFixture, "editing export preserves the complete top-level node")
{
    const auto* symbol = find_saved(saved_symbols, "R_Complex");
    REQUIRE(symbol != nullptr);

    auto exported = services::export_symbols(*db, {symbol->id()});
    REQUIRE(exported.has_value());
    CHECK(exported->kind == "kforge.symbols");
    CHECK(exported->version == 1);
    REQUIRE(exported->items.size() == 1);

    const auto& item = exported->items[0];
    CHECK_FALSE(item.dependency);
    CHECK(item.uuid == symbol->id());
    CHECK(item.kf_id == symbol->Kicad_Forge_ID());
    CHECK_FALSE(item.kf_id.empty());
    CHECK(item.name == "R_Complex");
    CHECK(item.library_id == library_id);
    CHECK(item.library_name == "editing-test");
    CHECK(fs::u8path(item.source_path) == file);
    CHECK(item.source_digest == io::content_digest(read_bytes(file)));
    // 完整节点原文逐字一致：图形、多单元、引脚、未知节点、隐藏属性全部保留。
    CHECK(item.sexpr == std::string(kSimpleSymbol));
    CHECK(item.sexpr.find("(rectangle") != std::string::npos);
    CHECK(item.sexpr.find("(polyline") != std::string::npos);
    CHECK(item.sexpr.find("kforge_unknown_node") != std::string::npos);
    CHECK(item.sexpr.find("(pin passive line") != std::string::npos);
    CHECK(item.sexpr.find("hide") != std::string::npos);
}

TEST_CASE_FIXTURE(EditingExportFixture, "editing export includes the extends dependency closure")
{
    const auto* derived = find_saved(saved_symbols, "OpAmp_Derived");
    REQUIRE(derived != nullptr);

    auto exported = services::export_symbols(*db, {derived->id()});
    REQUIRE(exported.has_value());
    REQUIRE(exported->items.size() == 2);

    const auto& parent = exported->items[0];
    CHECK(parent.dependency);
    CHECK(parent.name == "OpAmp_Base");
    CHECK(parent.sexpr == std::string(kBaseSymbol));
    CHECK(parent.uuid.empty());  // 父节点不是直接选中项，没有 DB 身份
    CHECK(parent.library_id == library_id);

    const auto& child = exported->items[1];
    CHECK_FALSE(child.dependency);
    CHECK(child.name == "OpAmp_Derived");
    CHECK(child.uuid == derived->id());
    CHECK(child.sexpr == std::string(kDerivedSymbol));

    // 两级继承链：父 → 子 → 孙，依赖在前。
    const auto* grand = find_saved(saved_symbols, "OpAmp_GrandChild");
    REQUIRE(grand != nullptr);
    auto chain = services::export_symbols(*db, {grand->id()});
    REQUIRE(chain.has_value());
    REQUIRE(chain->items.size() == 3);
    CHECK(chain->items[0].name == "OpAmp_Base");
    CHECK(chain->items[0].dependency);
    CHECK(chain->items[0].sexpr == std::string(kBaseSymbol));
    CHECK(chain->items[1].name == "OpAmp_Derived");
    CHECK(chain->items[1].dependency);
    CHECK(chain->items[2].name == "OpAmp_GrandChild");
    CHECK_FALSE(chain->items[2].dependency);

    // 父与子同时被选中：父只出现一次，并且以「直接选中」身份出现。
    const auto* base = find_saved(saved_symbols, "OpAmp_Base");
    REQUIRE(base != nullptr);
    auto both = services::export_symbols(*db, {derived->id(), base->id()});
    REQUIRE(both.has_value());
    REQUIRE(both->items.size() == 2);
    for (const auto& item : both->items)
        CHECK_FALSE(item.dependency);
}

TEST_CASE_FIXTURE(EditingExportFixture, "editing export rejects unknown, duplicate and out-of-range ids")
{
    const auto* symbol = find_saved(saved_symbols, "R_Complex");
    REQUIRE(symbol != nullptr);

    auto missing =
        services::export_symbols(*db, {std::string("00000000-0000-0000-0000-000000000000")});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind() == util::Error::Kind::NotFound);
    CHECK(missing.error().message().find("00000000-0000-0000-0000-000000000000") !=
          std::string::npos);

    auto duplicated = services::export_symbols(*db, {symbol->id(), symbol->id(), symbol->id()});
    REQUIRE(duplicated.has_value());
    CHECK(duplicated->items.size() == 1);

    auto empty = services::export_symbols(*db, {});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind() == util::Error::Kind::ValidationError);

    std::vector<std::string> too_many(201, std::string(36, 'a'));
    auto oversized = services::export_symbols(*db, too_many);
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().kind() == util::Error::Kind::ValidationError);

    auto bad_id = services::export_symbols(*db, {std::string(65, 'b')});
    REQUIRE_FALSE(bad_id.has_value());
    CHECK(bad_id.error().kind() == util::Error::Kind::ValidationError);
}

TEST_CASE_FIXTURE(EditingExportFixture, "editing export fails when the source file or node is gone")
{
    const auto* derived = find_saved(saved_symbols, "OpAmp_Derived");
    REQUIRE(derived != nullptr);

    // 父节点从文件里消失：闭包解析必须报错，不能导出半个对象。
    write_bytes(file,
                std::string("(kicad_symbol_lib (version 20231120)\n") +
                    std::string(kDerivedSymbol) + "\n)\n");
    auto missing_parent = services::export_symbols(*db, {derived->id()});
    REQUIRE_FALSE(missing_parent.has_value());
    CHECK(missing_parent.error().kind() == util::Error::Kind::ServiceError);
    CHECK(missing_parent.error().message().find("OpAmp_Base") != std::string::npos);

    // 直接选中的符号从文件里消失：同样报错（DB 与文件不一致）。
    const auto* symbol = find_saved(saved_symbols, "R_Complex");
    REQUIRE(symbol != nullptr);
    auto missing_node = services::export_symbols(*db, {symbol->id()});
    REQUIRE_FALSE(missing_node.has_value());
    CHECK(missing_node.error().kind() == util::Error::Kind::ServiceError);
    CHECK(missing_node.error().message().find("out of sync") != std::string::npos);

    // 源文件被删除：明确的文件错误。
    REQUIRE(fs::remove(file));
    auto missing_file = services::export_symbols(*db, {symbol->id()});
    REQUIRE_FALSE(missing_file.has_value());
    CHECK(missing_file.error().kind() == util::Error::Kind::IoError);

    // 库行被移除：宁可报「找不到」，也不能拿别的库顶替。
    {
        auto session = db->session();
        REQUIRE(storage::LibraryRepository(session).remove(library_id).has_value());
    }
    auto missing_library = services::export_symbols(*db, {symbol->id()});
    REQUIRE_FALSE(missing_library.has_value());
    CHECK(missing_library.error().kind() == util::Error::Kind::NotFound);
}

// ---------------------------------------------------------------
// S2：预览只读、提交一次业务事务、冲突/幂等/锁定/闭包/容量。
// ---------------------------------------------------------------
TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste dry run reports the plan without writing")
{
    const auto before_file = read_bytes(target_file);
    const auto before_rows = target_rows().size();

    auto request = paste_request("dry-run-1", {paste_item("R_Complex")});
    auto plan = services::preview_paste(*db, request);
    REQUIRE(plan.has_value());
    CHECK(plan->target.library_id == target_library_id);
    CHECK(plan->target.name == "paste-target");
    CHECK(fs::u8path(plan->target.file_path) == target_file);
    CHECK(plan->target.digest == target_digest());
    CHECK(plan->target.exists);
    REQUIRE(plan->creates.size() == 1);
    CHECK(plan->creates[0].source_name == "R_Complex");
    CHECK(plan->creates[0].new_name == "R_Complex");
    CHECK(plan->conflicts.empty());

    // 预览不得改文件或建行。
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().size() == before_rows);
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste appends the node and inserts a new identity")
{
    auto request = paste_request("commit-1", {paste_item("R_Complex")});
    auto outcome = services::paste_symbols(*db, request);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->created.size() == 1);
    CHECK_FALSE(outcome->replayed);
    CHECK(outcome->created[0].name == "R_Complex");

    const auto* source = find_saved(saved_symbols, "R_Complex");
    REQUIRE(source != nullptr);
    CHECK(outcome->created[0].uuid != source->id());

    const auto text = read_bytes(target_file);
    CHECK(text.find("(symbol \"R_Complex\"") != std::string::npos);
    CHECK(text.find("(symbol \"R_Complex_1_1\"") != std::string::npos);
    CHECK(text.find("kforge_unknown_node") != std::string::npos);
    CHECK(text.find("(property \"Kicad_Forge_ID\"") != std::string::npos);
    CHECK(text.find("Pre_Kicad_Forge_ID") == std::string::npos);

    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id() == outcome->created[0].uuid);
    CHECK(rows[0].name() == "R_Complex");
    CHECK(rows[0].library_id() == target_library_id);
    CHECK_FALSE(rows[0].Kicad_Forge_ID().empty());

    // 落盘内容必须能被 Kicad 解析器读回，且 KF 与 DB 一致。
    auto parsed = parse_file(target_file);
    const core::Symbol* written = find_saved(parsed, "R_Complex");
    REQUIRE(written != nullptr);
    CHECK(written->Kicad_Forge_ID() == rows[0].Kicad_Forge_ID());
    CHECK(written->pins().size() == 2);
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste refuses a name conflict unless renamed")
{
    auto first = paste_request("conflict-1", {paste_item("R_Complex")});
    REQUIRE(services::paste_symbols(*db, first).has_value());

    auto blocked = paste_request("conflict-2", {paste_item("R_Complex")});
    auto plan = services::preview_paste(*db, blocked);
    REQUIRE(plan.has_value());
    REQUIRE(plan->conflicts.size() == 1);
    CHECK(plan->conflicts[0] == "R_Complex");

    const auto before_file = read_bytes(target_file);
    auto refused = services::paste_symbols(*db, blocked);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::paste_error_code(refused.error()) == "conflict");
    CHECK(services::paste_error_status("conflict") == 409);
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().size() == 1);

    // 明确改名后才允许写入；单元子符号名同步改写。
    auto renamed = paste_request("conflict-3", {paste_item("R_Complex")});
    renamed.renames["R_Complex"] = "R_Complex_Copy";
    auto outcome = services::paste_symbols(*db, renamed);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    REQUIRE(outcome->created.size() == 1);
    CHECK(outcome->created[0].name == "R_Complex_Copy");

    const auto text = read_bytes(target_file);
    CHECK(text.find("(symbol \"R_Complex_Copy\"") != std::string::npos);
    CHECK(text.find("(symbol \"R_Complex_Copy_0_1\"") != std::string::npos);
    CHECK(text.find("(symbol \"R_Complex_Copy_1_1\"") != std::string::npos);

    bool has_original = false;
    bool has_copy = false;
    for (const auto& row : target_rows())
    {
        has_original = has_original || row.name() == "R_Complex";
        has_copy = has_copy || row.name() == "R_Complex_Copy";
    }
    CHECK(has_original);
    CHECK(has_copy);
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste rejects a stale target digest")
{
    const auto before_file = read_bytes(target_file);
    auto request = paste_request("stale-1", {paste_item("OpAmp_Base")});
    request.target_digest = "0000000000000000";
    auto refused = services::paste_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::paste_error_code(refused.error()) == "source_changed");
    CHECK(services::paste_error_status("source_changed") == 409);
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste replays the same operation id without duplicating")
{
    auto request = paste_request("replay-1", {paste_item("OpAmp_Base")});
    auto first = services::paste_symbols(*db, request);
    REQUIRE(first.has_value());
    const auto after_file = read_bytes(target_file);
    const auto after_rows = target_rows().size();

    auto replay = services::paste_symbols(*db, request);
    REQUIRE(replay.has_value());
    CHECK(replay->replayed);
    REQUIRE(replay->created.size() == first->created.size());
    CHECK(replay->created[0].uuid == first->created[0].uuid);
    CHECK(read_bytes(target_file) == after_file);
    CHECK(target_rows().size() == after_rows);

    // 同 operation_id 换内容：拒绝，且不得覆盖首次内容。
    auto different = paste_request("replay-1", {paste_item("OpAmp_Derived")});
    auto conflict = services::paste_symbols(*db, different);
    REQUIRE_FALSE(conflict.has_value());
    CHECK(services::paste_error_code(conflict.error()) == "operation_conflict");
    CHECK(services::paste_error_status("operation_conflict") == 409);
    CHECK(read_bytes(target_file) == after_file);
    CHECK(target_rows().size() == after_rows);
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste refuses a locked target library")
{
    const auto owner = create_component_library("locked-owner", temp.path() / "libs", true);
    const auto locked_file = temp.path() / "libs" / "locked-target.kicad_sym";
    write_bytes(locked_file,
                "(kicad_symbol_lib (version 20231120) (generator kforge-tests)\n)\n");
    const auto locked_id = register_target_library("locked-target", locked_file, owner);

    services::PasteRequest request;
    request.target_library_id = locked_id;
    request.operation_id = "locked-1";
    request.target_digest = io::content_digest(read_bytes(locked_file));
    request.items = {paste_item("OpAmp_Base")};

    const auto before_digest = request.target_digest;
    auto refused = services::preview_paste(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::paste_error_code(refused.error()) == "target_not_writable");
    CHECK(services::paste_error_status("target_not_writable") == 403);
    CHECK(io::content_digest(read_bytes(locked_file)) == before_digest);
    CHECK(rows_for(locked_id).empty());
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste writes the inherited parent closure in order")
{
    // 只给子项、父不在目标文件里：写任何东西之前就拒绝。
    const auto before_file = read_bytes(target_file);
    auto orphan = paste_request("closure-1", {paste_item("OpAmp_Derived")});
    auto refused = services::paste_symbols(*db, orphan);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::paste_error_code(refused.error()) == "symbol_invalid");
    CHECK(services::paste_error_status("symbol_invalid") == 422);
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());

    // 带上父的完整原文（dependency=true，父在前）：子可解析，父只进文件不建行。
    auto request = paste_request("closure-2", dependency_items("OpAmp_Derived"));
    auto outcome = services::paste_symbols(*db, request);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->created.size() == 1);
    CHECK(outcome->created[0].name == "OpAmp_Derived");

    const auto text = read_bytes(target_file);
    const auto base_at = text.find("(symbol \"OpAmp_Base\"");
    const auto derived_at = text.find("(symbol \"OpAmp_Derived\"");
    REQUIRE(base_at != std::string::npos);
    REQUIRE(derived_at != std::string::npos);
    CHECK(base_at < derived_at);
    CHECK(text.find("(extends \"OpAmp_Base\")") != std::string::npos);

    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name() == "OpAmp_Derived");
    CHECK(rows[0].pin_count() == 2);

    auto parsed = parse_file(target_file);
    const core::Symbol* parent = find_saved(parsed, "OpAmp_Base");
    REQUIRE(parent != nullptr);
    CHECK(parent->Kicad_Forge_ID().empty());
    const core::Symbol* child = find_saved(parsed, "OpAmp_Derived");
    REQUIRE(child != nullptr);
    CHECK(child->Kicad_Forge_ID() == rows[0].Kicad_Forge_ID());
    CHECK(child->pins().size() == 2);
}

TEST_CASE_FIXTURE(EditingPasteFixture,
                  "editing paste rejects an unresolved footprint reference on commit")
{
    auto item = paste_item("R_Complex");
    const std::string marker = "(property \"Footprint\" \"\"";
    const auto at = item.sexpr.find(marker);
    REQUIRE(at != std::string::npos);
    item.sexpr.replace(at + marker.size() - 2, 2, "\"Missing:FP\"");

    const auto before_file = read_bytes(target_file);
    auto request = paste_request("footprint-unresolved-1", {item});
    auto plan = services::preview_paste(*db, request);
    REQUIRE(plan.has_value());
    REQUIRE_FALSE(plan->warnings.empty());
    CHECK(plan->warnings.front().rfind("footprint reference", 0) == 0);

    auto refused = services::paste_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::paste_error_code(refused.error()) == "symbol_invalid");
    CHECK(services::paste_error_status("symbol_invalid") == 422);
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste enforces the request size and item limits")
{
    services::PasteItem big;
    big.name = "Big";
    big.sexpr.assign(services::kMaxPasteBytes + 128, 'x');
    auto oversized = services::preview_paste(*db, paste_request("limit-1", {big}));
    REQUIRE_FALSE(oversized.has_value());
    CHECK(services::paste_error_code(oversized.error()) == "payload_too_large");
    CHECK(services::paste_error_status("payload_too_large") == 413);

    std::vector<services::PasteItem> many;
    for (int i = 0; i < 201; ++i)
    {
        services::PasteItem item;
        item.name = "S" + std::to_string(i);
        item.sexpr = "(symbol \"S" + std::to_string(i) + "\")";
        many.push_back(std::move(item));
    }
    auto too_many = services::preview_paste(*db, paste_request("limit-2", std::move(many)));
    REQUIRE_FALSE(too_many.has_value());
    CHECK(services::paste_error_code(too_many.error()) == "invalid_request");
    CHECK(target_rows().empty());
}

TEST_CASE_FIXTURE(EditingPasteFixture, "editing paste creates a missing target library file")
{
    const auto owner = create_component_library("create-owner", temp.path() / "libs");
    const auto new_file = temp.path() / "libs" / "new-target.kicad_sym";
    REQUIRE_FALSE(fs::exists(new_file));
    const auto new_id = register_target_library("new-target", new_file, owner);

    services::PasteRequest request;
    request.target_library_id = new_id;
    request.operation_id = "create-1";
    request.items = {paste_item("R_Complex")};

    auto plan = services::preview_paste(*db, request);
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->target.exists);
    CHECK(plan->target.digest.empty());
    CHECK_FALSE(plan->warnings.empty());
    CHECK_FALSE(fs::exists(new_file));

    auto outcome = services::paste_symbols(*db, request);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->created.size() == 1);
    REQUIRE(fs::exists(new_file));

    const auto text = read_bytes(new_file);
    CHECK(text.rfind("(kicad_symbol_lib", 0) == 0);
    CHECK(text.find("(symbol \"R_Complex\"") != std::string::npos);

    auto rows = rows_for(new_id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name() == "R_Complex");
    CHECK_FALSE(rows[0].Kicad_Forge_ID().empty());
}

// ---------------------------------------------------------------
// S3：剪切意图只读、移动原子提交、noop、冲突/依赖/幂等/token 生命周期。
// ---------------------------------------------------------------
TEST_CASE_FIXTURE(EditingMoveFixture, "editing cut builds a one-shot token without writing")
{
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto cut = cut_or_fail({symbol_id("R_Complex")});
    CHECK(cut.kind == "kforge.symbols.move");
    CHECK(cut.version == 1);
    REQUIRE(cut.token.size() == 32);
    CHECK(cut.token.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(cut.summary.find("pending") != std::string::npos);
    REQUIRE(cut.items.size() == 1);
    CHECK_FALSE(cut.items[0].dependency);
    CHECK(cut.items[0].name == "R_Complex");
    CHECK(cut.items[0].sexpr == std::string(kSimpleSymbol));

    // 剪切意图只读：文件、DB 行、目标库都不变。
    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    CHECK(rows_for(library_id).size() == 4);
    CHECK(target_rows().empty());
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing cut refuses a locked source library")
{
    const auto locked_root = temp.path() / "locked-src";
    const auto locked_file = locked_root / "locked.kicad_sym";
    write_bytes(locked_file, library_text());
    const auto owner = create_component_library("locked-source-owner", locked_root, true);
    const auto locked_library_id = register_target_library("locked-source", locked_file, owner);
    auto rows = import_into(locked_library_id, locked_file);
    REQUIRE(rows.size() == 4);
    const auto before = read_bytes(locked_file);

    auto refused = services::cut_symbols(*db, {rows[0].id()});
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::cut_error_code(refused.error()) == "target_not_writable");
    CHECK(services::cut_error_status("target_not_writable") == 403);
    CHECK(read_bytes(locked_file) == before);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing cut enforces the item limit")
{
    std::vector<std::string> too_many(services::kMaxCutItems + 1, std::string(36, 'a'));
    auto refused = services::cut_symbols(*db, too_many);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::cut_error_code(refused.error()) == "invalid_request");
    CHECK(services::cut_error_status("invalid_request") == 400);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move dry run reports the plan without writing")
{
    auto cut = cut_or_fail({symbol_id("R_Complex")});
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto plan = services::preview_move(*db, move_request(cut.token, "dry-1", true));
    REQUIRE(plan.has_value());
    CHECK_FALSE(plan->noop);
    CHECK(plan->target.library_id == target_library_id);
    CHECK(plan->target.name == "paste-target");
    CHECK(fs::u8path(plan->target.file_path) == target_file);
    CHECK(plan->target.digest == target_digest());
    REQUIRE(plan->moves.size() == 1);
    CHECK(plan->moves[0].uuid == symbol_id("R_Complex"));
    CHECK(plan->moves[0].name == "R_Complex");
    CHECK(plan->moves[0].new_name == "R_Complex");
    CHECK(plan->dependencies.empty());
    CHECK(plan->conflicts.empty());

    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    CHECK(rows_for(library_id).size() == 4);
    CHECK(target_rows().empty());

    // dry_run 不消费 token：再预览一次仍成功。
    CHECK(services::preview_move(*db, move_request(cut.token, "dry-2", true)).has_value());
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move commits file and identity in one operation")
{
    const auto id = symbol_id("R_Complex");
    const auto kf = find_saved(saved_symbols, "R_Complex")->Kicad_Forge_ID();
    auto cut = cut_or_fail({id});

    auto outcome = services::move_symbols(*db, move_request(cut.token, "move-1"));
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    CHECK_FALSE(outcome->noop);
    CHECK_FALSE(outcome->replayed);
    REQUIRE(outcome->moved.size() == 1);
    CHECK(outcome->moved[0].uuid == id);
    CHECK(outcome->moved[0].new_name == "R_Complex");

    CHECK(read_bytes(file).find("(symbol \"R_Complex\"") == std::string::npos);
    const auto target_text = read_bytes(target_file);
    CHECK(target_text.find("(symbol \"R_Complex\"") != std::string::npos);
    CHECK(target_text.find("kforge_unknown_node") != std::string::npos);

    CHECK(rows_for(library_id).size() == 3);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id() == id);
    CHECK(rows[0].name() == "R_Complex");
    CHECK(rows[0].library_id() == target_library_id);
    CHECK(rows[0].Kicad_Forge_ID() == kf);

    // 目标文件可被 KiCad 解析器读回；节点原文逐字保留，引脚不变。
    CHECK(target_text.find(std::string(kSimpleSymbol)) != std::string::npos);
    auto parsed = parse_file(target_file);
    const core::Symbol* moved = find_saved(parsed, "R_Complex");
    REQUIRE(moved != nullptr);
    CHECK(moved->pins().size() == 2);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move within the same library is a noop and consumes the token")
{
    const auto id = symbol_id("R_Complex");
    auto cut = cut_or_fail({id});
    services::MoveRequest request;
    request.token = cut.token;
    request.target_library_id = library_id;
    request.operation_id = "noop-1";
    request.target_digest = io::content_digest(read_bytes(file));
    request.dry_run = true;
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto plan = services::preview_move(*db, request);
    REQUIRE(plan.has_value());
    CHECK(plan->noop);
    CHECK(plan->moves.empty());
    CHECK(plan->conflicts.empty());

    request.dry_run = false;
    auto outcome = services::move_symbols(*db, request);
    REQUIRE(outcome.has_value());
    CHECK(outcome->noop);
    CHECK(outcome->moved.empty());
    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    CHECK(rows_for(library_id).size() == 4);
    CHECK(target_rows().empty());

    // noop 也消费 token；换 operation_id 再用同一 token 必须失效。
    request.operation_id = "noop-2";
    auto again = services::move_symbols(*db, request);
    REQUIRE_FALSE(again.has_value());
    CHECK(services::move_error_code(again.error()) == "invalid_token");
    CHECK(services::move_error_status("invalid_token") == 409);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move requires a rename on name conflict and rewrites unit names")
{
    // 目标库先粘贴一个同名对象，制造跨库同名冲突。
    auto seeded = services::paste_symbols(*db, paste_request("conflict-seed", {paste_item("R_Complex")}));
    REQUIRE(seeded.has_value());
    const auto before_target = read_bytes(target_file);
    const auto id = symbol_id("R_Complex");
    auto cut = cut_or_fail({id});
    auto request = move_request(cut.token, "conflict-1", true);

    auto plan = services::preview_move(*db, request);
    REQUIRE(plan.has_value());
    REQUIRE(plan->conflicts.size() == 1);
    CHECK(plan->conflicts[0].name == "R_Complex");
    CHECK(plan->conflicts[0].reason == "name_conflict");

    request.dry_run = false;
    auto refused = services::move_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::move_error_code(refused.error()) == "conflict");
    CHECK(services::move_error_status("conflict") == 409);
    CHECK(read_bytes(target_file) == before_target);
    CHECK(rows_for(library_id).size() == 4);

    // 明确改名后提交：多单元子符号前缀同步改写。
    request.renames["R_Complex"] = "R_Complex_Moved";
    auto outcome = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    REQUIRE(outcome->moved.size() == 1);
    CHECK(outcome->moved[0].uuid == id);
    CHECK(outcome->moved[0].new_name == "R_Complex_Moved");

    const auto text = read_bytes(target_file);
    CHECK(text.find("(symbol \"R_Complex_Moved\"") != std::string::npos);
    CHECK(text.find("(symbol \"R_Complex_Moved_0_1\"") != std::string::npos);
    CHECK(text.find("(symbol \"R_Complex_Moved_1_1\"") != std::string::npos);

    auto rows = target_rows();
    CHECK(rows.size() == 2);  // 先粘贴的副本 + 移动过来的对象
    bool has_moved = false;
    for (const auto& row : rows)
    {
        if (row.id() == id)
        {
            has_moved = true;
            CHECK(row.name() == "R_Complex_Moved");
            CHECK(row.library_id() == target_library_id);
        }
    }
    CHECK(has_moved);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move rewrites extends atoms when parents are renamed")
{
    auto cut = cut_or_fail({symbol_id("OpAmp_Base"), symbol_id("OpAmp_Derived"),
                            symbol_id("OpAmp_GrandChild")});
    REQUIRE(cut.items.size() == 3);

    auto request = move_request(cut.token, "extends-1");
    request.renames = {{"OpAmp_Base", "OpAmp_Base2"},
                       {"OpAmp_Derived", "OpAmp_Derived2"},
                       {"OpAmp_GrandChild", "OpAmp_GrandChild2"}};
    auto outcome = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    CHECK(outcome->moved.size() == 3);

    const auto text = read_bytes(target_file);
    CHECK(text.find("(symbol \"OpAmp_Derived2\"") != std::string::npos);
    CHECK(text.find("(extends \"OpAmp_Base2\")") != std::string::npos);
    CHECK(text.find("(extends \"OpAmp_Derived2\")") != std::string::npos);
    CHECK(read_bytes(file).find("OpAmp_") == std::string::npos);

    auto rows = target_rows();
    CHECK(rows.size() == 3);
    auto parsed = parse_file(target_file);
    const core::Symbol* child = find_saved(parsed, "OpAmp_Derived2");
    REQUIRE(child != nullptr);
    CHECK(child->property("extends") == "OpAmp_Base2");
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move refuses to strand derived symbols")
{
    auto cut = cut_or_fail({symbol_id("OpAmp_Base")});
    auto request = move_request(cut.token, "derived-1", true);

    auto plan = services::preview_move(*db, request);
    REQUIRE(plan.has_value());
    REQUIRE(plan->conflicts.size() == 1);
    CHECK(plan->conflicts[0].name == "OpAmp_Derived");
    CHECK(plan->conflicts[0].reason == "unmoved_derivatives");

    const auto source_before = read_bytes(file);
    request.dry_run = false;
    auto refused = services::move_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::move_error_code(refused.error()) == "conflict");
    CHECK(services::move_error_status("conflict") == 409);
    CHECK(read_bytes(file) == source_before);
    CHECK(rows_for(library_id).size() == 4);
    CHECK(target_rows().empty());
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move rejects a changed source and keeps the token")
{
    auto cut = cut_or_fail({symbol_id("R_Complex")});
    const auto original = read_bytes(file);
    write_bytes(file, original + "\n");  // 仅尾随空白变化，仍是合法库

    auto request = move_request(cut.token, "stale-1");
    auto refused = services::move_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::move_error_code(refused.error()) == "source_changed");
    CHECK(services::move_error_status("source_changed") == 409);
    CHECK(rows_for(library_id).size() == 4);
    CHECK(target_rows().empty());

    // 失败不消费 token：恢复摘要后可继续提交。
    write_bytes(file, original);
    auto outcome = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    CHECK(outcome->moved.size() == 1);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move rejects unknown, expired and consumed tokens")
{
    auto unknown = services::preview_move(
        *db, move_request("0123456789abcdef0123456789abcdef", "token-1", true));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(services::move_error_code(unknown.error()) == "invalid_token");
    CHECK(services::move_error_status("invalid_token") == 409);

    auto expiring = cut_or_fail({symbol_id("R_Complex")});
    services::testing::advance_cut_clock(std::chrono::seconds(31 * 60));
    auto expired =
        services::preview_move(*db, move_request(expiring.token, "token-2", true));
    REQUIRE_FALSE(expired.has_value());
    CHECK(services::move_error_code(expired.error()) == "invalid_token");

    auto cut = cut_or_fail({symbol_id("R_Complex")});
    auto moved = services::move_symbols(*db, move_request(cut.token, "token-3"));
    REQUIRE(moved.has_value());
    auto consumed =
        services::preview_move(*db, move_request(cut.token, "token-4", true));
    REQUIRE_FALSE(consumed.has_value());
    CHECK(services::move_error_code(consumed.error()) == "invalid_token");
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move refuses a locked target library")
{
    const auto locked_root = temp.path() / "locked-move-target";
    const auto locked_file = locked_root / "locked-target.kicad_sym";
    write_bytes(locked_file,
                "(kicad_symbol_lib (version 20231120) (generator kforge-tests)\n)\n");
    const auto owner = create_component_library("locked-move-owner", locked_root, true);
    const auto locked_library_id =
        register_target_library("locked-move-target", locked_file, owner);
    auto cut = cut_or_fail({symbol_id("R_Complex")});

    services::MoveRequest request;
    request.token = cut.token;
    request.target_library_id = locked_library_id;
    request.operation_id = "locked-1";
    request.target_digest = io::content_digest(read_bytes(locked_file));
    request.dry_run = true;

    auto plan = services::preview_move(*db, request);
    REQUIRE_FALSE(plan.has_value());
    CHECK(services::move_error_code(plan.error()) == "target_not_writable");
    CHECK(services::move_error_status("target_not_writable") == 403);
    CHECK(rows_for(locked_library_id).empty());

    request.dry_run = false;
    auto refused = services::move_symbols(*db, request);
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::move_error_code(refused.error()) == "target_not_writable");
    CHECK(read_bytes(file).find("(symbol \"R_Complex\"") != std::string::npos);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move creates a missing target library file")
{
    const auto root = temp.path() / "created-move";
    const auto new_file = root / "moved-target.kicad_sym";
    REQUIRE_FALSE(fs::exists(new_file));
    const auto owner = create_component_library("created-move-owner", root);
    const auto new_library_id = register_target_library("created-move-target", new_file, owner);
    auto cut = cut_or_fail({symbol_id("R_Complex")});

    services::MoveRequest request;
    request.token = cut.token;
    request.target_library_id = new_library_id;
    request.operation_id = "create-1";
    request.dry_run = true;

    auto plan = services::preview_move(*db, request);
    REQUIRE(plan.has_value());
    CHECK(plan->target.digest.empty());
    CHECK_FALSE(plan->warnings.empty());
    CHECK_FALSE(fs::exists(new_file));

    request.dry_run = false;
    auto outcome = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(outcome.has_value(), outcome.error().format_message());
    REQUIRE(fs::exists(new_file));
    const auto text = read_bytes(new_file);
    CHECK(text.rfind("(kicad_symbol_lib", 0) == 0);
    CHECK(text.find("(symbol \"R_Complex\"") != std::string::npos);

    auto rows = rows_for(new_library_id);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name() == "R_Complex");
    CHECK(rows[0].Kicad_Forge_ID() ==
          find_saved(saved_symbols, "R_Complex")->Kicad_Forge_ID());
    CHECK(rows_for(library_id).size() == 3);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move replays the same operation id without duplicating")
{
    const auto id = symbol_id("R_Complex");
    auto cut = cut_or_fail({id});
    auto request = move_request(cut.token, "replay-move-1");
    auto first = services::move_symbols(*db, request);
    REQUIRE(first.has_value());
    const auto target_after = read_bytes(target_file);
    const auto source_after = read_bytes(file);

    auto replay = services::move_symbols(*db, request);
    REQUIRE(replay.has_value());
    CHECK(replay->replayed);
    REQUIRE(replay->moved.size() == 1);
    CHECK(replay->moved[0].uuid == id);
    CHECK(read_bytes(target_file) == target_after);
    CHECK(read_bytes(file) == source_after);
    CHECK(target_rows().size() == 1);

    // 同 operation_id 换内容：拒绝且不产生第二次移动。
    request.renames["R_Complex"] = "R_Complex_Again";
    auto conflict = services::move_symbols(*db, request);
    REQUIRE_FALSE(conflict.has_value());
    CHECK(services::move_error_code(conflict.error()) == "operation_conflict");
    CHECK(services::move_error_status("operation_conflict") == 409);
    CHECK(target_rows().size() == 1);
}

TEST_CASE_FIXTURE(EditingMoveFixture, "editing move rejects a missing extends parent")
{
    const auto* derived = find_saved(saved_symbols, "OpAmp_Derived");
    const auto* grand = find_saved(saved_symbols, "OpAmp_GrandChild");
    REQUIRE(derived != nullptr);
    REQUIRE(grand != nullptr);
    // 同时选中子与孙：闭包里只有 Base 是 dependency，故意把它丢掉，
    // 模拟损坏的剪贴板内容；正常 cut 总会带出完整闭包。
    auto exported = services::export_symbols(*db, {derived->id(), grand->id()});
    REQUIRE(exported.has_value());
    std::vector<services::ExportedSymbol> items;
    for (const auto& item : exported->items)
    {
        if (!item.dependency)
            items.push_back(item);
    }
    REQUIRE(items.size() == 2);
    const auto token = services::testing::install_cut_intent_for_test(items);
    REQUIRE(token.size() == 32);

    auto plan = services::preview_move(*db, move_request(token, "missing-parent-1", true));
    REQUIRE(plan.has_value());
    REQUIRE(plan->conflicts.size() == 1);
    CHECK(plan->conflicts[0].name == "OpAmp_Derived");
    CHECK(plan->conflicts[0].reason == "missing_parent");

    auto refused = services::move_symbols(*db, move_request(token, "missing-parent-2"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(services::move_error_code(refused.error()) == "conflict");
    CHECK(services::move_error_status("conflict") == 409);
}

// ---------------------------------------------------------------
// S4 历史：撤销/重做必须整体恢复文件字节与 DB 行，冲突时指针不动。
// 每个用例由 fixture 重置历史、移动幂等与 cut token；粘贴 operation_id 全局唯一。
// ---------------------------------------------------------------
struct EditingHistoryFixture : EditingMoveFixture
{
};

const core::Symbol* row_named(const std::vector<core::Symbol>& rows, std::string_view name)
{
    for (const auto& row : rows)
    {
        if (row.name() == name)
            return &row;
    }
    return nullptr;
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history undo and redo restore a pasted symbol byte for byte")
{
    const auto before_file = read_bytes(target_file);
    auto pasted = services::paste_symbols(*db, paste_request("history-paste-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);

    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    const std::string kf = rows.front().Kicad_Forge_ID();
    CHECK(rows.front().id() == uuid);
    CHECK(rows.front().pin_count() == 2);
    CHECK(rows.front().default_value() == "10k");

    auto state = services::history_state();
    REQUIRE(state.count == 1);
    CHECK(state.bytes > 0);
    CHECK(state.can_undo);
    CHECK_FALSE(state.can_redo);
    REQUIRE(state.entries.size() == 1);
    CHECK(state.entries.front().kind == "paste");
    CHECK(state.entries.front().count == 1);
    CHECK(state.entries.front().target_library_name == "paste-target");
    CHECK(state.entries.front().source_library_name.empty());
    CHECK(state.entries.front().created_at_ms > 0);

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(undone->kind == "paste");
    CHECK(undone->count == 1);
    REQUIRE(undone->restored.size() == 1);
    CHECK(undone->restored.front().uuid == uuid);
    CHECK(undone->restored.front().name == "R_Complex");
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
    state = services::history_state();
    CHECK(state.count == 1);
    CHECK_FALSE(state.can_undo);
    CHECK(state.can_redo);

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(redone->kind == "paste");
    CHECK(read_bytes(target_file) == after_file);
    rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().id() == uuid);
    CHECK(rows.front().Kicad_Forge_ID() == kf);
    CHECK(rows.front().pin_count() == 2);
    CHECK(rows.front().default_value() == "10k");
    CHECK(rows.front().library_id() == target_library_id);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects an undo after the file changed externally")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-file-conflict-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    const auto after_file = read_bytes(target_file);
    write_bytes(target_file, after_file + "\n");
    const auto modified = read_bytes(target_file);
    CHECK(modified != after_file);

    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_conflict");
    CHECK(services::history_error_status("history_conflict") == 409);
    CHECK(read_bytes(target_file) == modified);
    CHECK(target_rows().size() == 1);
    const auto state = services::history_state();
    CHECK(state.can_undo);
    CHECK_FALSE(state.can_redo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects an undo after the row changed externally")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-row-conflict-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);
    {
        auto session = db->session();
        storage::SymbolRepository repository(session);
        auto row = repository.find_by_id(uuid);
        REQUIRE(row.has_value());
        row->set_name("R_Complex_External");
        auto updated = repository.update(*row);
        REQUIRE_MESSAGE(updated.has_value(), updated.error().format_message());
    }

    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_conflict");
    CHECK(read_bytes(target_file) == after_file);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Complex_External");
    CHECK(services::history_state().can_undo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects an undo after the row was deleted")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-row-delete-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);
    {
        auto session = db->session();
        storage::SymbolRepository repository(session);
        auto removed = repository.remove(uuid);
        REQUIRE_MESSAGE(removed.has_value(), removed.error().format_message());
    }

    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_conflict");
    CHECK(read_bytes(target_file) == after_file);
    CHECK(target_rows().empty());
    CHECK(services::history_state().can_undo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history reports empty undo and redo without moving")
{
    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_empty");
    CHECK(services::history_error_status("history_empty") == 409);

    auto redone = services::redo_history(*db);
    REQUIRE_FALSE(redone.has_value());
    CHECK(services::history_error_code(redone.error()) == "history_empty");
    const auto state = services::history_state();
    CHECK(state.count == 0);
    CHECK_FALSE(state.can_undo);
    CHECK_FALSE(state.can_redo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history ignores previews, dry runs, failures and replays")
{
    auto preview = services::preview_paste(*db, paste_request("history-preview-1", {paste_item("R_Complex")}));
    REQUIRE(preview.has_value());
    CHECK(services::history_state().count == 0);

    const auto* simple = find_saved(saved_symbols, "R_Complex");
    REQUIRE(simple != nullptr);
    auto exported = services::export_symbols(*db, {simple->id()});
    REQUIRE(exported.has_value());
    CHECK(services::history_state().count == 0);

    auto dry_request = paste_request("history-dry-1", {paste_item("R_Complex")});
    dry_request.dry_run = true;
    auto dry = services::paste_symbols(*db, dry_request);
    REQUIRE_FALSE(dry.has_value());
    CHECK(services::history_state().count == 0);

    auto first = services::paste_symbols(*db, paste_request("history-replay-1", {paste_item("OpAmp_Base")}));
    REQUIRE_MESSAGE(first.has_value(), first.error().format_message());
    CHECK(services::history_state().count == 1);

    auto replay = services::paste_symbols(*db, paste_request("history-replay-1", {paste_item("OpAmp_Base")}));
    REQUIRE_MESSAGE(replay.has_value(), replay.error().format_message());
    CHECK(replay->replayed);
    CHECK(services::history_state().count == 1);

    auto failed = services::paste_symbols(*db, paste_request("history-failed-1", {paste_item("OpAmp_Base")}));
    REQUIRE_FALSE(failed.has_value());
    CHECK(services::history_state().count == 1);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history keeps only the newest entry when the entry capacity is one")
{
    services::testing::set_history_limits(1, services::kMaxHistoryBytes, services::kMaxHistoryFileBytes);

    auto first = services::paste_symbols(*db, paste_request("history-capacity-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(first.has_value(), first.error().format_message());
    const auto after_first = read_bytes(target_file);

    auto second = services::paste_symbols(*db, paste_request("history-capacity-2", {paste_item("OpAmp_Base")}));
    REQUIRE_MESSAGE(second.has_value(), second.error().format_message());
    auto state = services::history_state();
    REQUIRE(state.count == 1);
    CHECK(state.can_undo);
    CHECK_FALSE(state.can_redo);

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(read_bytes(target_file) == after_first);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Complex");
    state = services::history_state();
    CHECK(state.count == 1);
    CHECK_FALSE(state.can_undo);
    CHECK(state.can_redo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects a paste that exceeds the snapshot budget before writing")
{
    const auto before_file = read_bytes(target_file);

    services::testing::set_history_limits(20, 128, services::kMaxHistoryFileBytes);
    auto too_large = services::paste_symbols(*db, paste_request("history-full-1", {paste_item("R_Complex")}));
    REQUIRE_FALSE(too_large.has_value());
    CHECK(services::paste_error_code(too_large.error()) == "history_full");
    CHECK(services::paste_error_status("history_full") == 409);
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
    CHECK(services::history_state().count == 0);

    services::testing::set_history_limits(20, services::kMaxHistoryBytes, 64);
    auto file_too_large =
        services::paste_symbols(*db, paste_request("history-full-2", {paste_item("R_Complex")}));
    REQUIRE_FALSE(file_too_large.has_value());
    CHECK(services::paste_error_code(file_too_large.error()) == "history_full");
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
    CHECK(services::history_state().count == 0);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history undoes and redoes a move across two files and libraries")
{
    const auto* source = find_saved(saved_symbols, "R_Complex");
    REQUIRE(source != nullptr);
    const std::string uuid = source->id();
    const std::string kf = source->Kicad_Forge_ID();
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto cut = cut_or_fail({uuid});
    auto moved = services::move_symbols(*db, move_request(cut.token, "history-move-1"));
    REQUIRE_MESSAGE(moved.has_value(), moved.error().format_message());
    REQUIRE(moved->moved.size() == 1);
    CHECK_FALSE(moved->noop);
    CHECK(moved->moved.front().uuid == uuid);

    const auto source_after = read_bytes(file);
    const auto target_after = read_bytes(target_file);
    CHECK(source_after != source_before);
    CHECK(target_after != target_before);

    auto state = services::history_state();
    REQUIRE(state.count == 1);
    REQUIRE(state.entries.size() == 1);
    CHECK(state.entries.front().kind == "move");
    CHECK(state.entries.front().count == 1);
    CHECK(state.entries.front().target_library_name == "paste-target");
    CHECK(state.entries.front().source_library_name == "editing-test");

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(undone->kind == "move");
    CHECK(undone->count == 1);
    REQUIRE(undone->restored.size() == 1);
    CHECK(undone->restored.front().uuid == uuid);
    CHECK(undone->restored.front().name == "R_Complex");
    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    CHECK(target_rows().empty());
    auto source_rows = rows_for(library_id);
    REQUIRE(source_rows.size() == 4);
    const auto* restored = row_named(source_rows, "R_Complex");
    REQUIRE(restored != nullptr);
    CHECK(restored->id() == uuid);
    CHECK(restored->Kicad_Forge_ID() == kf);

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(redone->kind == "move");
    CHECK(read_bytes(file) == source_after);
    CHECK(read_bytes(target_file) == target_after);
    auto target_rows_after = target_rows();
    REQUIRE(target_rows_after.size() == 1);
    CHECK(target_rows_after.front().id() == uuid);
    CHECK(target_rows_after.front().name() == "R_Complex");
    CHECK(target_rows_after.front().Kicad_Forge_ID() == kf);
    CHECK(target_rows_after.front().library_id() == target_library_id);
    CHECK(rows_for(library_id).size() == 3);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history clears redo after a new successful paste")
{
    const auto before_file = read_bytes(target_file);
    auto first = services::paste_symbols(*db, paste_request("history-redo-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(first.has_value(), first.error().format_message());
    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(services::history_state().can_redo);

    auto second = services::paste_symbols(*db, paste_request("history-redo-2", {paste_item("OpAmp_Base")}));
    REQUIRE_MESSAGE(second.has_value(), second.error().format_message());
    auto state = services::history_state();
    REQUIRE(state.count == 1);
    CHECK(state.can_undo);
    CHECK_FALSE(state.can_redo);
    REQUIRE(state.entries.size() == 1);
    CHECK(state.entries.front().kind == "paste");

    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "OpAmp_Base");
    CHECK_FALSE(row_named(rows, "R_Complex") != nullptr);

    auto undone_again = services::undo_history(*db);
    REQUIRE_MESSAGE(undone_again.has_value(), undone_again.error().format_message());
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());
    CHECK_FALSE(services::history_state().can_undo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history removes and recreates a target file created by paste")
{
    const fs::path new_file = temp.path() / "libs" / "history-target.kicad_sym";
    const std::string owner = create_component_library("history-owner", temp.path() / "libs");
    const std::string library = register_target_library("history-target", new_file, owner);

    services::PasteRequest request;
    request.target_library_id = library;
    request.operation_id = "history-new-file-1";
    request.target_digest = "";
    request.items = {paste_item("R_Complex")};
    auto pasted = services::paste_symbols(*db, request);
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(new_file);
    auto rows = rows_for(library);
    REQUIRE(rows.size() == 1);
    const std::string kf = rows.front().Kicad_Forge_ID();

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK_FALSE(fs::exists(new_file));
    CHECK(rows_for(library).empty());

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(fs::exists(new_file));
    CHECK(read_bytes(new_file) == after_file);
    rows = rows_for(library);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().id() == uuid);
    CHECK(rows.front().Kicad_Forge_ID() == kf);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history does not record a move that is a noop")
{
    const std::string uuid = find_saved(saved_symbols, "R_Complex")->id();
    auto cut = cut_or_fail({uuid});
    const auto source_before = read_bytes(file);
    auto request = move_request(cut.token, "history-noop-1");
    request.target_library_id = library_id;
    auto moved = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(moved.has_value(), moved.error().format_message());
    CHECK(moved->noop);
    CHECK(moved->moved.empty());
    CHECK(read_bytes(file) == source_before);
    CHECK(services::history_state().count == 0);
    CHECK(rows_for(library_id).size() == 4);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history walks two paste entries in oldest to newest order")
{
    const auto before_file = read_bytes(target_file);
    auto first = services::paste_symbols(*db, paste_request("history-order-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(first.has_value(), first.error().format_message());
    const auto after_first = read_bytes(target_file);
    auto second = services::paste_symbols(*db, paste_request("history-order-2", {paste_item("OpAmp_Base")}));
    REQUIRE_MESSAGE(second.has_value(), second.error().format_message());
    const auto after_second = read_bytes(target_file);

    auto state = services::history_state();
    REQUIRE(state.count == 2);
    REQUIRE(state.entries.size() == 2);
    CHECK(state.entries[0].kind == "paste");
    CHECK(state.entries[1].kind == "paste");
    CHECK(state.entries[0].created_at_ms <= state.entries[1].created_at_ms);
    CHECK(state.undo_index == 1);
    CHECK(state.redo_index == 2);

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(read_bytes(target_file) == after_first);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Complex");
    state = services::history_state();
    CHECK(state.can_undo);
    CHECK(state.can_redo);
    CHECK(state.undo_index == 0);
    CHECK(state.redo_index == 1);

    auto undone_twice = services::undo_history(*db);
    REQUIRE_MESSAGE(undone_twice.has_value(), undone_twice.error().format_message());
    CHECK(read_bytes(target_file) == before_file);
    CHECK_FALSE(services::history_state().can_undo);

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(read_bytes(target_file) == after_first);
    auto redone_again = services::redo_history(*db);
    REQUIRE_MESSAGE(redone_again.has_value(), redone_again.error().format_message());
    CHECK(read_bytes(target_file) == after_second);
    CHECK(target_rows().size() == 2);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history keeps the paste dependency closure across undo and redo")
{
    const auto before_file = read_bytes(target_file);
    auto pasted = services::paste_symbols(*db, paste_request("history-closure-1", dependency_items("OpAmp_Derived")));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    const std::string kf = rows.front().Kicad_Forge_ID();
    auto parsed = parse_file(target_file);
    CHECK(parsed.size() == 2);
    CHECK(row_named(rows, "OpAmp_Derived") != nullptr);

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(read_bytes(target_file) == before_file);
    CHECK(target_rows().empty());

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(read_bytes(target_file) == after_file);
    rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().id() == uuid);
    CHECK(rows.front().name() == "OpAmp_Derived");
    CHECK(rows.front().Kicad_Forge_ID() == kf);
    parsed = parse_file(target_file);
    REQUIRE(parsed.size() == 2);
    const auto* derived = row_named(parsed, "OpAmp_Derived");
    REQUIRE(derived != nullptr);
    CHECK(derived->property("extends") == "OpAmp_Base");
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history undoes and redoes a renamed move")
{
    const auto* source = find_saved(saved_symbols, "R_Complex");
    REQUIRE(source != nullptr);
    const std::string uuid = source->id();
    const std::string kf = source->Kicad_Forge_ID();
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto cut = cut_or_fail({uuid});
    auto request = move_request(cut.token, "history-move-rename-1");
    request.renames["R_Complex"] = "R_Moved";
    auto moved = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(moved.has_value(), moved.error().format_message());
    REQUIRE(moved->moved.size() == 1);
    CHECK(moved->moved.front().new_name == "R_Moved");
    const auto source_after = read_bytes(file);
    const auto target_after = read_bytes(target_file);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Moved");
    CHECK(rows.front().Kicad_Forge_ID() == kf);

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    CHECK(target_rows().empty());
    auto source_rows = rows_for(library_id);
    REQUIRE(source_rows.size() == 4);
    const auto* restored = row_named(source_rows, "R_Complex");
    REQUIRE(restored != nullptr);
    CHECK(restored->id() == uuid);
    CHECK(restored->Kicad_Forge_ID() == kf);

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    CHECK(read_bytes(file) == source_after);
    CHECK(read_bytes(target_file) == target_after);
    rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().id() == uuid);
    CHECK(rows.front().name() == "R_Moved");
    CHECK(rows.front().Kicad_Forge_ID() == kf);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects a paste undo after later metadata was edited")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-meta-conflict-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);
    {
        auto session = db->session();
        storage::SymbolRepository repository(session);
        auto row = repository.find_by_id(uuid);
        REQUIRE(row.has_value());
        row->set_description("later metadata edit");
        auto updated = repository.update(*row);
        REQUIRE_MESSAGE(updated.has_value(), updated.error().format_message());
    }

    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_conflict");
    CHECK(read_bytes(target_file) == after_file);
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().description() == "later metadata edit");
    CHECK(services::history_state().can_undo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history rejects a paste undo after a footprint relationship was added")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-link-conflict-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    REQUIRE(pasted->created.size() == 1);
    const std::string uuid = pasted->created.front().uuid;
    const auto after_file = read_bytes(target_file);
    db->execute("INSERT INTO footprints(id,name,library_path) VALUES ('history-fp','HistoryFP','history.kicad_mod')");
    db->execute("INSERT INTO symbol_footprint_links(id,symbol_id,footprint_id,link_type) "
                "VALUES ('history-link','" + uuid + "','history-fp','manual')");

    auto undone = services::undo_history(*db);
    REQUIRE_FALSE(undone.has_value());
    CHECK(services::history_error_code(undone.error()) == "history_conflict");
    CHECK(read_bytes(target_file) == after_file);
    CHECK(target_rows().size() == 1);
    {
        auto session = db->session();
        storage::RelationshipRepository relationships(session);
        auto links = relationships.find_symbol_link_rows(uuid);
        REQUIRE(links.has_value());
        CHECK(links->size() == 1);
    }
    CHECK(services::history_state().can_undo);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history clears redo after another successful symbol delete")
{
    auto pasted =
        services::paste_symbols(*db, paste_request("history-delete-redo-1", {paste_item("R_Complex")}));
    REQUIRE_MESSAGE(pasted.has_value(), pasted.error().format_message());
    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    REQUIRE(services::history_state().can_redo);

    services::LibraryService library_service(db.get());
    auto removed = library_service.delete_symbol(symbol_id("OpAmp_GrandChild"));
    REQUIRE_MESSAGE(removed.has_value(), removed.error().format_message());
    CHECK_FALSE(services::history_state().can_redo);
    CHECK(rows_for(library_id).size() == 3);
}

TEST_CASE_FIXTURE(EditingHistoryFixture, "editing history restores lib_id across a renamed cross-library move")
{
    const auto* source = find_saved(saved_symbols, "R_Complex");
    REQUIRE(source != nullptr);
    const std::string uuid = source->id();
    db->execute("UPDATE symbols SET lib_id='editing-test:R_Complex' WHERE id='" + uuid + "'");
    const auto source_before = read_bytes(file);
    const auto target_before = read_bytes(target_file);

    auto cut = cut_or_fail({uuid});
    auto request = move_request(cut.token, "history-lib-id-1");
    request.renames["R_Complex"] = "R_Moved";
    auto moved = services::move_symbols(*db, request);
    REQUIRE_MESSAGE(moved.has_value(), moved.error().format_message());
    auto rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Moved");
    CHECK(rows.front().lib_id() == "paste-target:R_Moved");

    auto undone = services::undo_history(*db);
    REQUIRE_MESSAGE(undone.has_value(), undone.error().format_message());
    CHECK(read_bytes(file) == source_before);
    CHECK(read_bytes(target_file) == target_before);
    auto source_rows = rows_for(library_id);
    REQUIRE(source_rows.size() == 4);
    const auto* restored = row_named(source_rows, "R_Complex");
    REQUIRE(restored != nullptr);
    CHECK(restored->lib_id() == "editing-test:R_Complex");

    auto redone = services::redo_history(*db);
    REQUIRE_MESSAGE(redone.has_value(), redone.error().format_message());
    rows = target_rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().name() == "R_Moved");
    CHECK(rows.front().lib_id() == "paste-target:R_Moved");
}
