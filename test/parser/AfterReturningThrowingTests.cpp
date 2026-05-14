//
// A6: @AfterReturning + @AfterThrowing codegen.
//
// @AfterReturning fires only on the normal-return path (same hook
// points as A4's @After but a separate match kind). @AfterThrowing
// fires only from the catch arm of the try/catch wrapping the body
// (or the @Around-advice call, when @Around is also present).
//
// Tests use the drop-count side-channel: each advice declares
// `int32[] tmp = new int32[1]` whose scope-exit drop bumps the
// counter. Combined with try/catch in run() to recover from
// thrown values, the test can observe both whether advice fired
// AND that the throw still propagates after @AfterThrowing
// observes it.
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

// @AfterReturning on a method that returns normally. Advice fires
// once. The encoding is the same as the A4 tests: run() returns
// target value + 100 * dropCount. work() returns 7; count after
// is 1 (one advice fired); encoded result 107.
TEST(AfterReturningThrowingTests, afterReturningFiresOnNormalReturn) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterReturning(Audited.class)\n"
        "    public static void onReturn() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 107);
}

// @AfterReturning on a method that throws. Advice does NOT fire —
// only normal-return paths trigger it. run() catches the throw
// and returns the count, which should be 0 (no advice fired).
TEST(AfterReturningThrowingTests, afterReturningSkipsThrowPath) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterReturning(Audited.class)\n"
        "    public static void onReturn() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() {\n"
        "        throw 42;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        try {\n"
        "            int32 r = work();\n"
        "        } catch (Exception e) {\n"
        "        }\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// @AfterThrowing on a method that throws. Advice fires once,
// observing the throw. The throw still propagates — caught in
// run(). dropCount == 1 confirms the advice ran.
TEST(AfterReturningThrowingTests, afterThrowingFiresOnThrowPath) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterThrowing(Audited.class)\n"
        "    public static void onThrow() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() {\n"
        "        throw 42;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        try {\n"
        "            int32 r = work();\n"
        "        } catch (Exception e) {\n"
        "        }\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// @AfterThrowing on a method that returns normally. Advice does
// NOT fire. Confirms the catch path is the only trigger.
TEST(AfterReturningThrowingTests, afterThrowingSkipsNormalReturn) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterThrowing(Audited.class)\n"
        "    public static void onThrow() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);   // body's 7, no advice firings
}

// @AfterThrowing doesn't suppress the throw — the value still
// reaches the catch arm in run() with its original payload.
// Verifies the advice is observe-only: re-raise happens after.
TEST(AfterReturningThrowingTests, afterThrowingDoesNotSuppress) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterThrowing(Audited.class)\n"
        "    public static void onThrow() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() {\n"
        "        throw 42;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 caught = 0;\n"
        "        try {\n"
        "            int32 r = work();\n"
        "        } catch (Exception e) {\n"
        "            caught = (int32) e;\n"
        "        }\n"
        "        return caught;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// @AfterReturning + @AfterThrowing on the same target, normal path.
// AfterReturning fires (count == 1); AfterThrowing doesn't.
TEST(AfterReturningThrowingTests, bothNormalPathFiresAfterReturningOnly) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterReturning(Audited.class)\n"
        "    public static void onReturn() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "    @AfterThrowing(Audited.class)\n"
        "    public static void onThrow() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 107);   // 7 + 1*100
}

// @AfterReturning + @AfterThrowing on the same target, throw path.
// AfterThrowing fires; AfterReturning doesn't. Caught throw makes
// run() return dropCount.
TEST(AfterReturningThrowingTests, bothThrowPathFiresAfterThrowingOnly) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class A {\n"
        "    @AfterReturning(Audited.class)\n"
        "    public static void onReturn() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "    @AfterThrowing(Audited.class)\n"
        "    public static void onThrow() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() {\n"
        "        throw 42;\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        try {\n"
        "            int32 r = work();\n"
        "        } catch (Exception e) {\n"
        "        }\n"
        "        return (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
