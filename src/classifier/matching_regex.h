#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "util/error.h"

namespace kforge::classifier {

// ============================================================
// 匹配引擎适配层：正则引擎收敛在这一个模块里，核心（type_matching）不直接碰它的头/API。
//
// 方言与字节语义（API/README 必须保持一致的说法）：
//   * 语法是 PCRE2，不是 ECMAScript/JavaScript —— 本模块不做语法翻译，也不假装等价。
//     支持 lookaround、反向引用等 PCRE2 能力；不做禁词过滤，也不自研语法校验器，
//     合法性完全由 PCRE2 编译结果说了算。
//   * 8-bit 库、按字节求值：模式与输入都是原始字节串（显式长度，允许内嵌 NUL），
//     UTF-8 文本只是恰好可用；不启用 UTF/UCP（PCRE2_NEVER_UTF | PCRE2_NEVER_UCP），
//     所以 `.` 与字符类按单字节算，中文是多字节序列而不是一个字符。
//   * ^$ 是整段文本边界：PCRE2_DOLLAR_ENDONLY 且不开 multiline/dotall —— `$` 只匹配
//     主题末尾，不会在结尾换行之前提前成功；用户要子串匹配就自己写锚。
//   * 换行约定 ANYCRLF：点号不匹配 CR/LF。\R 沿用 PCRE2 自身的 BSR 默认值，本模块不覆盖。
//   * case_sensitive=false 走 PCRE2_CASELESS，只做 ASCII 大小写折叠；不承诺 Unicode 折叠。
//   * 不启用 JIT，也不设置任何 JIT 相关选项。
//
// 工作量限制（不是墙钟超时，也不是「所有内部指令」的计数）：
//   * 编译时打开 PCRE2_AUTO_CALLOUT；一次 RegexSession 的 callout 计数在所有规则、
//     所有起点上共享，达到上限后回调返回负值中止整次匹配 —— 是显式的超限结果，
//     不会被当成普通的不匹配。PCRE2 自己的优化会跳过一部分 callout，所以这个计数是
//     工作量的下界估计，不是精确的指令数。
//   * 解释器另外设 match/depth/heap 限额与编译期括号嵌套上限；限额触发与引擎报错是
//     两种可区分的状态，调用方不能把它们吞成 unmatched。
//   * 这些限额都是本文件里的常量，配置里没有让用户调大的开关。
// ============================================================

/// 一次 evaluate 允许消耗的 callout 数（跨全部规则与全部起点累计）。
inline constexpr std::uint64_t kMaxCalloutsPerEvaluation = 100000;
/// PCRE2 解释器自身的限额：内部匹配函数调用次数。
inline constexpr std::uint32_t kRegexMatchLimit = 100000;
/// 回溯深度上限（PCRE2 10.x 的解释器用堆帧，深度不是线程栈深度）。
inline constexpr std::uint32_t kRegexDepthLimit = 256;
/// 单次匹配可用的堆上限，单位 KiB。
inline constexpr std::uint32_t kRegexHeapLimitKib = 1024;
/// 编译期的括号嵌套上限。
inline constexpr std::uint32_t kRegexParensNestLimit = 256;

enum class RegexOutcome
{
    Matched,        // 命中
    NoMatch,        // 普通不匹配（只有 PCRE2_ERROR_NOMATCH 会走到这里）
    LimitExceeded,  // callout 预算或解释器限额耗尽：整次匹配被中止
    EngineError,    // 引擎报错（内存、参数、内部错误）：不是不匹配
};

struct RegexMatchResult
{
    RegexOutcome outcome = RegexOutcome::NoMatch;
    /// PCRE2 的负错误码（0 = 没有错误），只在 LimitExceeded / EngineError 时有意义。
    int error_code = 0;
    /// 该次搜索结束时，本会话累计消耗的 callout 数（诊断用，不是精确指令数）。
    std::uint64_t callouts = 0;
    /// 引擎给的人读错误文本；普通不匹配时为空。
    std::string error_message;
};

/// 一条已编译模式：不可变、RAII 释放、可被多个线程同时使用
/// （每次 search 用调用方自己的 RegexSession 与 match data，没有共享可变匹配状态）。
class RegexProgram
{
public:
    RegexProgram();
    ~RegexProgram();
    RegexProgram(RegexProgram&&) noexcept;
    RegexProgram& operator=(RegexProgram&&) noexcept;
    RegexProgram(const RegexProgram&) = delete;
    RegexProgram& operator=(const RegexProgram&) = delete;

    /// 编译一条模式。where 是给人看的定位（要带条目 id），错误消息里会带上它和出错偏移。
    static util::Result<RegexProgram> compile(std::string_view pattern, bool case_sensitive,
                                              const std::string& where);

    bool valid() const;

private:
    friend class RegexSession;
    struct Compiled;
    std::unique_ptr<const Compiled> compiled_;
};

/// 一次 evaluate 的匹配会话：自己的 match context 与自己的 callout 预算。
/// 会话内的计数跨多次 search 累计；不同会话之间不共享任何可变状态，可以并发使用。
class RegexSession
{
public:
    RegexSession();
    ~RegexSession();
    RegexSession(const RegexSession&) = delete;
    RegexSession& operator=(const RegexSession&) = delete;
    RegexSession(RegexSession&&) = delete;
    RegexSession& operator=(RegexSession&&) = delete;

    /// match context 与限额都设置成功才可用；不可用时 search 返回 EngineError。
    bool valid() const;

    /// regex_search 语义：不写锚就是从任意起点开始找。text 用显式长度，允许内嵌 NUL。
    RegexMatchResult search(const RegexProgram& program, std::string_view text);

private:
    struct Scratch;
    std::unique_ptr<Scratch> scratch_;
};

}  // namespace kforge::classifier
