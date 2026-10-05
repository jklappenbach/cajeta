// `x #= c ? a : b` is `#=` applied to the arm that runs: the chosen arm's title moves, a lent arm
// records a borrow, and the arm that does not run keeps its title (field-store-ownership 6.2.6).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& workBody) {
    return "package test;\n"
           "public class Cell {\n"
           "    public int64 n;\n"
           "    public Cell(int64 v) { this.n = v; }\n"
           "    public static boolean operator< (Cell x, Cell y) { return x.n < y.n; }\n"
           "}\n"
           "public class Box {\n"
           "    public Cell c;\n"
           "}\n"
           "public final class K {\n"
           "    public static void pickSlot(Cell[] a, int32 i, int32 j, Cell[] s, boolean u) {\n"
           "        s[0] #= u ? a[i] : a[j];\n"
           "    }\n"
           "    public static void pickLocal(Cell[] s, boolean u) {\n"
           "        Cell x = heap Cell(11L);\n"
           "        Cell y = heap Cell(22L);\n"
           "        s[0] #= u ? x : y;\n"
           "    }\n"
           "    public static void pickLent(Cell[] s, ^Cell lent, boolean u) {\n"
           "        Cell y = heap Cell(22L);\n"
           "        s[0] #= u ? lent : y;\n"
           "    }\n"
           "    public static void pickField(Box b, boolean u) {\n"
           "        Cell x = heap Cell(11L);\n"
           "        Cell y = heap Cell(22L);\n"
           "        b.c #= u ? x : y;\n"
           "    }\n"
           "    public static void pickNested(Cell[] s, int32 k) {\n"
           "        Cell x = heap Cell(11L);\n"
           "        Cell y = heap Cell(22L);\n"
           "        Cell z = heap Cell(33L);\n"
           "        s[0] #= k == 0 ? x : (k == 1 ? y : z);\n"
           "    }\n"
           "    public static void swapIfLess(Cell[] a, int32 i, int32 j) {\n"
           "        Cell va #= a[i];\n"
           "        Cell vb #= a[j];\n"
           "        boolean lt = vb < va;\n"
           "        a[i] #= lt ? vb : va;\n"
           "        a[j] #= lt ? va : vb;\n"
           "    }\n"
           "    public static void bindLocal(Cell[] s, boolean u) {\n"
           "        Cell x = heap Cell(11L);\n"
           "        Cell y = heap Cell(22L);\n"
           "        Cell z #= u ? x : y;\n"
           "        s[0] #= z;\n"
           "    }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static void churn() {\n"
           "        int64 i = 0;\n"
           "        while (i < 400L) { Cell c = heap Cell(107800L); i = i + 1; }\n"
           "    }\n"
           "    public static int32 work() {\n"
           "        " + workBody + "\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        int64 base = Cajeta.liveCount();\n"
           "        int32 t = Ut.work();\n"
           "        int64 leaked = Cajeta.liveCount() - base;\n"
           "        return (int32) (leaked * 100) + t;\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& workBody) {
    auto jit = CajetaJit::compile(makeSource(workBody), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string pickSlot(const char* u, const char* expect) {
    return std::string(
        "Cell[] a = heap Cell[2];\n"
        "a[0] #= heap Cell(5L);\n"
        "a[1] #= heap Cell(7L);\n"
        "Cell[] s = heap Cell[1];\n"
        "K.pickSlot(a, 0, 1, s, ") + u + ");\n"
        "a = heap Cell[1];\n"
        "Ut.churn();\n"
        "if (s[0].n != " + expect + ") { return -1; }\n"
        "return 1;";
}

}  // namespace

// The chosen slot's title moves, so the slot survives its first array dying.
TEST(ConditionalSharpStoreTests, slotArmTakesTheChosenTitle) {
    EXPECT_EQ(runJit(pickSlot("true", "5L")), 1);
    EXPECT_EQ(runJit(pickSlot("false", "7L")), 1);
}

// The chosen owned local moves into the slot; the other is freed with the frame.
TEST(ConditionalSharpStoreTests, ownedLocalArmMoves) {
    EXPECT_EQ(runJit(
        "Cell[] s = heap Cell[1];\n"
        "K.pickLocal(s, true);\n"
        "Ut.churn();\n"
        "if (s[0].n != 11L) { return -1; }\n"
        "K.pickLocal(s, false);\n"
        "Ut.churn();\n"
        "if (s[0].n != 22L) { return -2; }\n"
        "return 1;"), 1);
}

// A lent arm records a borrow: the lender keeps and frees it, and nothing leaks.
TEST(ConditionalSharpStoreTests, lentArmRecordsABorrow) {
    EXPECT_EQ(runJit(
        "Cell keep = heap Cell(9L);\n"
        "Cell[] s = heap Cell[1];\n"
        "K.pickLent(s, keep, true);\n"
        "Ut.churn();\n"
        "if (s[0].n != 9L || keep.n != 9L) { return -1; }\n"
        "s = heap Cell[1];\n"
        "Ut.churn();\n"
        "if (keep.n != 9L) { return -2; }\n"
        "return 1;"), 1);
}

TEST(ConditionalSharpStoreTests, fieldArmMoves) {
    EXPECT_EQ(runJit(
        "Box b = heap Box();\n"
        "K.pickField(b, false);\n"
        "Ut.churn();\n"
        "if (b.c.n != 22L) { return -1; }\n"
        "return 1;"), 1);
}

TEST(ConditionalSharpStoreTests, nestedConditionalArmsMove) {
    EXPECT_EQ(runJit(
        "Cell[] s = heap Cell[1];\n"
        "int32 k = 0;\n"
        "while (k < 3) {\n"
        "    K.pickNested(s, k);\n"
        "    Ut.churn();\n"
        "    if (s[0].n != (int64) (11 * (k + 1))) { return -1 - k; }\n"
        "    k = k + 1;\n"
        "}\n"
        "return 1;"), 1);
}

// Two conditionals on one test move each local exactly once, whichever way the test goes.
TEST(ConditionalSharpStoreTests, correlatedConditionalsSwapTitles) {
    EXPECT_EQ(runJit(
        "Cell[] a = heap Cell[4];\n"
        "a[0] #= heap Cell(5L);\n"
        "a[1] #= heap Cell(3L);\n"
        "a[2] #= heap Cell(1L);\n"
        "a[3] #= heap Cell(9L);\n"
        "K.swapIfLess(a, 0, 1);\n"
        "K.swapIfLess(a, 2, 3);\n"
        "Ut.churn();\n"
        "if (a[0].n != 3L || a[1].n != 5L || a[2].n != 1L || a[3].n != 9L) { return -1; }\n"
        "return 1;"), 1);
}

TEST(ConditionalSharpStoreTests, localBindTakesTheChosenArm) {
    EXPECT_EQ(runJit(
        "Cell[] s = heap Cell[1];\n"
        "K.bindLocal(s, true);\n"
        "Ut.churn();\n"
        "if (s[0].n != 11L) { return -1; }\n"
        "K.bindLocal(s, false);\n"
        "Ut.churn();\n"
        "if (s[0].n != 22L) { return -2; }\n"
        "return 1;"), 1);
}

// A slot arm that holds a borrow forwards the borrow: no take panic, and the owner keeps it.
TEST(ConditionalSharpStoreTests, borrowedSlotArmForwardsTheBorrow) {
    EXPECT_EQ(runJit(
        "Cell keep = heap Cell(5L);\n"
        "Cell[] a = heap Cell[2];\n"
        "a[0] = keep;\n"
        "a[1] #= heap Cell(7L);\n"
        "Cell[] s = heap Cell[1];\n"
        "K.pickSlot(a, 0, 1, s, true);\n"
        "if (s[0].n != 5L) { return -1; }\n"
        "s = heap Cell[1];\n"
        "a = heap Cell[1];\n"
        "Ut.churn();\n"
        "if (keep.n != 5L) { return -2; }\n"
        "return 1;"), 1);
}
