// ============================================================
// 独立故障注入测试：PCRE2 编译 context 在 C++ 分配异常路径上的归属
//
// 目标缺陷（core review：src/classifier/matching_regex.cpp:84/104/109）：
// RegexProgram::compile 先创建裸的 pcre2_compile_context，再执行
// std::make_unique<Compiled>()；如果这次 C++ 分配抛 std::bad_alloc，栈展开时
// 没有任何对象负责释放 context —— 真正的 SDK 资源泄漏。
//
// 做法（全部是真实调用，不伪造任何返回值，也不改适配层实现）：
//   * 链接期用 GNU/LLD 的 --wrap= 浅包装两个公开 SDK 函数
//     （pcre2_compile_context_create_8 / pcre2_compile_context_free_8）：
//     __wrap_* 委托 __real_*（真实静态库），只用固定存储记录指针与计数。
//     包装里不分配、不抛、不加锁，也不复制任何正则算法。
//   * 本 exe 内替换普通 operator new/delete（malloc/free 实现）：只让「被武装的
//     下一次分配」抛 std::bad_alloc，pattern/where 都是提前准备好的 SSO 小字符串，
//     保证这一次异常就是 Compiled 对象那一次分配。
//   * 这个 exe 只收录本文件与 src/classifier/matching_regex.cpp（xmake.lua 的
//     test_matching_regex_raii），故障注入不会外溢到其它测试二进制。
//
// 当前实现（裸 context 无 RAII 所有者）在异常路径留下 1 个存活 context → 红；
// 适配层加上 RAII 所有者后，同一份测试不需要放宽就转绿。
// 另一条控制用例证明包装确实拦到了真实 SDK 调用（计数随真实编译变化）。
// ============================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// 静态链接：不定义 PCRE2_STATIC 时 pcre2.h 会把函数声明成 __declspec(dllimport)，
// 与 libpcre2-8.a 里的普通符号对不上（与 matching_regex.cpp 同样的理由）。
#define PCRE2_STATIC 1
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "classifier/matching_regex.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

namespace
{

/// 被武装后，本线程的下一次普通分配抛 std::bad_alloc；其余时刻完全走 malloc/free。
/// thread_local + 一次性：doctest 自己的分配、别的线程的分配都不受影响。
thread_local bool fail_next_allocation = false;

void arm_next_allocation_failure()
{
    fail_next_allocation = true;
}

}  // namespace

// 替换全局 operator new/delete 必须留在全局作用域（放进 namespace 就不是替换了）。
// 只在被武装的那一次抛异常，正常路径就是 malloc/free，且与配套的 delete 成对。
void* operator new(std::size_t size)
{
    if (fail_next_allocation)
    {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    // size 为 0 也要返回合法且互不相同的指针。
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr)
        throw std::bad_alloc();
    return memory;
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{

// ---------------- compile context 账本：固定存储，不分配、不抛 ----------------

/// 存活 compile context 的浅账本。只记录指针与计数，不参与匹配逻辑 ——
/// 它唯一的作用是让 --wrap 拦到的真实 SDK 调用可观察。
struct ContextLedger
{
    static constexpr int kCapacity = 16;

    int live = 0;
    int created = 0;
    int freed = 0;
    int stored = 0;
    pcre2_compile_context* pointers[kCapacity] = {};

    void on_created(pcre2_compile_context* context)
    {
        ++created;
        if (context == nullptr)
            return;
        ++live;
        if (stored < kCapacity)
            pointers[stored++] = context;
    }

    void on_freed(pcre2_compile_context* context)
    {
        ++freed;
        if (context == nullptr)
            return;
        --live;
        for (int index = 0; index < stored; ++index)
        {
            if (pointers[index] == context)
            {
                pointers[index] = pointers[--stored];
                break;
            }
        }
    }
};

ContextLedger& context_ledger()
{
    // 常量初始化：没有动态初始化，也就没有需要异常保护的构造。
    static ContextLedger ledger{};
    return ledger;
}

}  // namespace

// ---------------- 被 --wrap 拦到的真实 SDK 调用 ----------------
// --wrap=sym 让「对 sym 的引用」走 __wrap_sym，「对 __real_sym 的引用」走真实 sym。
// 两个包装只记一笔账，之后（或之前）就委托真实实现，本身不改变任何行为。

extern "C" pcre2_compile_context* __real_pcre2_compile_context_create_8(pcre2_general_context*);
extern "C" void __real_pcre2_compile_context_free_8(pcre2_compile_context*);

extern "C" pcre2_compile_context* __wrap_pcre2_compile_context_create_8(pcre2_general_context* general)
{
    pcre2_compile_context* context = __real_pcre2_compile_context_create_8(general);
    context_ledger().on_created(context);
    return context;
}

extern "C" void __wrap_pcre2_compile_context_free_8(pcre2_compile_context* context)
{
    context_ledger().on_freed(context);
    __real_pcre2_compile_context_free_8(context);
}

namespace
{

/// 把本 exe 里仍然存活的 context 交回真实 free（红的时候只剩这次注入留下的残留），
/// 后续用例从干净计数开始；走真实释放路径，所以计数仍然准确。
void release_leftover_contexts()
{
    auto& ledger = context_ledger();
    while (ledger.stored > 0)
        __wrap_pcre2_compile_context_free_8(ledger.pointers[0]);
}

}  // namespace

// ---------------- 用例 ----------------

TEST_CASE("a failing Compiled allocation must not leak the PCRE2 compile context")
{
    auto& ledger = context_ledger();
    REQUIRE(ledger.live == 0);  // 前置：本 exe 的用例串行跑，进来时账本必须是干净的

    // pattern 与 where 都提前备好：SSO 放得下，调用点不会再有别的堆分配，
    // 所以被武装的那一次分配只可能是 Compiled 对象。
    const std::string pattern = "^R[0-9?]*$";
    const std::string where = "raii-test rule";  // 14 字节，libstdc++ SSO 上限 15
    REQUIRE(pattern.size() <= 15);

    arm_next_allocation_failure();
    bool caught_bad_alloc = false;
    try
    {
        auto program = kforge::classifier::RegexProgram::compile(pattern, false, where);
        // 注入失败时不该走到这里：走到就说明这一次分配没有真的抛出去。
        (void)program;
    }
    catch (const std::bad_alloc&)
    {
        caught_bad_alloc = true;
    }

    CHECK(caught_bad_alloc);
    if (!caught_bad_alloc)
        MESSAGE("the armed allocation never threw: the fault window is not the Compiled allocation any more");
    MESSAGE("live compile contexts after the injected std::bad_alloc: ", ledger.live,
            " (created ", ledger.created, ", freed ", ledger.freed, ")");
    // 异常路径同样必须释放已经创建的 compile context：当前实现留 1 个，这里就是红。
    CHECK(ledger.live == 0);

    // 本用例自己把残留收干净，不让后面的用例继承这份泄漏。
    release_leftover_contexts();
    CHECK(ledger.live == 0);
}

TEST_CASE("real compiles go through the wrapped SDK calls and leave no context behind")
{
    auto& ledger = context_ledger();
    REQUIRE(ledger.live == 0);
    const int created_before = ledger.created;
    const int freed_before = ledger.freed;

    // 正常编译：包装确实拦到了真实 SDK 调用（计数随真实编译变化），编译成功后归零
    const std::string_view good = "^R[0-9?]*$";
    const std::string where = "control rule";  // 12 字节，SSO
    auto program = kforge::classifier::RegexProgram::compile(good, false, where);
    if (!program)
        MESSAGE("compile failed: ", program.error().format_message());
    REQUIRE(program.has_value());
    CHECK(program->valid());
    CHECK(ledger.created == created_before + 1);
    CHECK(ledger.freed == freed_before + 1);
    CHECK(ledger.live == 0);

    // 坏语法：还是真实 SDK，返回明确错误而不是抛异常，context 同样归零
    const std::string_view broken = "^R(";
    auto rejected = kforge::classifier::RegexProgram::compile(broken, false, where);
    REQUIRE_FALSE(rejected.has_value());
    MESSAGE("rejected: ", rejected.error().format_message());
    CHECK(ledger.created == created_before + 2);
    CHECK(ledger.live == 0);
}
