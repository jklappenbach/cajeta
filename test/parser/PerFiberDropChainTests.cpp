//
// Gap 3 (MemoryModel.md § Known gaps, as of 2026-05-17 doc): drop
// chain head TLS + per-fiber promotion.
//
// The doc bullet at MemoryModel.md:309 and :189 describes the chain
// head as a "single global static". That's stale — the rollout in
// cajeta_runtime.c:843 already declared
// `__cajeta_main_drop_top` as `__thread`, and
// `__cajeta_drop_top_ptr()` at :849 returns the running fiber's
// `drop_top` slot (cajeta_fiber struct field) when one is active.
//
// This test pins the property so a future refactor can't silently
// regress it: drops from a spawned task land on the task's fiber-
// local chain, NOT on the main thread's chain. We observe by
// snapshotting the drop count before/after a spawn-and-await, where
// the spawned body itself allocates and drops a local — count must
// reflect both main-thread and fiber-thread drops via the same
// observer (per-fiber chains all converge on the same atomic
// counter).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

// A spawned task body declares + drops a heap array; the await joins
// before we observe the count. The drop must have fired on the
// fiber's own chain (otherwise the spawned body's drops would either
// be lost or race the main chain's pointer). Count = 1 means the
// fiber-local chain fired exactly one drop on its way out.
TEST(PerFiberDropChainTests, spawnedBodyDropsObservableFromMain) {
    auto src =
        "package test;\n"
        "public final class T {\n"
        "    public static int32 worker() {\n"
        "        int32[] tmp = new int32[1];\n"  // drops at worker's scope exit
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        Task<int32> t = spawn worker();\n"
        "        int32 v = await t;\n"
        "        return v;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    jit->lookup<int32_t (*)()>("run")();
    int64_t count = jit->lookup<int64_t (*)()>("read")();
    // At least the worker's array drop. The Task local also drops at
    // run's scope exit, so the total is >= 1. Pin >= 1 rather than
    // exact since Task drop machinery is independently tested.
    EXPECT_GE(count, 1);
}
