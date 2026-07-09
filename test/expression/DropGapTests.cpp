// Slices plan Unit 9 backlog — pre-existing drop gaps surfaced by the plan's
// balance asserts (not slices regressions; owned-string controls fail
// identically):
//   9.3.1 `String d; d = #t;` — a move-assign into an UNINITIALIZED local
//         registers no drop for `d`; the wrapper + buffer leak.
//   9.4.1 Unnamed temporaries — a droppable call-result consumed directly as
//         an argument has no drop entry; wrapper + stake leak.
// Each test diffs Cajeta.liveCount() across a scope that must fully reclaim.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const std::string MODULE_SRC =
    "package test;\n"
    "public final class D {\n"

    // 9.3.1 — declare-then-move-assign registers a drop for `d`.
    "    public static int64 run_declareThenMoveAssign() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String t = a + b;\n"
    "            String d;\n"
    "            d = #t;\n"
    "            if (d.size() != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.3.1 control — initialized move declaration is already balanced.
    "    public static int64 run_moveInitControl() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String t = a + b;\n"
    "            String d = #t;\n"
    "            if (d.size() != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.3.1 regression — the assignment may run in an INNER block while the
    // local is declared outside: the entry must live in the DECLARING frame
    // (a push at the assignment site freed the value at the inner `}` — the
    // sweep-caught substringOutlivesSource crash) and the value must both
    // survive the inner scope AND still reclaim at the declaring scope's end.
    "    public static int64 run_moveAssignAcrossScopes() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String keep;\n"
    "            {\n"
    "                String t = s.substring(23, 29);\n"
    "                keep = #t;\n"
    "            }\n"
    "            if (keep.size() != 6) { return -99; }\n"
    "            if (keep.charAt(0) != (int8) 120) { return -98; }\n"  // 'x'
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.4.1 — a String call-result consumed directly as an argument drops at
    // statement end (wrapper + stake). substring of a heap root takes a stake
    // on the root; the unnamed temp must release it or the root leaks too.
    "    public static int64 sink(String s) {\n"
    "        return (int64) s.size();\n"
    "    }\n"
    "    public static int64 run_tempArgDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            int64 n = D.sink(s.substring(10, 16));\n"
    "            if (n != 6) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.4.1 — an owned call-result temp (allocating method, no shared root)
    "    public static int64 run_tempOwnedArgDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            int64 n = D.sink(s.toUpperCase());\n"
    "            if (n != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    "}\n";

class DropGapTests : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        jit = CajetaJit::compile(MODULE_SRC, "test.D");
    }
    static void TearDownTestSuite() {
        jit.reset();
    }
    static int64_t i64(const char* name) {
        auto fn = jit->lookup<int64_t (*)()>(name);
        return fn();
    }
    static std::unique_ptr<CajetaJit> jit;
};

std::unique_ptr<CajetaJit> DropGapTests::jit;

}  // namespace

TEST_F(DropGapTests, moveInitControl)        { EXPECT_EQ(i64("run_moveInitControl"), 0); }
TEST_F(DropGapTests, declareThenMoveAssign)  { EXPECT_EQ(i64("run_declareThenMoveAssign"), 0); }
TEST_F(DropGapTests, moveAssignAcrossScopes)  { EXPECT_EQ(i64("run_moveAssignAcrossScopes"), 0); }
TEST_F(DropGapTests, tempArgDrops)           { EXPECT_EQ(i64("run_tempArgDrops"), 0); }
TEST_F(DropGapTests, tempOwnedArgDrops)      { EXPECT_EQ(i64("run_tempOwnedArgDrops"), 0); }
