//
// cajeta.concurrent.FiberLocal<T> / FiberContext — ambient per-request state
// (docs/stdlib/FiberLocal.md). Fiber-keyed, scope-restored binding; the sound
// replacement for a thread-pool ThreadLocal. These tests load the real stdlib
// types from the embedded standard library and exercise the runtime intrinsics
// (__cajeta_fiber_local_push/pop/get/is_bound + fiber_context_*).
//
// T may be any type (the value is boxed in FiberLocalBox<T>); a small `Box`
// reference class carries the test payload (an int32) and doubles as a heap
// accumulator the lambdas write through.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile a single `test.D` source whose static `run() -> int32` body is
// supplied. `Box` is a package-private reference payload in the same file (v1
// FiberLocal<T> requires a reference T; Box carries an int32 for assertions).
int32_t runI32(const std::string& dBody) {
    std::string src =
        std::string("package test;\n")
        + "import cajeta.concurrent.FiberLocal;\n"
        + "import cajeta.concurrent.FiberContext;\n"
        + "import cajeta.error.Exception;\n"
        + "public final class D {\n" + dBody + "}\n"
        + "class Box {\n"
        + "    public int32 v;\n"
        + "    public Box(int32 v) { this.v = v; }\n"
        + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// T may be a PRIMITIVE — the value is boxed (FiberLocalBox<T>) so it round-trips
// through the per-fiber store and back out of get() as a real int32, not a
// pointer-bitcast garbage value.
TEST(FiberLocalTests, primitiveTypeArg) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<int32> s = heap FiberLocal<int32>();\n"
        "        Box acc = heap Box(0);\n"
        "        s.where(7, () -> { acc.v = s.get(); });\n"
        "        return acc.v;\n"
        "    }\n"
    ), 7);
}

// Isolation: does the multi-source Box helper itself work (no FiberLocal)?
TEST(FiberLocalTests, boxOnly) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Box b = heap Box(5);\n"
        "        return b.v;\n"
        "    }\n"
    ), 5);
}

// Isolation: just construct + drop a FiberLocal<Box>, no method call.
TEST(FiberLocalTests, justConstruct) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

// Construct a FiberLocal<Box>; nothing bound yet → isBound() is false.
TEST(FiberLocalTests, constructUnbound) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        return s.isBound() ? 0 : 1;\n"
        "    }\n"
    ), 1);
}

// orElse on an unbound key (no default) returns the fallback.
TEST(FiberLocalTests, orElseUnboundReturnsFallback) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        return s.orElse(heap Box(99)).v;\n"
        "    }\n"
    ), 99);
}

// where() binds for the closure extent; get() inside sees the bound value.
// (Local FiberLocal + heap accumulator captured by the lambda — the JIT test
// harness does not run static-field initializers.)
TEST(FiberLocalTests, whereBindsAndGet) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        Box acc = heap Box(0);\n"
        "        s.where(heap Box(7), () -> { acc.v = s.get().v; });\n"
        "        return acc.v;\n"
        "    }\n"
    ), 7);
}

// Nested where() shadows, then restores the outer binding on exit.
// Accumulate 7 (outer) + 42 (nested) + 7 (restored) = 56.
TEST(FiberLocalTests, nestedShadowAndRestore) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        Box acc = heap Box(0);\n"
        "        s.where(heap Box(7), () -> {\n"
        "            acc.v = acc.v + s.get().v;\n"
        "            s.where(heap Box(42), () -> { acc.v = acc.v + s.get().v; });\n"
        "            acc.v = acc.v + s.get().v;\n"
        "        });\n"
        "        return acc.v;\n"
        "    }\n"
    ), 56);
}

// After where() returns, the binding is gone (isBound() false again).
TEST(FiberLocalTests, restoredUnboundAfterWhere) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        s.where(heap Box(1), () -> { });\n"
        "        return s.isBound() ? 0 : 1;\n"
        "    }\n"
    ), 1);
}

// The binding is restored even when the body throws (finally on the unwind).
TEST(FiberLocalTests, restoreOnThrow) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        try {\n"
        "            s.where(heap Box(1), () -> { throw heap Exception(\"x\"); });\n"
        "        } catch (Exception e) { }\n"
        "        return s.isBound() ? 0 : 1;\n"
        "    }\n"
    ), 1);
}

// FiberContext.capture() + run() reinstalls the captured binding (exercises the
// capture / install / pop / free intrinsics end to end).
// Capture a context while a binding is in effect, then install it via run() and
// read the carried value. The ctx.run lambda lives in a helper method (not
// nested directly inside the where lambda) — lambda-inside-lambda capturing
// outer captures is a separate codegen limitation, unrelated to FiberLocal.
TEST(FiberLocalTests, fiberContextCaptureRun) {
    EXPECT_EQ(runI32(
        "    static void useCtx(FiberContext ctx, Box acc, FiberLocal<Box> s) {\n"
        "        ctx.run(() -> { acc.v = s.get().v; });\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        Box acc = heap Box(0);\n"
        "        s.where(heap Box(13), () -> { useCtx(FiberContext.capture(), acc, s); });\n"
        "        return acc.v;\n"
        "    }\n"
    ), 13);
}

// Layer 2 — inherit-on-spawn. A child fiber spawned (inside a scope) while a
// binding is in effect sees that binding via the snapshot __cajeta_task_run
// deep-copies from the spawner. The child reads s.get() and writes it to a
// shared Box the scope join makes visible. Returns 21 only if the child
// inherited the parent's binding (else get() throws on the unbound child).
TEST(FiberLocalTests, inheritOnSpawn) {
    EXPECT_EQ(runI32(
        "    public static async int32 child(FiberLocal<Box> s, Box out) {\n"
        "        out.v = s.get().v;\n"
        "        return 0;\n"
        "    }\n"
        "    public static void fanout(FiberLocal<Box> s, Box out) {\n"
        "        scope {\n"
        "            spawn child(s, out);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        Box out = heap Box(0);\n"
        "        s.where(heap Box(21), () -> { fanout(s, out); });\n"
        "        return out.v;\n"
        "    }\n"
    ), 21);
}

TEST(FiberLocalTests, captureOnly) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        FiberContext ctx = FiberContext.capture();\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}

TEST(FiberLocalTests, captureThenRunEmpty) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        FiberLocal<Box> s = heap FiberLocal<Box>();\n"
        "        FiberContext ctx = FiberContext.capture();\n"
        "        ctx.run(() -> { });\n"
        "        return 1;\n"
        "    }\n"
    ), 1);
}
