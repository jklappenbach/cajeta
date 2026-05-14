//
// Task<T> as a user-typeable template.
//
// Before this rollout, `Task<T>` was only synthesized at spawn sites
// (CajetaTask::getOrCreate fired from inside SpawnExpression::resolveTypes).
// The user couldn't write `Task<int32>` as a local-variable type, so the
// canonical "store two task handles, then await each" pattern had no
// expression in the source language. See AsyncStatus.md § Plan: Task<T>
// as user-typeable template for the punch list and ownership-transfer
// rationale.
//
// What this exercises:
//   - CajetaType::fromContext recognizes `Task<T>` and routes through
//     CajetaTask::getOrCreate instead of failing to resolve the bare
//     name `Task`.
//   - AwaitExpression accepts an identifier whose resolvedType is a
//     CajetaTask (defensive re-resolve at codegen time for the
//     pre-pass-runs-before-the-local-is-in-scope case).
//   - loadIfLValue passes a SpawnExpression result through unchanged
//     (the malloc'd ptr IS the local's value; loading the struct
//     through it would copy 24 bytes into an 8-byte slot).
//   - Ownership-transfer (option a): SpawnExpression's drop entry is
//     marked inactive when the result is bound to a named local, so
//     the local's class-instance drop entry becomes the canonical
//     owner. Without this the same Task is freed twice and the second
//     free walks freed memory.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Same two-step pattern DropChainTests / SpawnDropTests use: run()
// exercises the spawns + resets the drop counter, read() returns the
// post-run count. Lets us assert on drop firings from outside the
// JIT'd code (where the counter isn't readable mid-method).
std::string dropObserveSource(const std::string& body) {
    return std::string(
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        ") + body + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
}

int64_t observeDrops(const std::string& body) {
    auto jit = CajetaJit::compile(dropObserveSource(body), "test.D");
    auto runFn = jit->lookup<int32_t (*)()>("run");
    auto readFn = jit->lookup<int64_t (*)()>("read");
    runFn();
    return readFn();
}

} // namespace

// The minimum: declare a Task<int32> local, await it, return the value.
// Without the typing changes this fails at type-resolution time
// (`Task` is an unknown class) or at codegen (await on the local
// doesn't recognize CajetaTask). With them, it produces the inner
// compute()'s 7.
TEST(TaskTypingTests, declareAwaitOneTask) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn compute();\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The canonical use case the AsyncStatus.md gap explicitly named:
// store two task handles, then await each. Lets future tests exercise
// fiber-on-fiber lock contention (when there's something to contend
// for); for v1 with sync lowering it just proves both handles can
// outlive the call site of either spawn.
TEST(TaskTypingTests, twoHandlesStoredThenAwaited) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 left()  { return 10; }\n"
        "    public static async int32 right() { return 32; }\n"
        "    public static int32 run() {\n"
        "        Task<int32> a = spawn left();\n"
        "        Task<int32> b = spawn right();\n"
        "        int32 va = await a;\n"
        "        int32 vb = await b;\n"
        "        return va + vb;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Different element types resolve as distinct Task<T> instances —
// Task<int32> and Task<int64> share the wrapper class machinery but
// each gets its own CajetaTask with the right value-field width.
// Catches any place the int32-specialization leaks across to int64.
TEST(TaskTypingTests, differentTaskElementTypes) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 small() { return 100; }\n"
        "    public static async int64 large() { return 100000000000; }\n"
        "    public static int32 run() {\n"
        "        Task<int32> a = spawn small();\n"
        "        Task<int64> b = spawn large();\n"
        "        int32 va = await a;\n"
        "        int64 vb = await b;\n"
        "        return va + (int32)(vb - 99999999900);\n"
        "    }\n"
        "}\n";
    // small() = 100; large() = 1e11. (1e11 - 99999999900) = 100. Sum = 200.
    EXPECT_EQ(runI32(src), 200);
}

// Drop-count assertion: two declared-then-awaited tasks drop exactly
// twice, not four times. Without the ownership-transfer wiring,
// SpawnExpression's transient drop entry AND LocalVariableDeclaration's
// class-instance drop entry would both fire — 4 drops, plus the
// second drop's free walking freed memory. With the option-a
// inactivation, the spawn entry is dormant and only the local's drop
// fires. One drop per local; two tasks => two drops.
TEST(TaskTypingTests, declaredTasksDropOncePerLocal) {
    EXPECT_EQ(observeDrops(
        "Task<int32> a = spawn compute();\n"
        "        Task<int32> b = spawn compute();\n"
        "        int32 va = await a;\n"
        "        int32 vb = await b;"), 2);
}

// Variable shadowing / declaration order isn't unique to Task<T>, but
// covers the case where a Task local outlives its `await` call —
// reads after await on the same local should still work (returns the
// already-completed value's cache; sync MVP collapses).
//
// More importantly: after `await t`, the drop chain still owns t.
// At scope exit t is dropped exactly once. Counter == 1.
TEST(TaskTypingTests, awaitedTaskStillDropsOnceAtScopeExit) {
    EXPECT_EQ(observeDrops(
        "Task<int32> t = spawn compute();\n"
        "        int32 first = await t;"), 1);
}
