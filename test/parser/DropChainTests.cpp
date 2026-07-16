//
// Session 3 — drop-chain verification tests.
//
// Owned heap allocations (today: CajetaArray locals) push DropEntry records
// onto the runtime drop chain; scope-exit pops fire `__cajeta_free_array`.
// Move-out via `#` flips the entry inactive so the drop doesn't double-free.
//
// We observe drop activity via the language intrinsics `Cajeta.dropCount()` /
// `Cajeta.dropCountReset()`, which the runtime exposes for test purposes.
//
// Limitation today: drops fire at method return, so we can't observe them
// from inside the same method. Instead each test JIT-compiles a worker that
// returns the post-drop count by reading it AFTER the inner scope exits via
// a try/finally pattern is unsupported. Pragmatic stand-in: have the worker
// declare its owners in a nested *function-shaped* construct... we don't have
// that either. So for now we verify the drop chain by reading the count
// right at the return site — which sees the count BEFORE the method's drops
// fire — and rely on subsequent reads to confirm post-return state.
//
// To make this observable: the test pipeline runs the JIT'd `run` once
// (which drops on return), then a second `read` function returns the
// accumulated count. Both functions are in the same module so they share the
// same runtime counter.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Build a source with two static methods in one class:
//   - `run()`  resets the counter, then runs the body (owners drop at return).
//   - `read()` returns the post-run drop count.
//
// We call `run()` first to exercise the drops, then `read()` to inspect.
std::string twoStepSource(const std::string& body) {
    return std::string(
        "package test;\n"
        "public final class D {\n"
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
    auto jit = CajetaJit::compile(twoStepSource(body), "test.D");
    auto runFn = jit->lookup<int32_t (*)()>("run");
    auto readFn = jit->lookup<int64_t (*)()>("read");
    runFn();
    return readFn();
}

} // namespace

// --- Normal-flow drops ------------------------------------------------------

TEST(DropChainTests, singleArrayDropsAtReturn) {
    EXPECT_EQ(observe("int32[] xs = heap int32[8];"), 1);
}

TEST(DropChainTests, twoArraysBothDrop) {
    EXPECT_EQ(observe(
        "int32[] a = heap int32[4];\n"
        "int32[] b = heap int32[2];"), 2);
}

TEST(DropChainTests, multipleArrayUseDoesntChangeDropCount) {
    EXPECT_EQ(observe(
        "int32[] xs = heap int32[5];\n"
        "xs[0] = 1;\n"
        "xs[1] = 2;"), 1);
}

// --- Move-out suppresses the original drop ---------------------------------

TEST(DropChainTests, moveOutSuppressesOriginalDrop) {
    EXPECT_EQ(observe(
        "int32[] xs = heap int32[5];\n"
        "int32[] ys #= xs;"), 1);
}

TEST(DropChainTests, moveOutOfOneOfTwo) {
    EXPECT_EQ(observe(
        "int32[] a = heap int32[3];\n"
        "int32[] b = heap int32[3];\n"
        "int32[] c #= a;"), 2);
}
