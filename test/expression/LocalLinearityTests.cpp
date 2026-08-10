//
// title-tracking Unit 2 (plan 2.1) — static linearity for locals (spec §3).
// A local's role is fixed by its initializer shape: owner (fresh
// construction, #-returning call, `#= x` move) or borrow (bare identifier,
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

// transfer-demotes-to-borrow — a read of a transferred binding is legal:
// `#` moves the title, not the binding. Asserts the source compiles.
// Value-correctness of such reads is pinned in DemotedBindingReadTests.
void compileExpectOk(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "expected a clean compile, got "
                      << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected a clean compile, got " << e.what();
    }
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
        "        Cell obj3 #= obj2;\n"     // move out of a borrow — reject
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
        "        s2.take(#v);\n"          // second transfer — still rejected
        "        return s2.seen;\n"
        "    }\n"
        "}\n";
    // A transfer demotes its source, so a SECOND transfer is a transfer from
    // a borrow — one error covers both shapes (transfer-demotes-to-borrow §1.3).
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_MOVE_OF_BORROW");
    EXPECT_NE(msg.find("v"), std::string::npos) << msg;
}

// 2.1.2 corollary — a plain READ after a call-arg transfer is also
// use-after-move (today it compiles and reads a deactivated value).
TEST(LocalLinearityTests, readAfterCallArgTransferIsLegal) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s1 = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        s1.take(#v);\n"
        "        return v.n;\n"           // demoted to a borrow — readable
        "    }\n"
        "}\n";
    compileExpectOk(src);
}

// 2.1.2 — ctor-arg transfers mark moved too (`heap Holder(#v)`).
TEST(LocalLinearityTests, readAfterCtorArgTransferIsLegal) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell held;\n"
        "    public Holder(#Cell c) { this.held = c; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell v = heap Cell(5);\n"
        "        Holder h = heap Holder(#v);\n"
        "        return v.n;\n"           // demoted to a borrow — readable
        "    }\n"
        "}\n";
    compileExpectOk(src);
}

// 2.1.3 — legal same-scope move chain: exactly one drop at scope exit
// (liveCount delta 0), the final holder readable.
TEST(LocalLinearityTests, legalMoveChainSingleDrop) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell obj = heap Cell(42);\n"
        "        Cell obj3 #= obj;\n"
        "        Cell obj4 #= obj3;\n"
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
TEST(LocalLinearityTests, readOfMovedChainLinkIsLegal) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell obj = heap Cell(42);\n"
        "        Cell obj3 #= obj;\n"
        "        return obj.n;\n"         // demoted above — still readable
        "    }\n"
        "}\n";
    compileExpectOk(src);
}

// 2.1.3 — borrows are move-transparent: a borrow taken BEFORE the owner
// moved stays readable (the deferred §7.4 hazard, deliberately legal).
TEST(LocalLinearityTests, borrowRemainsReadableAcrossOwnerMove) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell obj = heap Cell(9);\n"
        "        Cell ref = obj;\n"       // borrow
        "        Cell obj3 #= obj;\n"     // owner moves; borrow untouched
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
TEST(LocalLinearityTests, branchJoinMovedIsLegal) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run(int32 flag) {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        if (flag > 0) {\n"
        "            s.take(#v);\n"
        "        }\n"
        "        return v.n;\n"           // demoted on one path — readable
        "    }\n"
        "}\n";
    compileExpectOk(src);
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
        "            keep #= t;\n"
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

// --- double sharp: `x #= #y` is an error ---------------------------------------
//
// `#=` IS the transfer — the store site carries it (language-ownership: "a
// store uses `#=`; everything else uses `#v`"). Writing `#` on the RHS as well
// says "transfer" twice and does nothing the single sharp did not already do.
// It is TECHNICALLY VALID — `#=` already carries the source's mode, so the
// second `#` restates it rather than asking for anything different — which is
// why it WARNS (CAJETA_WARN_REDUNDANT_TRANSFER) rather than failing the build.
// The code must still compile and behave identically to the single sharp.

TEST(LocalLinearityTests, doubleSharpStoreWarnsAndCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(3);\n"
        "        Cell b #= #a;\n"           // double sharp — redundant, not wrong
        "        return b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
    EXPECT_TRUE(CajetaJit::sawDiagnostic("CAJETA_WARN_REDUNDANT_TRANSFER"));
}

// The same on a FIELD store — the shape the stdlib actually used.
TEST(LocalLinearityTests, doubleSharpFieldStoreWarnsAndCompiles) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell held;\n"
        "    public void put(Cell c) { this.held #= #c; }\n"   // double sharp
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    EXPECT_TRUE(CajetaJit::sawDiagnostic("CAJETA_WARN_REDUNDANT_TRANSFER"));
}

// The rule is BLANKET in the same way it always was — `#` on the RHS of `#=`
// restates the store whatever the source's shape — but it is a WARNING, and a
// SLOT source additionally forwards that slot's mode exactly as the single
// sharp does.
//
// It used to be narrowed to bare-identifier sources so that a FIELD or ELEMENT
// source could keep the "fused claim" — a double sharp that forwarded whatever
// mode the source slot held, owned or borrowed, VERBATIM. That existed for one
// reason: a container slot MIGHT hold a borrow, so a store out of one could not
// assert an unconditional transfer. Spec 2.3 removes the premise — every
// container owns its elements — and Unit 2 collapsed all 20 stdlib fused claims
// to single moves accordingly (`grep '#= #' runtime/src` is now 0).
//
// So the narrowing no longer buys anything, and the language gets one rule:
// the STORE carries the transfer, and it is spelled exactly once.
//
// Array-element → array-element stores keep forwarding the source bit verbatim
// under the SINGLE sharp (BinaryOpExpression's fwdLhs/fwdSrc arm does not
// require the double), so `dst[i] #= src[j]` is the shift/sift primitive it
// always was. Nothing is lost here except a second spelling.

// 3.1.1 — a FIELD source. This test previously asserted the opposite; it is
// inverted rather than deleted so the history shows the rule changing.
TEST(LocalLinearityTests, doubleSharpFromFieldWarnsAndForwards) {
    std::string src = std::string(kCellSrc) +
        "public class Node2 {\n"
        "    public Cell value;\n"
        "    public Node2(Cell v) { this.value #= v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Node2 n = heap Node2(heap Cell(4));\n"
        "        Cell t #= #n.value;\n"     // redundant sharp over a MEMBER slot
        "        return t.n;\n"
        "    }\n"
        "}\n";
    // Forwards the member's mode exactly as `#= n.value` does, and warns.
    EXPECT_EQ(runI32(src), 4);
    EXPECT_TRUE(CajetaJit::sawDiagnostic("CAJETA_WARN_REDUNDANT_TRANSFER"));
}

// 3.1.5 (field half) — the origin guard. The SAME fixture with the sharp
// dropped must compile clean. Without this, a DOUBLE_TRANSFER raised anywhere
// else in the compile (the stdlib, a fixture typo) would read as a pass: §6
// records three tests that once went green for exactly that reason. The pair
// makes the offending line the only difference between red and green.
TEST(LocalLinearityTests, doubleSharpFromFieldSingleSharpControlCompiles) {
    std::string src = std::string(kCellSrc) +
        "public class Node2 {\n"
        "    public Cell value;\n"
        "    public Node2(Cell v) { this.value #= v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Node2 n = heap Node2(heap Cell(4));\n"
        "        Cell t #= n.value;\n"      // the one-character difference
        "        return t.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// 3.1.2 — an ARRAY-ELEMENT source.
TEST(LocalLinearityTests, doubleSharpFromElementWarnsAndForwards) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell[] arr = heap Cell[2];\n"
        "        arr[0] #= heap Cell(5);\n"
        "        Cell t #= #arr[0];\n"      // redundant sharp over an ELEMENT slot
        "        return t.n;\n"
        "    }\n"
        "}\n";
    // Forwards the element's mode exactly as `#= arr[0]` does, and warns.
    EXPECT_EQ(runI32(src), 5);
    EXPECT_TRUE(CajetaJit::sawDiagnostic("CAJETA_WARN_REDUNDANT_TRANSFER"));
}

// 3.1.5 (element half) — the same fixture minus the sharp.
TEST(LocalLinearityTests, doubleSharpFromElementSingleSharpControlCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell[] arr = heap Cell[2];\n"
        "        arr[0] #= heap Cell(5);\n"
        "        Cell t #= arr[0];\n"
        "        return t.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 3.1.3 — a CALL-RESULT source. `make()` already hands back a title (`#Cell`),
// so `#make()` claims a title that is already the caller's.
TEST(LocalLinearityTests, doubleSharpFromCallResultWarnsAndCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell make() { return heap Cell(6); }\n"
        "    public static int32 run() {\n"
        "        Cell t #= #make();\n"      // redundant sharp over a `#R` result
        "        return t.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
    EXPECT_TRUE(CajetaJit::sawDiagnostic("CAJETA_WARN_REDUNDANT_TRANSFER"));
}

// 3.1.5 (call half) — the same fixture minus the sharp.
TEST(LocalLinearityTests, doubleSharpFromCallResultSingleSharpControlCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static #Cell make() { return heap Cell(6); }\n"
        "    public static int32 run() {\n"
        "        Cell t #= make();\n"
        "        return t.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 3.1.2 (store half) — an element STORE whose source is an element. This is
// the shift/sift primitive, and it must keep forwarding the source bit under
// the single sharp: slot 0 is owned, so slot 1 ends up owning it and slot 0
// decays. One live Cell at scope exit, not two, and no double free.
TEST(LocalLinearityTests, elementToElementStoreForwardsUnderSingleSharp) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell[] arr = heap Cell[2];\n"
        "            arr[0] #= heap Cell(7);\n"
        "            arr[1] #= arr[0];\n"   // forward the title, no double sharp
        "            t = arr[1].n;\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// The CORRECT spelling keeps working — the store carries the transfer.
TEST(LocalLinearityTests, singleSharpStoreStillCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(3);\n"
        "        Cell b #= a;\n"
        "        return b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// And the LEGACY `dst = #v` spelling is untouched — it is deprecated, not an
// error, and rejecting it here would be a different (breaking) decision.
TEST(LocalLinearityTests, legacyPlainAssignWithSharpRhsStillCompiles) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(5);\n"
        "        Cell b = #a;\n"
        "        return b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
