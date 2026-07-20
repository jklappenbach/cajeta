//
// Tasks.sleepMillis — the Cajeta surface over __cajeta_fiber_sleep_nanos
// (cooperative fiber sleep; main-thread callers cond_timedwait). Pinned
// wall-clock-style like TimerTests: must actually wait, must not overrun
// grossly. First consumer: dev.cajeta.gossip's bounded ack-drain
// (withTimeout cannot interrupt a channel park, so probe waits poll with
// a fiber sleep).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <chrono>
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Timed(const std::string& src, int64_t& elapsedMs) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto t0 = std::chrono::steady_clock::now();
    int32_t result = fn();
    elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return result;
}

} // namespace

TEST(TasksSleepTests, sleepMillisWaitsInFiber) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> {\n"
        "            Tasks.sleepMillis(50);\n"
        "            return 1;\n"
        "        };\n"
        "        return Tasks.runBlocking<int32>(body);\n"
        "    }\n"
        "}\n";
    int64_t elapsedMs = 0;
    EXPECT_EQ(runI32Timed(src, elapsedMs), 1);
    EXPECT_GE(elapsedMs, 40);      // actually waited (10ms jitter slack)
    EXPECT_LE(elapsedMs, 5000);    // and didn't wait absurdly long
}

TEST(TasksSleepTests, nonPositiveReturnsImmediately) {
    auto src =
        "package test;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Tasks.sleepMillis(0);\n"
        "        Tasks.sleepMillis(-5);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    int64_t elapsedMs = 0;
    EXPECT_EQ(runI32Timed(src, elapsedMs), 1);
}
