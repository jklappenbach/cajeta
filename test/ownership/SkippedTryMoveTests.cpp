// `return #x` of a String that holds no title returns an owned copy (decided
// with Julian 2026-10-23, as rule 5's lent-String `#=` resolves to a copy).
// Measured on v0.46.3 (cajeta-cloud-aws S3Errors): a literal-initialised local
// moved into inside a try that a run skipped raised CAJETA_PANIC_TITLE_MISS (3)
// at `return #out`, because the try's `#=` made the title a runtime flag;
// with no such assignment the flag was a constant 0 and the same return handed
// back a borrow. Both now copy.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string program(const std::string& run) {
    return
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.error.RecoverableException;\n"
        "public final class U {\n"
        // Moved into in the try, reassigned in the catch.
        "    static #String tryMoves(boolean enter) {\n"
        "        String out = \"\";\n"
        "        if (enter) {\n"
        "            try {\n"
        "                out #= \"\" + \"inside\";\n"
        "            } catch (RecoverableException e) {\n"
        "                out = \"\";\n"
        "            }\n"
        "        }\n"
        "        return #out;\n"
        "    }\n"
        // Moved into only in the catch.
        "    static #String catchMoves(boolean enter) {\n"
        "        String out = \"start\";\n"
        "        int32 n = 0;\n"
        "        if (enter) {\n"
        "            try {\n"
        "                n = 1;\n"
        "            } catch (RecoverableException e) {\n"
        "                out #= \"\" + \"caught\";\n"
        "            }\n"
        "        }\n"
        "        return #out;\n"
        "    }\n"
        // No assignment can give `out` a title: a compile-time miss.
    "    static #String literalOnly() {\n"
    "        String out = \"lit\";\n"
    "        return #out;\n"
    "    }\n"
    "    public static int32 run() {\n" + run +
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& run) {
    auto jit = CajetaJit::compile(program(run), "test.U");
    return jit->lookup<int32_t (*)()>("run")();
}

} // namespace

TEST(SkippedTryMoveTests, skippedTryReturnsTheLiteral) {
    EXPECT_EQ(runI32(
        "        String r #= U.tryMoves(false);\n"
        "        return r.byteLength();\n"), 0);
}

TEST(SkippedTryMoveTests, takenTryReturnsTheMovedValue) {
    EXPECT_EQ(runI32(
        "        String r #= U.tryMoves(true);\n"
        "        return r.equals(\"inside\") ? 1 : 0;\n"), 1);
}

TEST(SkippedTryMoveTests, skippedTryWithACatchMoveReturnsTheLiteral) {
    EXPECT_EQ(runI32(
        "        String r #= U.catchMoves(false);\n"
        "        return r.equals(\"start\") ? 1 : 0;\n"), 1);
}

TEST(SkippedTryMoveTests, bothPathsKeepTheLiveCountBalanced) {
    EXPECT_EQ(runI32(
        "        String w0 #= U.tryMoves(true);\n"
        "        String w1 #= U.tryMoves(false);\n"
        "        int64 live = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        int32 total = 0;\n"
        "        while (i < 200) {\n"
        "            String a #= U.tryMoves((i % 2) == 0);\n"
        "            String b #= U.catchMoves((i % 3) == 0);\n"
        "            total = total + a.byteLength() + b.byteLength();\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return Cajeta.liveCount() == live ? total : -1;\n"), 100 * 6 + 200 * 5);
}

TEST(SkippedTryMoveTests, aStaticMissReturnsAnOwnedCopy) {
    // The copy is a live heap String while it is held, and is freed with it.
    EXPECT_EQ(runI32(
        "        int64 before = Cajeta.liveCount();\n"
        "        int32 held = 0;\n"
        "        if (true) {\n"
        "            String r #= U.literalOnly();\n"
        "            held = (int32) (Cajeta.liveCount() - before);\n"
        "            if (!r.equals(\"lit\")) held = -10;\n"
        "        }\n"
        "        int32 after = (int32) (Cajeta.liveCount() - before);\n"
        "        return held * 10 + after;\n"), 10);
}
