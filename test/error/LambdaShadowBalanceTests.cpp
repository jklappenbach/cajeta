// cajeta-profiler 6.4.B — a lambda call must leave the line-info shadow stack
// exactly as it found it.
//
// The defect: a lambda body is codegen'd inline by LambdaExpression::generate-
// Code, which does NOT run Method::generateCode's prologue and so never emits
// the paired `__cajeta_line_enter`. Its `return` statements still funnel
// through emitScopeExitToWatermark, which emits `__cajeta_line_leave` — and
// that leave takes no argument, so it pops whatever is on top: the ENCLOSING
// method's frame. Every block-bodied lambda call eats one frame off the
// caller's stack.
//
// The debug frame chain is immune to the same shape because its leave is
// node-paired (`struct cajeta_dbg_frame::owner`, added for exactly this class
// of bug); the shadow stack, an index into an array, has no equivalent.
//
// In the tour this ate `Stream.forEach` and then `tour.Tour.main`, after which
// every demo rendered at depth 0 as if nothing had called it.
//
// Observed IN-LANGUAGE through getStackTrace(), the only consumer of the
// shadow stack a test can reach — see FiberShadowStackTests for why calling
// __cajeta_shadow_get_top() from the test process reads the wrong copy.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -1;
    return fn();
}
} // namespace

// 6.4.B.1.a — measure the trace depth, run a block-bodied lambda five times,
// measure again. Equal depths => balanced. Returns 0 on success, otherwise
// before*1000 + after so a failure reports both numbers.
TEST(LambdaShadowBalance, blockLambdaCallLeavesTheShadowStackAsItFoundIt) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 depth() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 before = D.depth();\n"
        "        (int32) -> int32 f = (int32 x) -> { return x + 1; };\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 5) {\n"
        "            acc = acc + f(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 after = D.depth();\n"
        "        if (after == before) { return 0; }\n"
        "        return before * 1000 + after;\n"
        "    }\n"
        "}\n"), 0);
}

// An expression-bodied lambda emits neither probe today, so it is already
// balanced. Pinned so a future change that gives lambdas a real frame has to
// keep both halves paired rather than only adding the enter.
TEST(LambdaShadowBalance, expressionLambdaCallLeavesTheShadowStackAsItFoundIt) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 depth() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 before = D.depth();\n"
        "        (int32) -> int32 f = (int32 x) -> x + 1;\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 5) {\n"
        "            acc = acc + f(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 after = D.depth();\n"
        "        if (after == before) { return 0; }\n"
        "        return before * 1000 + after;\n"
        "    }\n"
        "}\n"), 0);
}

// --- 6.4.C: the lambda is a REAL frame -------------------------------------
//
// Balance alone left lambdas invisible: a profile of a stream pipeline showed
// the terminal and the element work, with nothing in between naming the
// user's own callback. These assert the frame is actually there — a trace
// taken INSIDE the lambda is exactly one deeper than the same trace taken
// from the enclosing method, for both lambda body forms.

TEST(LambdaShadowBalance, blockLambdaContributesExactlyOneFrame) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 depth() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 outside = D.depth();\n"
        "        (int32) -> int32 f = (int32 x) -> { return D.depth(); };\n"
        "        int32 inside = f(0);\n"
        "        if (inside == outside + 1) { return 0; }\n"
        "        return outside * 1000 + inside;\n"
        "    }\n"
        "}\n"), 0);
}

TEST(LambdaShadowBalance, expressionLambdaContributesExactlyOneFrame) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 depth() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 outside = D.depth();\n"
        "        (int32) -> int32 f = (int32 x) -> D.depth();\n"
        "        int32 inside = f(0);\n"
        "        if (inside == outside + 1) { return 0; }\n"
        "        return outside * 1000 + inside;\n"
        "    }\n"
        "}\n"), 0);
}

// A void lambda has no `return` at all, so its frame can only be popped by the
// fall-through path in LambdaExpression::generateCode. Without that leave the
// enter leaks and the caller's stack GROWS — the mirror image of the 6.4.B
// erosion, and just as invisible without an explicit check.
TEST(LambdaShadowBalance, voidLambdaCallLeavesTheShadowStackAsItFoundIt) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class D {\n"
        "    public static int32 sink = 0;\n"
        "    public static void boom() { throw heap Exception(\"x\"); }\n"
        "    public static int32 depth() {\n"
        "        try {\n"
        "            D.boom();\n"
        "            return 0;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs #= e.getStackTrace();\n"
        "            return fs.count();\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 before = D.depth();\n"
        "        (int32) -> void g = (int32 x) -> { D.sink = D.sink + x; };\n"
        "        int32 i = 0;\n"
        "        while (i < 5) {\n"
        "            g(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 after = D.depth();\n"
        "        if (after == before) { return 0; }\n"
        "        return before * 1000 + after;\n"
        "    }\n"
        "}\n"), 0);
}
