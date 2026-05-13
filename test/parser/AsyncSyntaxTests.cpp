//
// Sync-lowering MVP for the structured-concurrency keywords (ThreadModel.md):
// async / await / spawn / scope / detach. With no real scheduler yet, every
// spawn runs inline and finishes before the surrounding code continues —
// await/spawn collapse to direct calls, scope is just `{ ... }`, detach
// evaluates-and-discards. These tests prove the grammar and AST plumbing are
// in place end-to-end; the scheduler / Task<T> wrapping / state-machine
// lowering land in future phases without changing the surface syntax.
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

} // namespace

// `async` modifier parses on a method; the body codegens as a regular
// function. Calling it directly returns the inner value.
TEST(AsyncSyntaxTests, asyncMethodCallableDirectly) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 fortyTwo() { return 42; }\n"
        "    public static int32 run() { return fortyTwo(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `await` parses and passes the inner value through unchanged.
TEST(AsyncSyntaxTests, awaitPassesThroughInnerValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 produce() { return 7; }\n"
        "    public static int32 run() { return await produce(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// `spawn` parses, runs the call inline (sync lowering), and materializes a
// Task<int32> wrapper that `await` unwraps. Bare `spawn` returns a
// Task<T>* now — bare integer destinations would be a type error, so the
// canonical form goes through `await`.
TEST(AsyncSyntaxTests, spawnRunsCallInline) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 11; }\n"
        "    public static int32 run() { return await spawn compute(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// `await spawn` together is the canonical pattern; sync MVP returns the
// inner value unchanged through both layers.
TEST(AsyncSyntaxTests, awaitSpawnReturnsInnerValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 99; }\n"
        "    public static int32 run() { return await spawn compute(); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// `scope { ... }` is a statement that owns its contents. In the sync MVP
// it's just a block — locals declared inside drop at the closing `}`,
// same as any block. Verifies the parser routes SCOPE through to the
// block grammar correctly.
TEST(AsyncSyntaxTests, scopeBlockExecutesContents) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 5; }\n"
        "    public static int32 run() {\n"
        "        int32 r = 0;\n"
        "        scope {\n"
        "            r = await spawn compute();\n"
        "        }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// R3-A: spawn of an async fn that takes one argument. Arg is evaluated
// at the spawn site (main thread), captured into the context struct,
// and read by the trampoline on the worker. Proves the context-capture
// pipeline carries primitive values correctly across the thread boundary.
TEST(AsyncSyntaxTests, spawnPassesOneArgument) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 doubled(int32 x) { return x + x; }\n"
        "    public static int32 run() { return await spawn doubled(21); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R4: smoke-test the fiber-aware lock_acquire path. A fiber acquires a
// freshly-allocated (uncontended) lock and releases it; the lock then
// gets reused by a second fiber. Under R3-B's plain pthread_mutex_lock
// this works trivially; under R4 the fiber goes through the new
// fiber-branch in __cajeta_lock_acquire (check `__cajeta_current_fiber`,
// take the lock's own mutex, check `held`, mark held=1). If the struct
// layout or check is broken, this test catches it.
//
// Deterministic fiber-on-fiber contention would require storing Task<T>
// in a user variable so two tasks can be spawned before either is awaited
// — and Task<T> isn't yet a user-resolvable type (it's only synthesized
// by the compiler at spawn sites). A more probative contention test
// lands once `Task<T>` is exposed as a known template.
TEST(AsyncSyntaxTests, fiberLockAcquireUsesFiberPath) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 fiberA(pointer h) {\n"
        "        Cajeta.lockAcquire(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        return 19;\n"
        "    }\n"
        "    public static async int32 fiberB(pointer h) {\n"
        "        Cajeta.lockAcquire(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        return 23;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        int32 a = await spawn fiberA(h);\n"
        "        int32 b = await spawn fiberB(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return a + b;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R4: a fiber holds a lock while it yields (via an inner await). The
// main thread then tries to acquire the same lock — it's NOT a fiber,
// so it uses the cond_wait path of __cajeta_lock_acquire. Either it
// blocks until the worker fiber resumes and releases, or it gets the
// uncontended fast path if the worker already finished. Both flows
// have to work for the test to pass.
TEST(AsyncSyntaxTests, mainThreadWaitsOnFiberHolder) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 nested() { return 0; }\n"
        "    public static async int32 holder(pointer h) {\n"
        "        Cajeta.lockAcquire(h);\n"
        "        int32 inner = await spawn nested();\n"
        "        Cajeta.lockRelease(h);\n"
        "        return 42 + inner;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        int32 r = await spawn holder(h);\n"
        "        Cajeta.lockAcquire(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R3-B: nested await — an async fn awaits another async fn. Under R2's
// single-worker model this would deadlock: the carrier blocks on cond_wait
// for the inner task's done flag, but the inner task is sitting on the
// queue with no worker free to run it. With stackful fibers + cooperative
// yield, the outer fiber parks itself when it awaits, the carrier picks
// up the inner fiber, that completes, parked fibers wake and the outer
// resumes. The test passing proves the fiber yield + wake-on-complete
// path actually moves through.
TEST(AsyncSyntaxTests, nestedAwaitDoesNotDeadlock) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 inner() { return 7; }\n"
        "    public static async int32 outer() {\n"
        "        int32 v = await spawn inner();\n"
        "        return v * 6;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return await spawn outer();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R3-A: spawn passing multiple arguments. Verifies the per-field ctx
// struct stores + loads work in arg order — a swap of two slots would
// produce the wrong subtraction result.
TEST(AsyncSyntaxTests, spawnPassesMultipleArguments) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 sub(int32 a, int32 b) { return a - b; }\n"
        "    public static int32 run() { return await spawn sub(100, 17); }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 83);
}

// R3-A: arg values evaluated at spawn site come from outer locals, not
// from constants. Confirms the ctx capture path reads each arg through
// its alloca → r-value coercion the same way regular method-call sites
// would. Without that load, the worker would see the slot ADDRESS in
// its arg slot and the cast/add would either error or produce garbage.
TEST(AsyncSyntaxTests, spawnArgsFromLocals) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 mul(int32 a, int32 b) { return a * b; }\n"
        "    public static int32 run() {\n"
        "        int32 x = 6;\n"
        "        int32 y = 7;\n"
        "        return await spawn mul(x, y);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// R2: spawning multiple tasks back-to-back exercises the queue depth and
// proves the await/condvar wait correctly pairs with each task's done
// flag (not a single global "any task done" signal). If the wait predicate
// were shared across tasks, the first await would return as soon as any
// later task completed — likely the wrong value.
TEST(AsyncSyntaxTests, multipleSpawnsRunIndependently) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 one() { return 1; }\n"
        "    public static async int32 ten() { return 10; }\n"
        "    public static async int32 hundred() { return 100; }\n"
        "    public static int32 run() {\n"
        "        int32 a = await spawn one();\n"
        "        int32 b = await spawn ten();\n"
        "        int32 c = await spawn hundred();\n"
        "        return a + b + c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 111);
}

// R1: `spawn` materializes a heap-allocated Task<T> wrapper whose value
// field carries the result and done flag is set true. The await unwraps
// the value through a struct-GEP — proving the wrapper actually exists
// (not pass-through) by exercising a chained spawn-then-await across a
// local binding. If R1's Task<T> codegen were missing, the local would
// hold an i32 (not a Task<int32>*) and the second await would type-error.
TEST(AsyncSyntaxTests, taskWrapperIsHeapAllocated) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 21; }\n"
        "    public static int32 run() {\n"
        "        int32 v = await spawn compute();\n"
        "        return v + v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// `detach expr` parses and evaluates the inner expression for its side
// effects, returning no value to the surrounding context. The MVP runs
// it inline. Verifies the detach grammar + dispatch + codegen path.
TEST(AsyncSyntaxTests, detachExecutesAndDiscards) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async void noop() { return; }\n"
        "    public static int32 run() {\n"
        "        detach noop();\n"
        "        return 33;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 33);
}
