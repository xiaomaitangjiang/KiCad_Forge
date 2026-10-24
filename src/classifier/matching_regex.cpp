#include "classifier/matching_regex.h"

// 静态链接：不定义 PCRE2_STATIC 时 pcre2.h 会把函数声明成 __declspec(dllimport)，
// 与 libpcre2-8.a 里的普通符号对不上。这个宏只影响本 TU，不需要污染其他目标。
#define PCRE2_STATIC 1
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <cstddef>
#include <utility>

namespace kforge::classifier {

namespace {

util::Error invalid(std::string message)
{
    return util::Error::make<util::Error::Kind::ValidationError>(std::move(message));
}

/// PCRE2 的错误文本；未知错误码退化成数字，不抛也不吞。
std::string pcre2_error_text(int code)
{
    PCRE2_UCHAR buffer[128] = {};
    const int written = pcre2_get_error_message(code, buffer, sizeof(buffer));
    if (written < 0)
        return "PCRE2 error " + std::to_string(code);
    return std::string(reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(written));
}

/// callout 预算：一次 RegexSession 一份，回调只做比较与自增 —— 不分配、不抛、不碰锁。
struct CalloutBudget
{
    std::uint64_t used = 0;
    std::uint64_t limit = kMaxCalloutsPerEvaluation;
};

/// 返回负值（PCRE2_ERROR_CALLOUT）才会中止整次匹配；返回正值只会让当前分支按
/// 「断言失败」回溯，预算就白设了。
int PCRE2_CALL_CONVENTION count_callout(pcre2_callout_block*, void* data) noexcept
{
    auto* budget = static_cast<CalloutBudget*>(data);
    if (budget == nullptr || budget->used >= budget->limit)
        return PCRE2_ERROR_CALLOUT;
    ++budget->used;
    return 0;
}

/// 空文本也要给一个合法指针：模式与主题都可能被传成空的 string_view，
/// 不能让引擎拿到空指针（长度是显式传的，但指针本身必须有效）。
const char kEmptyText = '\0';

const char* text_data(std::string_view text)
{
    return text.empty() ? &kEmptyText : text.data();
}

/// 编译 context 的 SDK 释放器。成功创建的 PCRE2 资源从创建点起就归 unique_ptr 管：
/// 限额设置失败的早退、std::make_unique<Compiled> 抛出的 std::bad_alloc、
/// 以及正常走完的路径都走同一条释放路径，没有「裸 context 等人手 release」的窗口。
struct CompileContextDeleter
{
    void operator()(pcre2_compile_context* context) const noexcept
    {
        if (context != nullptr)
            pcre2_compile_context_free(context);
    }
};

using CompileContextPtr = std::unique_ptr<pcre2_compile_context, CompileContextDeleter>;

}  // namespace

struct RegexProgram::Compiled
{
    pcre2_code* code = nullptr;

    ~Compiled()
    {
        if (code != nullptr)
            pcre2_code_free(code);
    }
};

RegexProgram::RegexProgram() = default;
RegexProgram::~RegexProgram() = default;
RegexProgram::RegexProgram(RegexProgram&&) noexcept = default;
RegexProgram& RegexProgram::operator=(RegexProgram&&) noexcept = default;

bool RegexProgram::valid() const
{
    return compiled_ != nullptr && compiled_->code != nullptr;
}

util::Result<RegexProgram> RegexProgram::compile(std::string_view pattern, bool case_sensitive,
                                                 const std::string& where)
{
    // 创建成功即纳入 RAII：后续任何早退或 C++ 异常（含 std::make_unique 的
    // std::bad_alloc）都由 unique_ptr 释放这个真实 SDK 资源。
    CompileContextPtr context(pcre2_compile_context_create(nullptr));
    if (!context)
        return std::unexpected(invalid(where + ".pattern: cannot create a PCRE2 compile context"));

    // 每个设置的返回值都真实处理：没设上就不发布这条规则。
    if (pcre2_set_parens_nest_limit(context.get(), kRegexParensNestLimit) != 0 ||
        pcre2_set_newline(context.get(), PCRE2_NEWLINE_ANYCRLF) != 0)
    {
        return std::unexpected(invalid(where + ".pattern: PCRE2 rejected the compile limits"));
    }

    // AUTO_CALLOUT 是预算计数的来源；DOLLAR_ENDONLY 让 $ 只匹配主题末尾；
    // NEVER_UTF/NEVER_UCP 明确锁死「8-bit 按字节」的语义（同时也拒绝 (*UTF)/(*UCP)）。
    // 不开 multiline/dotall/JIT。
    std::uint32_t options =
        PCRE2_AUTO_CALLOUT | PCRE2_DOLLAR_ENDONLY | PCRE2_NEVER_UTF | PCRE2_NEVER_UCP;
    if (!case_sensitive)
        options |= PCRE2_CASELESS;

    // 这一次分配是真实的异常窗口：它抛 std::bad_alloc 时 context 已由 unique_ptr 持有，
    // 栈展开会释放它，不会留下存活的 compile context。
    auto compiled = std::make_unique<Compiled>();
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    compiled->code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(text_data(pattern)), pattern.size(),
                                   options, &error_code, &error_offset, context.get());
    // 编译已返回，context 不再被需要：在这一点释放，与原来的释放时机一致。
    context.reset();
    if (compiled->code == nullptr)
    {
        return std::unexpected(invalid(where + ".pattern is not a valid PCRE2 pattern at offset " +
                                       std::to_string(static_cast<std::size_t>(error_offset)) + ": " +
                                       pcre2_error_text(error_code)));
    }

    RegexProgram program;
    program.compiled_ = std::move(compiled);
    return program;
}

struct RegexSession::Scratch
{
    pcre2_match_context* context = nullptr;
    CalloutBudget budget;

    ~Scratch()
    {
        if (context != nullptr)
            pcre2_match_context_free(context);
    }
};

RegexSession::RegexSession()
{
    auto scratch = std::make_unique<Scratch>();
    scratch->context = pcre2_match_context_create(nullptr);
    if (scratch->context != nullptr)
    {
        // 三个限额与 callout 都设上，这条会话才算可用：任何一项失败都不装作有限额。
        const bool ready = pcre2_set_match_limit(scratch->context, kRegexMatchLimit) == 0 &&
                           pcre2_set_depth_limit(scratch->context, kRegexDepthLimit) == 0 &&
                           pcre2_set_heap_limit(scratch->context, kRegexHeapLimitKib) == 0 &&
                           pcre2_set_callout(scratch->context, count_callout, &scratch->budget) == 0;
        if (!ready)
        {
            pcre2_match_context_free(scratch->context);
            scratch->context = nullptr;
        }
    }
    scratch_ = std::move(scratch);
}

RegexSession::~RegexSession() = default;

bool RegexSession::valid() const
{
    return scratch_ != nullptr && scratch_->context != nullptr;
}

RegexMatchResult RegexSession::search(const RegexProgram& program, std::string_view text)
{
    RegexMatchResult result;
    if (!valid() || !program.valid())
    {
        result.outcome = RegexOutcome::EngineError;
        result.error_code = 0;
        result.error_message = "the PCRE2 match context is not available";
        return result;
    }

    // match data 每次搜索独立：不在并发搜索之间共享可变状态。
    pcre2_match_data* data = pcre2_match_data_create_from_pattern(program.compiled_->code, nullptr);
    if (data == nullptr)
    {
        result.outcome = RegexOutcome::EngineError;
        result.error_message = "cannot allocate PCRE2 match data";
        return result;
    }

    // 起点 0、不带给任何选项 = PCRE2 的 regex_search：逐个起点尝试直到命中或穷尽。
    const int rc = pcre2_match(program.compiled_->code,
                               reinterpret_cast<PCRE2_SPTR>(text_data(text)), text.size(), 0, 0,
                               data, scratch_->context);
    pcre2_match_data_free(data);

    result.error_code = rc;
    result.callouts = scratch_->budget.used;
    if (rc >= 0)
    {
        // 0 = ovector 放不下全部捕获，但确实命中了。
        result.outcome = RegexOutcome::Matched;
        return result;
    }

    result.error_message = pcre2_error_text(rc);
    switch (rc)
    {
        case PCRE2_ERROR_NOMATCH:
            // 只有这一种负值才是普通的不匹配，其余负值一律不能被吞成 unmatched。
            result.outcome = RegexOutcome::NoMatch;
            result.error_message.clear();
            break;
        case PCRE2_ERROR_CALLOUT:
        case PCRE2_ERROR_MATCHLIMIT:
        case PCRE2_ERROR_DEPTHLIMIT:
        case PCRE2_ERROR_HEAPLIMIT:
            result.outcome = RegexOutcome::LimitExceeded;
            break;
        default:
            result.outcome = RegexOutcome::EngineError;
            break;
    }
    return result;
}

}  // namespace kforge::classifier
