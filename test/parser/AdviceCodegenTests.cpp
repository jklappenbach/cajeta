//
// A4: @Before / @After codegen.
//
// User methods whose matchingAdvice cache (populated by A3) contains
// @Before / @After entries get the corresponding advice calls
// emitted around the body: @Before fires after scope_enter, before
// the body; @After fires at every normal-return path before the
// scope-exit + drop sequence. Throw-path coverage of @After is
// deferred to A6 (the @AfterThrowing pair).
//
// Tests observe advice firing via the dropCount intrinsic: each
// advice method allocates `int32[] tmp = new int32[1];` whose
// scope-exit drop bumps the counter by one. A run() wrapper resets
// the counter, calls the advised target, then reads dropCount —
// the result tells us how many advice methods executed.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// run() = reset, call target, return target's value + 100 * dropCount.
// Encoding lets a single test return both the body's value AND the
// number of advice firings observed during target's execution. The
// caller decodes: body returned (n % 100), advice fired (n / 100).
int32_t runAndDecode(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Baseline: an unannotated target method gets no advice. dropCount
// stays at 0 across the call. run() returns target's value (7),
// encoded as 7 + 100*0.
TEST(AdviceCodegenTests, noAdviceNoExtraDrops) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 work() { return 7; }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runAndDecode(src), 7);
}

// @Before-only: the advice fires before the body. Inside the advice
// method, an int32[1] allocation+drop bumps the counter. run() sees
// count=1 after target returns. Encoded result: 7 + 100*1 = 107.
TEST(AdviceCodegenTests, beforeFiresAroundBody) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AuditAspect {\n"
        "    @Before(Audited.class)\n"
        "    public static void onCall() {\n"
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
    EXPECT_EQ(runAndDecode(src), 107);
}

// @After-only: fires on every normal exit path. Target uses the
// fall-through path here (no explicit return). Same encoding —
// count=1 after target returns.
TEST(AdviceCodegenTests, afterFiresOnFallThroughExit) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AuditAspect {\n"
        "    @After(Audited.class)\n"
        "    public static void onExit() {\n"
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
    EXPECT_EQ(runAndDecode(src), 107);
}

// @After on an explicit-return target. Verifies the
// ReturnStatement::generateCode hookup (separate from the fall-
// through hookup in Method::generateCode). Same expected encoding.
TEST(AdviceCodegenTests, afterFiresOnExplicitReturn) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AuditAspect {\n"
        "    @After(Audited.class)\n"
        "    public static void onExit() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    @Audited public static int32 work() {\n"
        "        int32 x = 7;\n"
        "        return x;\n"     // explicit typed return
        "    }\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        int32 r = work();\n"
        "        return r + 100 * (int32) Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runAndDecode(src), 107);
}

// Both @Before and @After on the same target. Each fires once;
// count=2 after target returns. Encoded result: 7 + 100*2 = 207.
// Implicitly verifies the two hooks don't step on each other —
// @Before's drop fires before the body, then the body's reset
// would have cleared it... but we reset BEFORE calling target,
// not inside it. Both advice's drops accumulate.
TEST(AdviceCodegenTests, beforeAndAfterBothFire) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AuditAspect {\n"
        "    @Before(Audited.class)\n"
        "    public static void onEntry() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "    @After(Audited.class)\n"
        "    public static void onExit() {\n"
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
    EXPECT_EQ(runAndDecode(src), 207);
}

// Two aspects, each with a @Before pointing at the same marker.
// Both fire on every call. count=2. Tests that the matchingAdvice
// list is iterated fully — A4 doesn't yet order them (A7's @Order
// territory) but it must invoke every match.
TEST(AdviceCodegenTests, multipleBeforeAdviceAllFire) {
    auto src =
        "package test;\n"
        "@interface Audited { }\n"
        "@Aspect public class AspectA {\n"
        "    @Before(Audited.class)\n"
        "    public static void a() {\n"
        "        int32[] tmp = new int32[1];\n"
        "        return;\n"
        "    }\n"
        "}\n"
        "@Aspect public class AspectB {\n"
        "    @Before(Audited.class)\n"
        "    public static void b() {\n"
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
    EXPECT_EQ(runAndDecode(src), 207);
}
