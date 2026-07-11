//
// title-tracking Unit 2 (plan 2.1) — static linearity for locals (spec §3).
// A local's role is fixed by its initializer shape: owner (fresh
// construction, #-returning call, `= #x` move) or borrow (bare identifier,
// field/element read, plain-returning call). `#x` demands a statically-
// active owner; every transfer (assignment, call arg, ctor arg) marks the
// source moved; reassignment re-arms; branch merges are conservative.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

// Class-typed payload + a #-formal sink. Cell is heap-observable via
// Cajeta.liveCount(). Sink.take(#Cell) consumes; the explicit
// Cajeta.dropValue is the ~HashMap idiom — an automatic callee-side drop
// entry for consumed #T formals is the Unit 5 signature-ABI work, not
// Unit 2's (local linearity).
const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n"
    "public class Sink {\n"
    "    public int32 seen;\n"
    "    public void take(#Cell c) {\n"
    "        this.seen = this.seen + c.n;\n"
    "        Cajeta.dropValue(c);\n"
    "    }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

} // namespace

// 2.1.1 — `#x` from a borrow forges a second owner. The borrow-shaped local
// (bare-identifier initializer) must reject move-out, naming the owner.
TEST(LocalLinearityTests, moveOutOfBorrowIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell obj = heap Cell(7);\n"
        "        Cell obj2 = obj;\n"       // borrow: bare-identifier init
        "        Cell obj3 = #obj2;\n"     // move out of a borrow — reject
        "        return obj3.n;\n"
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_MOVE_OF_BORROW");
    EXPECT_NE(msg.find("obj2"), std::string::npos) << msg;
    EXPECT_NE(msg.find("obj"), std::string::npos) << msg;
}

// 2.1.2 — the same owner transferred twice as a call argument: the second
// `#v` is use-after-move, named with the prior transfer.
TEST(LocalLinearityTests, doubleCallArgTransferIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s1 = heap Sink();\n"
        "        Sink s2 = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        s1.take(#v);\n"
        "        s2.take(#v);\n"          // second transfer — reject
        "        return s2.seen;\n"
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
    EXPECT_NE(msg.find("v"), std::string::npos) << msg;
}

// 2.1.2 corollary — a plain READ after a call-arg transfer is also
// use-after-move (today it compiles and reads a deactivated value).
TEST(LocalLinearityTests, readAfterCallArgTransferIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s1 = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        s1.take(#v);\n"
        "        return v.n;\n"           // read of moved local — reject
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
}

// 2.1.2 — ctor-arg transfers mark moved too (`heap Holder(#v)`).
TEST(LocalLinearityTests, readAfterCtorArgTransferIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell held;\n"
        "    public Holder(#Cell c) { this.held = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell v = heap Cell(5);\n"
        "        Holder h = heap Holder(#v);\n"
        "        return v.n;\n"           // read of moved local — reject
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
}

// 2.1.3 — legal same-scope move chain: exactly one drop at scope exit
// (liveCount delta 0), the final holder readable.
TEST(LocalLinearityTests, legalMoveChainSingleDrop) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell obj = heap Cell(42);\n"
        "        Cell obj3 = #obj;\n"
        "        Cell obj4 = #obj3;\n"
        "        return obj4.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// 2.1.3 — reads of the moved-out links are rejected.
TEST(LocalLinearityTests, readOfMovedChainLinkIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell obj = heap Cell(42);\n"
        "        Cell obj3 = #obj;\n"
        "        return obj.n;\n"         // moved at the line above
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
}

// 2.1.3 — borrows are move-transparent: a borrow taken BEFORE the owner
// moved stays readable (the deferred §7.4 hazard, deliberately legal).
TEST(LocalLinearityTests, borrowRemainsReadableAcrossOwnerMove) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell obj = heap Cell(9);\n"
        "        Cell ref = obj;\n"       // borrow
        "        Cell obj3 = #obj;\n"     // owner moves; borrow untouched
        "        return ref.n + obj3.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// 2.1.4 — loop re-arm: transfer, then reassign a fresh value next
// iteration (the trySplit shape). Compiles; no leak, no double free.
TEST(LocalLinearityTests, loopTransferReArmCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Sink s = heap Sink();\n"
        "        int32 i = 0;\n"
        "        Cell piece = heap Cell(1);\n"
        "        while (i < 4) {\n"
        "            s.take(#piece);\n"
        "            piece = heap Cell(i + 2);\n"  // re-arm
        "            i = i + 1;\n"
        "        }\n"
        "        s.take(#piece);\n"
        "        return s.seen;\n"                 // 1+2+3+4+5 = 15
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 2.1.5 — branch join is conservative: moved on one path = moved after.
TEST(LocalLinearityTests, branchJoinMovedIsCompileError) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run(int32 flag) {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        if (flag > 0) {\n"
        "            s.take(#v);\n"
        "        }\n"
        "        return v.n;\n"           // moved on the taken path — reject
        "    }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
}

// 2.1.5 control — reassigned on the SAME path after the move: re-armed at
// the join on that path, but the untaken path never moved it. Legal.
TEST(LocalLinearityTests, branchMoveThenReArmCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work(int32 flag) {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        if (flag > 0) {\n"
        "            s.take(#v);\n"
        "            v = heap Cell(6);\n" // re-arm before the join
        "        }\n"
        "        return v.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work(1) + work(0);\n"   // 6 + 5
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// 2.1.6 — class-typed bare declaration + inner-scope move-assign into the
// outer local (generalizes the String-only 9.3.1 retarget): value survives
// the inner scope, exactly one drop at the declaring scope's exit.
TEST(LocalLinearityTests, bareDeclMoveAssignAcrossScopes) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell keep;\n"
        "        {\n"
        "            Cell t = heap Cell(33);\n"
        "            keep = #t;\n"
        "        }\n"
        "        return keep.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 33);
}
