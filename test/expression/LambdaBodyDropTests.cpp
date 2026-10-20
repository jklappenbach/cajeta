//
// Owned locals declared in a lambda's BODY (open-defects D-18).
//
// A lambda body is lowered with no current method (Expression.cpp, so a body
// lookup can never reach an outer local), and every drop-frame site asked the
// current method for its frames. The local's runtime push was emitted and its
// entry was never registered, so no `}`, `return`, `break` or `continue` in a
// lambda popped it. Measured on v0.44.0: each owned local in a lambda body
// leaked, the drop chain's head was left pointing into the dead lambda frame,
// and a loop-body local re-pushed its own slot each pass, so the chain held a
// self-loop. A throw that then unwound past it ran that entry's drop forever
// (primavera's capacity race test spun at 100% of one core in Assert.fail).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// A program whose `run()` returns the live-object delta across `body`, with a
// Cell class, a void runner and an int32 runner for lambdas.
int64_t liveDelta(const std::string& body) {
    std::string src =
        "package test;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 n) { this.n = n; return; }\n"
        "}\n"
        "public final class L {\n"
        "    static int32 hits;\n"
        "    static void each(() -> void body) { body(); }\n"
        "    static int32 value(() -> int32 body) { return body(); }\n"
        "    static int32 caught(() -> void body) {\n"
        "        try {\n"
        "            body();\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    static async void work() { L.hits = L.hits + 1; }\n"
        "    static int32 readOwned(() -> #Cell f) {\n"
        "        Cell c #= f();\n"
        "        Cell churn = heap Cell(777);\n"
        "        return c.n;\n"
        "    }\n"
        "    static int32 readPlain(() -> Cell f) {\n"
        "        Cell c = f();\n"
        "        Cell churn = heap Cell(777);\n"
        "        return c.n;\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        " + body + "\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.L");
    return jit->lookup<int64_t (*)()>("run")();
}

} // namespace

// The defect in one line: a lambda's local leaked at its closing `}`.
TEST(LambdaBodyDropTests, aLambdaBodyLocalIsDroppedAtItsClosingBrace) {
    EXPECT_EQ(liveDelta(
        "for (int32 k = 0; k < 5; k = k + 1) {\n"
        "    L.each(() -> { Cell c = heap Cell(k); });\n"
        "}"), 0);
}

// A local inside a loop in the lambda drops every pass.
TEST(LambdaBodyDropTests, aLoopLocalInALambdaIsDroppedEachPass) {
    EXPECT_EQ(liveDelta(
        "L.each(() -> {\n"
        "    int32 i = 0;\n"
        "    while (i < 4) {\n"
        "        Cell c = heap Cell(i);\n"
        "        i = i + 1;\n"
        "    }\n"
        "});"), 0);
}

// A `return` from a nested block of a lambda drops every enclosing local.
TEST(LambdaBodyDropTests, aReturnFromALambdaDropsEveryEnclosingLocal) {
    EXPECT_EQ(liveDelta(
        "int32 got = L.value(() -> {\n"
        "    Cell a = heap Cell(3);\n"
        "    if (a.n > 0) {\n"
        "        Cell b = heap Cell(4);\n"
        "        return a.n + b.n;\n"
        "    }\n"
        "    return 0;\n"
        "});\n"
        "if (got != 7) { return (int64) -1000; }"), 0);
}

// `break` and `continue` in a lambda's loop drop the locals they skip past.
TEST(LambdaBodyDropTests, breakAndContinueInALambdaLoopDropTheirLocals) {
    EXPECT_EQ(liveDelta(
        "L.each(() -> {\n"
        "    for (int32 i = 0; i < 6; i = i + 1) {\n"
        "        Cell c = heap Cell(i);\n"
        "        if (i == 1) { continue; }\n"
        "        if (i == 4) { break; }\n"
        "    }\n"
        "});"), 0);
}

// A catch clause inside a lambda drops its own locals.
TEST(LambdaBodyDropTests, aCatchClauseInALambdaDropsItsLocals) {
    EXPECT_EQ(liveDelta(
        "L.each(() -> {\n"
        "    try {\n"
        "        throw heap Exception(\"x\");\n"
        "    } catch (Exception e) {\n"
        "        Cell c = heap Cell(1);\n"
        "    }\n"
        "});"), 0);
}

// The primavera shape: loop-body locals, then a throw out of the lambda,
// caught by its caller. Before the fix the unwind spun on a self-looped entry.
TEST(LambdaBodyDropTests, aThrowAfterALambdaLoopUnwindsToTheCaller) {
    EXPECT_EQ(liveDelta(
        "int32 r = L.caught(() -> {\n"
        "    int32 i = 0;\n"
        "    while (i < 3) {\n"
        "        Cell c = heap Cell(i);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    Cell kept = heap Cell(9);\n"
        "    throw heap Exception(\"after the loop\");\n"
        "});\n"
        "if (r != 1) { return (int64) -1000; }"), 0);
}

// A spawn's task handle inside a lambda's scope is dropped with it.
TEST(LambdaBodyDropTests, aSpawnHandleInALambdaScopeIsDropped) {
    EXPECT_EQ(liveDelta(
        "L.hits = 0;\n"
        "L.each(() -> {\n"
        "    scope {\n"
        "        for (int32 k = 0; k < 4; k = k + 1) {\n"
        "            spawn work();\n"
        "        }\n"
        "    }\n"
        "});\n"
        "if (L.hits != 4) { return (int64) -1000; }"), 0);
}

// A lambda written inside a lambda keeps its own frames.
TEST(LambdaBodyDropTests, aNestedLambdaDropsItsOwnLocals) {
    EXPECT_EQ(liveDelta(
        "L.each(() -> {\n"
        "    Cell outer = heap Cell(1);\n"
        "    L.each(() -> { Cell inner = heap Cell(2); });\n"
        "    Cell after = heap Cell(3);\n"
        "});"), 0);
}

// A lambda that hands back its own local by `#` keeps it alive for the caller,
// which frees it.
TEST(LambdaBodyDropTests, aLocalReturnedByTransferOutlivesTheLambda) {
    EXPECT_EQ(liveDelta(
        "int32 n = L.readOwned(() -> {\n"
        "    Cell c = heap Cell(5);\n"
        "    Cell other = heap Cell(6);\n"
        "    return #c;\n"
        "});\n"
        "if (n != 5) { return (int64) -1000 - n; }"), 0);
}

// A plain return of the lambda's own local carries its title out, as a
// method's does, so the local's drop at the return does not free it.
TEST(LambdaBodyDropTests, aLocalReturnedPlainCarriesItsTitleOut) {
    EXPECT_EQ(liveDelta(
        "int32 n = L.readPlain(() -> {\n"
        "    Cell c = heap Cell(5);\n"
        "    Cell other = heap Cell(6);\n"
        "    return c;\n"
        "});\n"
        "if (n != 5) { return (int64) -1000 - n; }"), 0);
}
