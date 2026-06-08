//
// R9.3 — cajeta.concurrent.Tasks.withTimeout. Exercises the user-facing
// timeout wrapper end-to-end: spawn → taskDonePointer → taskWaitTimeout
// (R9.1) → Duration deadline (R9.2) → Optional<R> return.
//
// v1 takes a pre-spawned Task<R> rather than a lambda — see the class
// doc in Tasks.cajeta for the rationale (spawn-of-lambda hasn't landed
// yet). v1 also reports timeout without cancelling — the body is always
// awaited to completion; the Optional just signals whether done flipped
// before the deadline.
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

// Body finishes immediately, deadline is 60s in the future. withTimeout
// returns Optional.present(value); `run` encodes present-ness as
// 1000+value (so the empty-vs-present-vs-wrong-value distinction is
// observable from a single int32 return).
// Probe: `heap Optional<int32>(false, null)` for primitive R — does the
// compiler accept `null` in the T-value position when T is primitive?
TEST(WithTimeoutTests, probeOptionalPrimitiveNull) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> o = heap Optional<int32>(false, null);\n"
        "        return o.orElse(99);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

TEST(WithTimeoutTests, fastTaskReturnsPresentValue) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Duration d = Duration.ofSeconds(60);\n"
        "        Task<int32> t = spawn compute();\n"
        "        Optional<int32> r = Tasks.withTimeoutInt32(d, t);\n"
        "        if (r.isPresent()) {\n"
        "            return 1000 + r.get();\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1007);
}

// Local-module regression for #47: a user-declared templated method
// whose method-level T is reachable only through a Task<R> formal.
// Before the fix Task<R> didn't expose its element type via the
// standard CajetaClass typeArguments interface, so the unifier's
// recursive walk into class-template formals never bound R and the
// MCE silently lowered to null. Self-contained (no stdlib types),
// pinning that the fix works in the local-module case too.
TEST(WithTimeoutTests, localTemplatedTaskParamInferred) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 9; }\n"
        "    public static int32 unwrapSize<R>(Task<R> t) {\n"
        "        return 7;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Task<int32> t = spawn compute();\n"
        "        return D.unwrapSize(t);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Generic form — Tasks.withTimeout<R> with R inferred from Task<int32>.
// Pins that a templated stdlib method whose method-level T is reachable
// only through a Task<R> formal binds correctly via implicit inference.
// Task is the compiler-synthesized class; its element-type slot has to
// be exposed via the standard CajetaClass typeArguments so the
// unifier's recursive walk fires (task #47). Before the fix the call
// dispatched against the wrong vtable and SIGSEGV'd at run.
TEST(WithTimeoutTests, genericFormInt32) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 compute() { return 9; }\n"
        "    public static int32 run() {\n"
        "        Duration d = Duration.ofSeconds(60);\n"
        "        Task<int32> t = spawn compute();\n"
        "        Optional<int32> r = Tasks.withTimeout(d, t);\n"
        "        if (r.isPresent()) {\n"
        "            return 1000 + r.get();\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1009);
}

// Body burns enough CPU to exceed a 1-nanosecond deadline. Because v1
// awaits the body regardless, withTimeout still waits for the loop to
// finish — but reports Optional.empty since the deadline expired
// during the wait. Verifies the empty branch + the discarded body
// value (slowCompute would return 5_000_000 if read).
TEST(WithTimeoutTests, slowTaskReportsEmptyOnExpiredDeadline) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.time.Duration;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class D {\n"
        "    public static async int32 slowCompute() {\n"
        "        int32 acc = 0;\n"
        "        for (int32 i = 0; i < 5000000; i = i + 1) {\n"
        "            acc = acc + 1;\n"
        "        }\n"
        "        return acc;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Duration d = Duration.ofNanos(1);\n"
        "        Task<int32> t = spawn slowCompute();\n"
        "        Optional<int32> r = Tasks.withTimeoutInt32(d, t);\n"
        "        if (r.isPresent()) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}
