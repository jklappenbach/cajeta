// Slices plan Unit 9 backlog — pre-existing drop gaps surfaced by the plan's
// balance asserts (not slices regressions; owned-string controls fail
// identically):
//   9.3.1 `String d; d #= t;` — a move-assign into an UNINITIALIZED local
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

    // 9.4.1 value half — ctor-arg consumer for a fresh Utf8 temp.
    "public final class UHolder {\n"
    "    public Utf8 v;\n"
    "    public UHolder(Utf8 v) { this.v = v; }\n"
    "}\n"

    "public final class D {\n"

    // 9.3.1 — declare-then-move-assign registers a drop for `d`.
    "    public static int64 run_declareThenMoveAssign() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String t = a + b;\n"
    "            String d;\n"
    "            d #= t;\n"
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
    "            String d #= t;\n"
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
    "                String t #= s.substring(23, 29);\n"
    "                keep #= t;\n"
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

    // 9.4.1 value half — a shared-capable VALUE call-result (Utf8) consumed
    // directly as an argument carries its stake with the bytes; the caller
    // owns the temp and must release it at statement end. `s` is a 36-byte
    // owned root, so Utf8.of takes a real stake (never Inline).
    "    public static int64 sinkU(Utf8 u) {\n"
    "        return u.size();\n"
    "    }\n"
    "    public static int64 run_tempUtf8ArgDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            int64 n = D.sinkU(Utf8.of(s));\n"
    "            if (n != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.4.1 value half, control — a NAMED Utf8 local passed as an arg is
    // balanced by its own release drop entry (LVD §6.1 hooks).
    "    public static int64 run_namedUtf8ArgControl() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            Utf8 u = Utf8.of(s);\n"
    "            int64 n = D.sinkU(u);\n"
    "            if (n != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.4.1 value half — fresh Utf8 RECEIVER (`Utf8.of(s).size()`): the
    // value temp is spilled for the by-pointer receiver ABI and nobody
    // releases its stake.
    "    public static int64 run_tempUtf8ReceiverDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            int64 n = Utf8.of(s).size();\n"
    "            if (n != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.4.1 value half — ctor-arg shape (`heap UHolder(Utf8.of(s))`): the
    // ctor's field store retains its own stake; the caller's temp stake
    // must still be released or the root leaks.
    "    public static int64 run_tempUtf8CtorArgDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            UHolder h = heap UHolder(Utf8.of(s));\n"
    "            if (h.v.size() != 36) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.2.1 — `#`-taken elements of a bare local String[] reclaim on array
    // drop. The signal is the store itself (option b): the first `#`-store
    // flips the array's sidecar to owning; scope exit walks the slots
    // (claim-gated) before the storage frees. Substrings are >12 B so the
    // wrappers are real pointer forms.
    "    public static int64 run_arrayElemMoveDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String[] arr = heap String[2];\n"
    "            String p0 #= s.substring(0, 20);\n"
    "            String p1 #= s.substring(16, 36);\n"
    "            arr[0] #= p0;\n"
    "            arr[1] #= p1;\n"
    "            if (arr[0].size() != 20) { return -99; }\n"
    "            if (arr[1].byteAt(0) != (int8) 113) { return -98; }\n"  // 'q'
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.2.1 — the loop-fill shape: the `#`-store runs in the loop BODY's
    // frame while the array lives in the enclosing one. Elements must
    // survive each iteration's scope exit and reclaim at the array's.
    "    public static int64 run_arrayElemLoopFill() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String[] arr = heap String[4];\n"
    "            for (int32 i = 0; i < 4; i++) {\n"
    "                String t #= s.substring(0, 13);\n"
    "                arr[i] #= t;\n"
    "            }\n"
    "            if (arr[3].size() != 13) { return -99; }\n"
    "            if (arr[0].byteAt(12) != (int8) 109) { return -98; }\n"  // 'm'
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.2.1 — overwriting a `#`-stored slot with another `#`-store releases
    // the orphaned first value.
    "    public static int64 run_arrayElemOverwriteDrops() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String[] arr = heap String[1];\n"
    "            String p0 #= s.substring(0, 20);\n"
    "            arr[0] #= p0;\n"
    "            String p1 #= s.substring(5, 30);\n"
    "            arr[0] #= p1;\n"
    "            if (arr[0].size() != 25) { return -99; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.2.1 control — plain (borrow) element stores leave ownership with the
    // named locals; array drop frees storage only and never touches them.
    "    public static int64 run_arrayElemBorrowControl() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String[] arr = heap String[2];\n"
    "            arr[0] = s;\n"
    "            arr[1] = a;\n"
    "            if (arr[0].size() != 36) { return -99; }\n"
    "            if (s.size() != 36) { return -98; }\n"
    "            if (a.size() != 26) { return -97; }\n"
    "        }\n"
    "        return Cajeta.liveCount() - base;\n"
    "    }\n"

    // 9.2.1 — `#arr[i]` moves an element OUT: the slot nulls (the walk
    // skips it) and the receiving local's entry reclaims the value.
    "    public static int64 run_arrayElemTakeOut() {\n"
    "        int64 base = Cajeta.liveCount();\n"
    "        {\n"
    "            String a = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "            String b = \"0123456789\";\n"
    "            String s = a + b;\n"
    "            String[] arr = heap String[2];\n"
    "            String p0 #= s.substring(0, 20);\n"
    "            String p1 #= s.substring(16, 36);\n"
    "            arr[0] #= p0;\n"
    "            arr[1] #= p1;\n"
    "            String back #= arr[0];\n"
    "            if (back.size() != 20) { return -99; }\n"
    "            if (arr[1].size() != 20) { return -98; }\n"
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
TEST_F(DropGapTests, namedUtf8ArgControl)    { EXPECT_EQ(i64("run_namedUtf8ArgControl"), 0); }
TEST_F(DropGapTests, tempUtf8ArgDrops)       { EXPECT_EQ(i64("run_tempUtf8ArgDrops"), 0); }
TEST_F(DropGapTests, tempUtf8ReceiverDrops)  { EXPECT_EQ(i64("run_tempUtf8ReceiverDrops"), 0); }
TEST_F(DropGapTests, tempUtf8CtorArgDrops)   { EXPECT_EQ(i64("run_tempUtf8CtorArgDrops"), 0); }
