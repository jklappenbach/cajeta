//
// cajeta.concurrent.Semaphore (R7-E) — counting permit pool composed on
// Mutex<int32>. acquire()/release() adjust the count; acquire blocks (parks
// the fiber) until a permit is free; withPermit runs a closure holding a
// permit with try/finally release. Loaded from the embedded stdlib
// (runtime/src/cajeta/concurrent/Semaphore.cajeta).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string semTestSource(const std::string& dBody) {
    return std::string("package test;\n")
        + "import cajeta.concurrent.Semaphore;\n"
        + "public final class D {\n" + dBody + "}\n";
}

int32_t runI32(const std::string& dBody) {
    auto jit = CajetaJit::compile(semTestSource(dBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(SemaphoreTests, availablePermitsReflectsInitial) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Semaphore s = heap Semaphore(3);\n"
        "        return s.availablePermits();\n"
        "    }\n"
    ), 3);
}

TEST(SemaphoreTests, acquireDecrements) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Semaphore s = heap Semaphore(3);\n"
        "        s.acquire();\n"
        "        return s.availablePermits();\n"
        "    }\n"
    ), 2);
}

TEST(SemaphoreTests, acquireReleaseRestores) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Semaphore s = heap Semaphore(2);\n"
        "        s.acquire();\n"
        "        s.acquire();\n"
        "        s.release();\n"
        "        return s.availablePermits();\n"
        "    }\n"
    ), 1);
}

// withPermit acquires a permit for the closure and releases it after, so
// the count is balanced (net zero) once it returns.
TEST(SemaphoreTests, withPermitBalancesCount) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Semaphore s = heap Semaphore(2);\n"
        "        s.withPermit(() -> { int32 x = 0; });\n"
        "        return s.availablePermits();\n"
        "    }\n"
    ), 2);
}

// Blocking hand-off: a consumer fiber acquires on an empty semaphore
// (parks), a producer fiber releases (wakes it). Exercises the permit
// wait/notify via the underlying Mutex condvar.
TEST(SemaphoreTests, blockingAcquireWokenByRelease) {
    EXPECT_EQ(runI32(
        "    public static async int32 consumer(Semaphore s, int32[] out) {\n"
        "        s.acquire();\n"
        "        out[0] = 42;\n"
        "        return 0;\n"
        "    }\n"
        "    public static async int32 producer(Semaphore s) {\n"
        "        s.release();\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Semaphore s = heap Semaphore(0);\n"
        "        int32[] out = heap int32[1];\n"
        "        scope {\n"
        "            spawn consumer(s, out);\n"
        "            spawn producer(s);\n"
        "        }\n"
        "        return out[0];\n"
        "    }\n"
    ), 42);
}
