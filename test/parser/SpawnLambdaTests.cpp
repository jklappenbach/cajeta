//
// spawn-of-lambda — SpawnExpression accepting a function-typed local /
// parameter / capture as the inner "method" and indirect-dispatching
// through the L3-3 closure record. Unblocks the lambda-form withTimeout
// shape documented in docs/specification/concurrent/Concurrency.md § withTimeout.
//
// v1 supports the heap-ownership / `(P) -> #R` return ABI and primitive
// returns; the sret value-return shape is rejected at compile time
// (CAJETA_ERROR_ASYNC_SPAWN_LAMBDA_SRET) because the result slot would
// need to outlive the worker's trampoline frame.
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

// Smallest probe: function-typed local declared, then spawned via the
// same identifier the lambda was bound to. The closure has no captures
// (free-of-locals body) so the captures arg threaded by the trampoline
// is the empty bag the L1 ABI emits.
TEST(SpawnLambdaTests, bareNoCapturesPrimitive) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        () -> int32 body = () -> { return 41; };\n"
        "        Task<int32> t = spawn body();\n"
        "        return await t + 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// One user-arg threaded through the ctx struct alongside the closure ptr.
// Confirms ctx layout {task, closure, user_arg0} and the trampoline
// loads ctx[2] for the lambda's first parameter.
TEST(SpawnLambdaTests, oneArgPrimitive) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        (int32) -> int32 doubler = (int32 x) -> { return x + x; };\n"
        "        Task<int32> t = spawn doubler(21);\n"
        "        return await t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Closure receives the lambda via a function-typed method parameter, then
// spawns it. This is the actual driver shape for withTimeout / withDeadline
// — the stdlib utility takes a callable and spawns it on the user's behalf.
TEST(SpawnLambdaTests, spawnFromFnTypeParameter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 runIt(() -> int32 body) {\n"
        "        Task<int32> t = spawn body();\n"
        "        return await t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        () -> int32 fortyTwo = () -> { return 42; };\n"
        "        return D.runIt(fortyTwo);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
