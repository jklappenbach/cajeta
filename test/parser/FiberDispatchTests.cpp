//
// FiberDispatchTests.cpp — the scheduler must dispatch and complete EVERY
// fiber it is handed, across repeated park/resume cycles.
//
// The bug this guards against loses a fiber silently. On Windows the ucontext
// API is emulated over Win32 Fibers, and three of its calls could fail without
// being checked — `CreateFiber`, `ConvertThreadToFiber`, and a `SwitchToFiber`
// whose target handle was NULL. The last one is the trap: the emulated
// `swapcontext` simply returned when the target had no handle, so the caller
// believed it had suspended (or dispatched) while control just fell through.
// The carrier then saw a fiber that was neither DONE nor runnable, dropped it,
// and moved on. Every awaiter blocked forever with all carriers idle — a hang
// with no diagnostic anywhere.
//
// A dropped fiber is not platform-specific in principle, so the assertion here
// is platform-neutral: spawn many fibers, make each park and resume several
// times, and require every one of them to complete. Parking repeatedly is the
// point — it is what the non-Linux socket path does for every readiness wait
// (`Reactor.pollPark` -> `Tasks.sleepMillis` -> the timer wheel), so this drives
// the same park/publish machinery that was losing fibers.
//
// If a fiber IS dropped the await never returns, so this test hangs rather than
// fails. That is intentional and is what the suite's per-test timeout is for:
// the sweep reports it as a timeout, which is a signal, whereas the silent drop
// it replaces produced nothing at all.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// 16 rounds x 4 concurrent fibers, each parking 3 times on the timer wheel.
// Kept modest so the test stays well inside the per-test budget while still
// running 64 fibers through 192 park/resume cycles.
constexpr int32_t kExpectedCompletions = 64;

} // namespace

TEST(FiberDispatchTests, everySpawnedFiberCompletesAcrossRepeatedParks) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 worker() {\n"
        "        int32 i = 0;\n"
        "        while (i < 3) {\n"
        "            Tasks.sleepMillis(2);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 total = 0;\n"
        "        int32 k = 0;\n"
        "        while (k < 16) {\n"
        "            Task<int32> a = spawn worker();\n"
        "            Task<int32> b = spawn worker();\n"
        "            Task<int32> c = spawn worker();\n"
        "            Task<int32> d = spawn worker();\n"
        "            total = total + await a + await b + await c + await d;\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return total;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");

    // Each worker contributes exactly 1. A short total would mean a fiber
    // returned the wrong value; a dropped fiber never returns at all.
    EXPECT_EQ(fn(), kExpectedCompletions);
}
