//
// Tests for the threading sync-primitive intrinsics — `Cajeta.lockNew`,
// `Cajeta.lockAcquire`, `Cajeta.lockRelease`, `Cajeta.lockTryAcquire`,
// `Cajeta.lockDestroy`. These are the low-level building blocks the
// future user-facing `Lock` class will wrap with RAII semantics once
// user-defined-drop-on-class infrastructure lands. See
// docs/specification/concurrent/Concurrency.md § Synchronization primitives.
//
// These tests exercise the intrinsics on a single thread — sufficient
// to verify the API behaves and the runtime helpers wire correctly.
// Multi-thread contention tests come once thread spawn lands.
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

// Smoke: create, acquire, release, destroy — no deadlock, no crash.
TEST(LockIntrinsicTests, acquireReleaseDestroyRoundTrip) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        Cajeta.lockAcquire(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// tryAcquire on an uncontended lock succeeds (returns 1).
TEST(LockIntrinsicTests, tryAcquireOnFreeLockSucceeds) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        int32 got = Cajeta.lockTryAcquire(h);\n"
        "        if (got == 1) Cajeta.lockRelease(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return got;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// tryAcquire on a lock already held by the calling thread fails (returns
// 0) — the underlying pthread_mutex is the default non-recursive flavour,
// so the same thread can't re-enter it.
TEST(LockIntrinsicTests, tryAcquireOnHeldLockFails) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        Cajeta.lockAcquire(h);\n"
        "        int32 secondTry = Cajeta.lockTryAcquire(h);\n"
        "        Cajeta.lockRelease(h);\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return secondTry;\n"  // expect 0 — already held
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Acquire-release pairs can repeat on the same lock without leaking.
TEST(LockIntrinsicTests, repeatedAcquireRelease) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer h = Cajeta.lockNew();\n"
        "        int32 total = 0;\n"
        "        for (int32 i = 0; i < 5; i++) {\n"
        "            Cajeta.lockAcquire(h);\n"
        "            total = total + 1;\n"
        "            Cajeta.lockRelease(h);\n"
        "        }\n"
        "        Cajeta.lockDestroy(h);\n"
        "        return total;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Two distinct locks coexist — independent handles, no shared state.
TEST(LockIntrinsicTests, twoIndependentLocks) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        pointer a = Cajeta.lockNew();\n"
        "        pointer b = Cajeta.lockNew();\n"
        "        Cajeta.lockAcquire(a);\n"
        "        int32 bFree = Cajeta.lockTryAcquire(b);\n"  // unrelated to a
        "        if (bFree == 1) Cajeta.lockRelease(b);\n"
        "        Cajeta.lockRelease(a);\n"
        "        Cajeta.lockDestroy(a);\n"
        "        Cajeta.lockDestroy(b);\n"
        "        return bFree;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
