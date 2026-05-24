// Bisect task: heap Optional<T>[1] worker-write hang.
//
// findAnyParallelChain currently uses a `T[1] + boolean[1]` slot
// pair with the orchestrator constructing the Optional after
// scope-join. The original `Optional<T>[1]` shape (worker writes
// `heap Optional<T>(...)` directly into the slot) hangs in
// pthread_cond_wait — root cause unidentified at the time of the
// workaround.
//
// These tests reproduce the minimal shape so the hang surfaces
// at HEAD before bisection. Run with a wall-clock budget — gtest
// has no built-in timeout, so the build script wraps the binary
// in `timeout 20s` (see CMakeLists.txt for the discover hook).
// If a test hangs, exit 124 from the wrapper signals "still
// reproducing — workaround in findAnyParallelChain is load-bearing."

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

// Baseline 1 — non-generic class-typed array, worker writes from
// inside a spawn. If this hangs, the issue is unrelated to
// Optional / generics specifically.
TEST(OptionalArrayWorkerWriteTests, classArrayWorkerWrite) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box[] out = new Box[1];\n"
        "        scope {\n"
        "            spawn writer(out);\n"
        "        }\n"
        "        return out[0].v;\n"
        "    }\n"
        "    public static async int32 writer(Box[] out) {\n"
        "        out[0] = heap Box(42);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Baseline 2 — generic class-typed array (Optional<int32>[1]).
// The exact shape from findAnyParallelChain's pre-workaround design.
TEST(OptionalArrayWorkerWriteTests, optionalIntArrayWorkerWrite) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<int32>[] out = new Optional<int32>[1];\n"
        "        scope {\n"
        "            spawn writer(out);\n"
        "        }\n"
        "        if (out[0].isPresent()) {\n"
        "            return out[0].get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "    public static async int32 writer(Optional<int32>[] out) {\n"
        "        out[0] = heap Optional<int32>(true, 42);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Multi-worker contention shape — mirrors findAnyParallelChain's
// spawn loop. Several workers race to write into the same
// Optional<int32>[1] slot. If the hang is contention-driven (e.g.
// drop dispatch on the displaced slot value taking a lock), this
// is where it surfaces.
TEST(OptionalArrayWorkerWriteTests, optionalIntArrayMultiWorkerRace) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<int32>[] out = new Optional<int32>[1];\n"
        "        scope {\n"
        "            spawn writer(out, 1);\n"
        "            spawn writer(out, 2);\n"
        "            spawn writer(out, 3);\n"
        "            spawn writer(out, 4);\n"
        "        }\n"
        "        if (out[0].isPresent()) {\n"
        "            return out[0].get();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "    public static async int32 writer(Optional<int32>[] out, int32 v) {\n"
        "        out[0] = heap Optional<int32>(true, v);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    int32_t r = runI32(src);
    // Any worker's value is a legal outcome; we only care that we
    // got SOMETHING and didn't hang.
    EXPECT_TRUE(r >= 1 && r <= 4) << "got r=" << r;
}
