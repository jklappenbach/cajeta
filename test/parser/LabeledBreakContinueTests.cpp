//
// Labeled break/continue (`break outer;` / `continue outer;`) referencing
// an enclosing labeled loop. Unlabeled forms (which jump to the innermost
// loop) were already working — these tests cover the labeled path.
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

// `break outer;` exits both loops; without the label it would only exit
// the inner one. Counter tracks how far we got — labeled-break should
// stop us at (0, 0).
TEST(LabeledBreakContinueTests, labeledBreakExitsOuterLoop) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 visited = 0;\n"
        "        outer: for (int32 i = 0; i < 3; i++) {\n"
        "            for (int32 j = 0; j < 3; j++) {\n"
        "                if (i == 1 && j == 1) {\n"
        "                    break outer;\n"
        "                }\n"
        "                visited++;\n"
        "            }\n"
        "        }\n"
        "        return visited;\n"
        "    }\n"
        "}\n";
    // Visited: (0,0)(0,1)(0,2)(1,0) then break-outer fires. = 4.
    EXPECT_EQ(runI32(src), 4);
}

// `continue outer;` advances the outer loop's iteration variable, skipping
// the rest of the inner one. Same outer-counter pattern.
TEST(LabeledBreakContinueTests, labeledContinueAdvancesOuter) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 visited = 0;\n"
        "        outer: for (int32 i = 0; i < 3; i++) {\n"
        "            for (int32 j = 0; j < 3; j++) {\n"
        "                if (j == 1) {\n"
        "                    continue outer;\n"
        "                }\n"
        "                visited++;\n"
        "            }\n"
        "        }\n"
        "        return visited;\n"
        "    }\n"
        "}\n";
    // For each outer i (0,1,2): only j=0 increments before continue outer.
    // visited = 3.
    EXPECT_EQ(runI32(src), 3);
}

// Sanity: bare `break;` still hits the innermost loop. Re-verify that the
// labeled-break wiring didn't regress unlabeled behavior.
TEST(LabeledBreakContinueTests, unlabeledBreakStillExitsInner) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 visited = 0;\n"
        "        outer: for (int32 i = 0; i < 2; i++) {\n"
        "            for (int32 j = 0; j < 3; j++) {\n"
        "                if (j == 1) break;\n"
        "                visited++;\n"
        "            }\n"
        "        }\n"
        "        return visited;\n"
        "    }\n"
        "}\n";
    // For each outer i (0,1): j=0 visits, j=1 break-inner. = 2.
    EXPECT_EQ(runI32(src), 2);
}
