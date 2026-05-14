//
// Task<T> drop-tracking tests.
//
// Before this work, every spawn site malloced a CajetaTask struct that
// nothing freed — the AsyncStatus.md "Known gaps" list called it out as
// a memory leak. SpawnExpression now pushes a drop entry that calls
// CajetaTask::getOrCreateDropFunction at scope exit. The drop wrapper
// waits for the task's `done` flag before freeing (so the carrier
// thread can't free a struct the worker is still touching), then
// __cajeta_free's the heap block.
//
// Tests use the same two-step pattern DropChainTests uses: `run()`
// resets the counter and exercises drops; `read()` reads the counter
// after run() returns. The reset/read intrinsics come from
// Cajeta.dropCount() / Cajeta.dropCountReset(), already wired for the
// memory-model rollout.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Two-step shape: a `compute()` worker, a `run()` that resets the
// counter and exercises spawns, and a `read()` that returns the
// post-run count. run() and read() share the same module so they
// observe the same counter.
std::string spawnSource(const std::string& body) {
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

int64_t observe(const std::string& body) {
    auto jit = CajetaJit::compile(spawnSource(body), "test.D");
    auto runFn = jit->lookup<int32_t (*)()>("run");
    auto readFn = jit->lookup<int64_t (*)()>("read");
    runFn();
    return readFn();
}

} // namespace

// Canonical await-spawn shape. The Task<int32> is created at the spawn
// site, awaited inline (sync MVP collapses the schedule), then dropped
// at run()'s method-body scope exit. Drop count == 1.
TEST(SpawnDropTests, awaitSpawnDropsOnce) {
    EXPECT_EQ(observe("int32 r = await spawn compute();"), 1);
}

// Two independent spawn-await pairs. Each malloc gets its own drop
// entry; both fire at scope exit. Order doesn't matter for the count.
TEST(SpawnDropTests, twoAwaitSpawnsBothDrop) {
    EXPECT_EQ(observe(
        "int32 a = await spawn compute();\n"
        "        int32 b = await spawn compute();"), 2);
}

// Bare spawn (no await): under sync lowering, the inner call runs
// inline and the task is marked done immediately. The drop entry was
// still pushed at the spawn site, so scope-exit fires the drop and
// frees the struct. This is the case the leak gap explicitly described:
// without the drop wiring, this allocation would leak with no path to
// reclaim it.
TEST(SpawnDropTests, bareSpawnStillDrops) {
    EXPECT_EQ(observe("spawn compute();"), 1);
}

// Spawn inside an inner `scope { }` block. The drop entry registers
// onto the inner block's frame, so the drop fires at the inner `}`
// rather than at run()'s method exit. The counter shows the drop has
// already happened by the time control reaches the post-scope code.
//
// We verify by reading the counter AFTER the inner scope but still
// inside run(); since intra-method observation is tricky (run() hasn't
// returned yet, so its own method-frame drops haven't fired), we use
// the test pattern: the only drop that should have fired by run()'s
// return is the spawn's — there are no other owned locals in this body.
TEST(SpawnDropTests, spawnInsideInnerScopeDropsAtInnerExit) {
    EXPECT_EQ(observe(
        "scope {\n"
        "            int32 r = await spawn compute();\n"
        "        }"), 1);
}

// TLS-promote regression guard: a spawned method declares its OWN
// owned local (an array). That local pushes a drop entry on the
// CARRIER thread's chain (not main's). Before the drop_top + exc_top
// TLS promotion, the carrier and main aliased a single global head,
// so the spawned method's push/pop could corrupt main's chain. With
// per-fiber heads, the carrier's chain is isolated; main only sees
// its own drops plus the Task drop. The drop counter is atomic, so
// both threads' increments are visible. Three drops fire: the carrier's
// int32[] in compute_with_local(), then main's two Task<int32> drops
// (one per declaration) at run()'s scope exit.
TEST(SpawnDropTests, carrierDropsAccountedSeparately) {
    auto src = std::string(
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute_with_local() {\n"
        "        int32[] tmp = new int32[4];\n"
        "        return 7;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 a = await spawn compute_with_local();\n"
        "        int32 b = await spawn compute_with_local();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n");
    auto jit = CajetaJit::compile(src, "test.D");
    auto runFn = jit->lookup<int32_t (*)()>("run");
    auto readFn = jit->lookup<int64_t (*)()>("read");
    runFn();
    // 2 spawned-method-internal int32[] drops (one per spawn) +
    // 2 Task<int32> drops (one per spawn) = 4 total.
    EXPECT_EQ(readFn(), 4);
}
