// Spec 6.2.2 — `#x` of a STATIC owner folds to the constant title (no entry
// read). The fold is sound because the scope records a move flow-insensitively
// and a second `#x` of a transferred name is rejected statically, so the site
// that folds is always the first move in flow.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

void expectRejected(const std::string& src, const std::string& code) {
    try {
        CajetaJit::compile(src.c_str(), "test.A");
        ADD_FAILURE() << "expected " << code;
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), code) << "wrong diagnostic: " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell c;\n"
    "        public Holder() { }\n"
    "    }\n";

std::string loop(const std::string& members, const std::string& probeBody,
                 const std::string& want, int failBase) {
    return std::string(PRE) + members
        + "    static int32 probe(int32 i) {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 1;\n"
        + "        while (i < 65) {\n"
        + "            if (A.probe(i) != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

std::string rejectSrc(const std::string& members) {
    return std::string(PRE) + members
        + "    public static int32 run() { return 0; }\n"
        + "}\n";
}

} // namespace

TEST(MoveFoldTests, moveReturnOfStaticOwnerIsBalanced) {
    std::string src = loop(
        "    static #Cell f(int32 i) { Cell c = heap Cell(i); return #c; }\n",
        "        Cell got #= A.f(i);\n"
        "        return got.n;\n", "i", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = leaked or freed twice";
}

TEST(MoveFoldTests, sharpBindOfStaticOwnerMovesOnce) {
    std::string src = loop("",
        "        Cell y = heap Cell(i);\n"
        "        Cell x #= #y;\n"                      // y's title moves to x; y is a borrow now
        "        Holder h = heap Holder();\n"
        "        h.c #= #x;\n"                         // and on to the holder
        "        return h.c.n;\n", "i", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the cell leaked or was freed twice";
}

TEST(MoveFoldTests, secondMoveAfterConditionalMoveIsRejected) {
    expectRejected(rejectSrc(
        "    static #Cell f(boolean m, int32 i) {\n"
        "        Cell c = heap Cell(i);\n"
        "        Holder h = heap Holder();\n"
        "        if (m) { h.c #= #c; }\n"              // moved on one path
        "        return #c;\n"                          // a second `#c`: rejected statically
        "    }\n"), "CAJETA_ERROR_MOVE_OF_BORROW");
}

// The loop-carried move: one `#y` site executed twice, of a `y` declared
// outside the loop. Neither the entry read nor the constant makes it safe, and
// the static rejection it needs does not exist yet, so this is DISABLED.
TEST(MoveFoldTests, DISABLED_loopCarriedMoveWitness) {
    std::string src = loop("",
        "        Cell y = heap Cell(i);\n"
        "        int32 k = 0;\n"
        "        int32 sum = 0;\n"
        "        while (k < 2) {\n"
        "            Cell x #= #y;\n"                   // iteration 2 moves what iteration 1 freed
        "            sum = sum + x.n;\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return sum;\n", "i + i", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = the second iteration read a freed cell; 41 = live count moved";
}

TEST(MoveFoldTests, conditionalMoveThenModeCarryReadsTheEntry) {
    std::string src = loop(
        "    static #Cell f(boolean m, int32 i) {\n"
        "        Cell c = heap Cell(i);\n"
        "        Holder h = heap Holder();\n"
        "        if (m) { h.c #= #c; return heap Cell(i + 100); }\n"
        "        return #= c;\n"                        // the entry says 1 on this path
        "    }\n",
        "        Cell a #= A.f(i % 2 == 0, i);\n"
        "        if (i % 2 == 0) { return a.n - 100; }\n"
        "        return a.n;\n", "i", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = leaked or freed twice on one of the paths";
}
