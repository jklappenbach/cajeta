//
// R8 Slice 1 — atomic primitives. The single-carrier cooperative scheduler
// means there's no real cross-thread contention yet, but the LLVM atomic
// instructions are still emitted and the semantic contract still holds.
// These tests exercise load/store correctness, fetch-and-add accumulation,
// and CAS success/failure paths so the operations don't regress once the
// multi-carrier pool (R8.3) starts actually running them concurrently.
//
// Atomicity itself can't be falsified on a single carrier — the
// `load atomic` / `atomicrmw` / `cmpxchg` IR is the contract the lower
// layers depend on. The multi-carrier work in R8.3 is what makes that
// contract observable; here we lock in functional correctness.
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

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(AtomicTests, i32LoadStoreRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(7);\n"
        "        a.store(42);\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(AtomicTests, i32FetchAddReturnsPriorValue) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(10);\n"
        "        int32 prior = a.fetchAdd(5);\n"  // returns 10, leaves 15
        "        return prior * 100 + a.load();\n"  // 1015
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1015);
}

TEST(AtomicTests, i32FetchAddAccumulatesAcrossCalls) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(0);\n"
        "        for (int32 i = 0; i < 100; i = i + 1) {\n"
        "            a.fetchAdd(1);\n"
        "        }\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

TEST(AtomicTests, i32CompareAndSetSuccess) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(7);\n"
        "        boolean ok = a.compareAndSet(7, 99);\n"
        "        if (!ok) return -1;\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

TEST(AtomicTests, i32CompareAndSetFailureLeavesCellUntouched) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(7);\n"
        "        boolean ok = a.compareAndSet(8, 99);\n"  // expected 8, actual 7 → no swap
        "        if (ok) return -1;\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

TEST(AtomicTests, i64LoadStoreRoundTrip) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 a = heap AtomicInt64(0L);\n"
        "        a.store(123456789012345L);\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{123456789012345LL});
}

TEST(AtomicTests, i64FetchAddAccumulates) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 a = heap AtomicInt64(1000000000000L);\n"
        "        a.fetchAdd(1L);\n"
        "        a.fetchAdd(1L);\n"
        "        a.fetchAdd(1L);\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{1000000000003LL});
}

TEST(AtomicTests, i64CompareAndSetSwapsLargeValues) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 a = heap AtomicInt64(9000000000000L);\n"
        "        boolean ok = a.compareAndSet(9000000000000L, 9000000000001L);\n"
        "        if (!ok) return -1L;\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{9000000000001LL});
}

// ----- R8.1b — fixed-ordering variants -------------------------------------
// Functional smoke for each named ordered method. Atomicity across carriers
// can't be tested on the single-carrier scheduler; what's exercised here is
// that the LLVM IR emitted by the fixed-ordering intrinsics is valid and
// produces the right value semantics under sequential execution.

TEST(AtomicTests, i32RelaxedFetchAddAccumulates) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(0);\n"
        "        for (int32 i = 0; i < 50; i = i + 1) {\n"
        "            a.fetchAddRelaxed(1);\n"
        "        }\n"
        "        return a.loadRelaxed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 50);
}

TEST(AtomicTests, i32ReleaseStoreAcquireLoadHandshake) {
    // The publish/subscribe pattern: writer stores with Release, reader
    // loads with Acquire. Same carrier here, so this just checks the
    // value path; the ordering pair is the contract the codegen pins.
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 flag = heap AtomicInt32(0);\n"
        "        flag.storeRelease(42);\n"
        "        return flag.loadAcquire();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

TEST(AtomicTests, i32AcquireCasSuccessAndFailure) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt32;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        AtomicInt32 a = heap AtomicInt32(7);\n"
        "        boolean ok1 = a.compareAndSetAcquire(7, 100);\n"
        "        boolean ok2 = a.compareAndSetAcquire(7, 999);\n"  // expected 7, actual 100 → false
        "        if (!ok1) return -1;\n"
        "        if (ok2) return -2;\n"
        "        return a.load();\n"  // 100
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

TEST(AtomicTests, i64ReleaseStoreAcquireLoadHandshake) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 seq = heap AtomicInt64(0L);\n"
        "        seq.storeRelease(123456789012345L);\n"
        "        return seq.loadAcquire();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{123456789012345LL});
}

TEST(AtomicTests, i64RelaxedFetchAddAccumulates) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 a = heap AtomicInt64(1000000000000L);\n"
        "        a.fetchAddRelaxed(1L);\n"
        "        a.fetchAddRelaxed(1L);\n"
        "        return a.loadRelaxed();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{1000000000002LL});
}

// benchmark-gap-sweep Unit 2 — the atomic-fetchadd bench hot loop: N fetchAdd(1)
// on a statically-typed AtomicInt64 local must sum to N. With AtomicInt64 `final`
// this call devirtualizes (no per-iteration vtable hash lookup + indirect call),
// the body inlines to `lock xadd`; this guards the result stays correct.
TEST(AtomicTests, i64FetchAddLoopSumsToN) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.AtomicInt64;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        AtomicInt64 a = heap AtomicInt64(0);\n"
        "        int32 n = 100000;\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            a.fetchAdd(1);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return a.load();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), int64_t{100000LL});
}
