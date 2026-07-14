//
// title-tracking Unit 5 (plan 5.1) — signature transfer ABI, spec §4 REV 2
// (caller discretion). Signatures carry no ownership spelling: every
// class-typed formal/return rides a hidden per-call flag. Plain arg lends
// (flag false); `#x` / owned rvalue surrenders (flag true). Formals are
// runtime owners: `#v` store/forward consumes; an unconsumed flag-true
// formal drops in the callee. `#V` survives as the opt-in must-own edge
// (sub-fork A). The single-hop dangling-lend check is sub-fork B.
// 5.1.11 (last-use advisory) is NOT probed here — warning capture lands
// with 5.2.8.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCellSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
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

// 5.1.1 — plain arg is a lend: the callee reads, the source survives the
// call, keeps its single drop. Regression pin on post-3A semantics.
TEST(SignatureAbiTests, plainArgLends) {
    std::string src = std::string(kCellSrc) +
        "public class Viewer {\n"
        "    public int32 look(Cell c) { return c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell v = heap Cell(3);\n"
        "        Viewer w = heap Viewer();\n"
        "        int32 t = w.look(v);\n"
        "        return t + v.n;\n"          // v survives, still readable
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 5.1.2 — `#x` into an UNANNOTATED formal surrenders the title: the
// source is statically moved and a later read is rejected, naming the
// transfer.
TEST(SignatureAbiTests, sharpArgMarksSourceMoved) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 seen;\n"
        "    public void take(Cell c) { this.seen = c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s = heap Sink();\n"
        "        Cell v = heap Cell(5);\n"
        "        s.take(#v);\n"              // surrender at a plain edge
        "        return v.n;\n"              // read of moved local — reject
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_USE_AFTER_MOVE");
    EXPECT_NE(msg.find("v"), std::string::npos) << msg;
}

// 5.1.3a — `#v` store of a formal forwards the CALL's flag into the field
// bit. Owned spelling at the call site → field owns → Box teardown drops.
TEST(SignatureAbiTests, ownedSpellingStoreFieldOwns) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void put(Cell v) { this.c = #v; }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Box b = heap Box();\n"
        "        b.put(#heap Cell(4));\n"    // flag true → field bit owned
        "        return b.peek();\n"
        "    }\n"                            // Box teardown drops the Cell
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// 5.1.3b — same put, plain spelling: field records a borrow, Box teardown
// skips it, the source outlives the Box with its single drop intact.
TEST(SignatureAbiTests, plainSpellingStoreFieldBorrows) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void put(Cell v) { this.c = #v; }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            Box b = heap Box();\n"
        "            b.put(mine);\n"         // flag false → field bit borrow
        "            if (b.peek() != 9) { return -98; }\n"
        "        }\n"                        // Box drops; must NOT free mine
        "        return mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}

// 5.1.4a — the paired return flag arms the caller's drop entry: a
// surrendered value forwarded back out is owned by the receiving local.
// (The container put/remove round-trip probes land with 6.1 — this pins
// the ABI mechanism the container will ride.)
TEST(SignatureAbiTests, flaggedReturnArmsCallerDrop) {
    std::string src = std::string(kCellSrc) +
        "public class Echo {\n"
        "    public Cell pass(Cell v) { return #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Echo e = heap Echo();\n"
        "        Cell r = e.pass(#heap Cell(6));\n"  // flag true → r owns
        "        return r.n;\n"
        "    }\n"                                    // r drops here
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 5.1.4b — flag false on the same path: the receiving local stays a
// borrow; the original owner keeps the single drop and survives.
TEST(SignatureAbiTests, flaggedReturnLeavesBorrowerInactive) {
    std::string src = std::string(kCellSrc) +
        "public class Echo {\n"
        "    public Cell pass(Cell v) { return #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(7);\n"
        "        Echo e = heap Echo();\n"
        "        {\n"
        "            Cell r = e.pass(mine);\n"  // flag false → r borrows
        "            if (r.n != 7) { return -97; }\n"
        "        }\n"                           // r's exit must NOT free mine
        "        return mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 5.1.5 — the `#V` opt-in must-own edge (sub-fork A): a plain borrow
// argument is rejected at compile time.
TEST(SignatureAbiTests, mustOwnEdgeRejectsPlainArg) {
    std::string src = std::string(kCellSrc) +
        "public class Registry {\n"
        "    public int32 seen;\n"
        "    public void adopt(#Cell c) { this.seen = c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Registry r = heap Registry();\n"
        "        Cell v = heap Cell(3);\n"
        "        r.adopt(v);\n"           // borrow into must-own — reject
        "        return r.seen;\n"
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_TRANSFER_REQUIRED");
    EXPECT_NE(msg.find("adopt"), std::string::npos) << msg;
}

// 5.1.6 — a two-deep forwarding chain of unannotated formals threads the
// flag both ways: the owned path drops once at the final place, the lent
// path leaves the source alive. The moveMask failure case, pinned.
TEST(SignatureAbiTests, forwardChainThreadsFlag) {
    std::string src = std::string(kCellSrc) +
        "public class Box {\n"
        "    public Cell c;\n"
        "    public void inner(Cell v) { this.c = #v; }\n"
        "    public void outer(Cell v) { this.inner(#v); }\n"
        "    public int32 peek() { return this.c.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Box b = heap Box();\n"
        "            b.outer(#heap Cell(3));\n"     // true threads through
        "            t = t + b.peek();\n"
        "        }\n"                               // owned → Box drops it
        "        Cell mine = heap Cell(4);\n"
        "        {\n"
        "            Box b2 = heap Box();\n"
        "            b2.outer(mine);\n"             // false threads through
        "            if (b2.peek() != 4) { return -96; }\n"
        "        }\n"                               // borrow → mine survives
        "        return t + mine.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 5.1.7 (drives 5.2.5) — a flag-true formal the body neither stores nor
// forwards is CONSUMED: the callee drops it. No Cajeta.dropValue (the
// ~HashMap idiom this retires). Red today — it leaks.
TEST(SignatureAbiTests, consumedFormalDropsInCallee) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 seen;\n"
        "    public void take(Cell c) {\n"
        "        this.seen = this.seen + c.n;\n"
        "    }\n"                            // flag true + unconsumed → drop
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Sink s = heap Sink();\n"
        "        s.take(#heap Cell(2));\n"
        "        s.take(#heap Cell(3));\n"
        "        Cell lent = heap Cell(10);\n"
        "        s.take(lent);\n"            // flag false → callee must NOT drop
        "        return s.seen + lent.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 25);
}

// 5.1.8 — same-name declarations differing only in transfer mode are
// rejected at declaration (dispatch is mode-erased).
TEST(SignatureAbiTests, modeOnlyOverloadRejectedAtDeclaration) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public void f(Cell c) { }\n"
        "    public void f(#Cell c) { }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_TRANSFER_MODE_OVERLOAD");
    EXPECT_NE(msg.find("f"), std::string::npos) << msg;
}

// 5.1.9 — regression pin: a plain return of a statically-owned local is
// still rejected (the local drops at exit; the caller would receive an
// immediately-dangling borrow). Fix spelling is `return #c`.
TEST(SignatureAbiTests, freshReturnStillNeedsTransfer) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell make() {\n"
        "        Cell c = heap Cell(1);\n"
        "        return c;\n"                // owner returned as borrow — reject
        "    }\n"
        "    public static int32 run() { return make().n; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER");
}

// 5.1.10a (drives 5.2.7, sub-fork B) — single-hop dangling lend, direct
// store shape: a local object holding a lend of a dying local escapes.
TEST(SignatureAbiTests, danglingLendOnDirectStoreRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell c;\n"
        "}\n"
        "public final class D {\n"
        "    public static Holder build() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell s = heap Cell(5);\n"
        "        h.c = s;\n"                 // lend of local s into h
        "        return #h;\n"               // h escapes; s dies here — reject
        "    }\n"
        "    public static int32 run() { return build().c.n; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_DANGLING_LEND");
    EXPECT_NE(msg.find("s"), std::string::npos) << msg;
}

// 5.1.10b — same hazard through a one-hop setter call (`m.put(k, s)`
// shape from spec §7.4).
TEST(SignatureAbiTests, danglingLendOnSetterLendRejected) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell c;\n"
        "    public void keep(Cell v) { this.c = #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static Holder build() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell s = heap Cell(5);\n"
        "        h.keep(s);\n"               // lend of local s into h
        "        return #h;\n"               // h escapes; s dies — reject
        "    }\n"
        "    public static int32 run() { return build().c.n; }\n"
        "}\n";
    compileExpectError(src, "CAJETA_ERROR_DANGLING_LEND");
}

// 5.1.10c — negative probes: the `#s` spelling suppresses the lint (the
// entry owns), and a lend into a NON-escaping holder is fine.
TEST(SignatureAbiTests, lendNegativeProbesCompileAndRun) {
    std::string src = std::string(kCellSrc) +
        "public class Holder {\n"
        "    public Cell c;\n"
        "    public void keep(Cell v) { this.c = #v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static Holder build() {\n"
        "        Holder h = heap Holder();\n"
        "        Cell s = heap Cell(5);\n"
        "        h.keep(#s);\n"              // surrendered — h owns, no lint
        "        return #h;\n"
        "    }\n"
        "    public static int32 local() {\n"
        "        Cell s = heap Cell(6);\n"
        "        Holder h = heap Holder();\n"
        "        h.keep(s);\n"               // lend, but h never escapes
        "        return h.c.n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Holder got = build();\n"
        "            t = got.c.n + local();\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// ---------------------------------------------------------------------------
// 5.2.5 — the callee-side drop of an unconsumed flag-true formal must fire on
// EVERY exit, not just the fall-through one 5.1.7 covers. The drop entries are
// registered on the method's drop frames, so returns run them via
// emitOwnerDrops; a THROW does not — it longjmps and the runtime unwinder walks
// the chain instead. That path is what these probe.
// ---------------------------------------------------------------------------

// 5.2.5a — early return: a consumed formal drops exactly once, on the path
// actually taken (and the lent arg on the other path still doesn't drop).
TEST(SignatureAbiTests, consumedFormalDropsOnEarlyReturn) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 take(Cell v, boolean early) {\n"
        "        if (early) { return v.n; }\n"   // consumed, early exit
        "        return v.n + 100;\n"            // consumed, late exit
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Sink s = heap Sink();\n"
        "        int32 a = s.take(#heap Cell(1), true);\n"   // drops at early ret
        "        int32 b = s.take(#heap Cell(2), false);\n"  // drops at late ret
        "        return a + b;\n"                            // 1 + 102 = 103
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 103);
}

// 5.2.5b — THROW path: the callee takes the title, then throws before consuming
// it. The value must still be reclaimed — a throw does NOT run emitOwnerDrops;
// it longjmps and the runtime unwinder walks the drop chain, which the formal's
// entry is on. The oracle is DIFFERENTIAL: a thrown-and-caught Exception object
// itself leaks today (pre-existing, unrelated to titles — see plan 5.2.5), so an
// absolute leak count would measure that bug instead of this one. Comparing the
// owned-arg call against a throw-only control cancels it out.
TEST(SignatureAbiTests, consumedFormalDropsWhenCalleeThrows) {
    std::string src = std::string(kCellSrc) +
        "public class Sink {\n"
        "    public int32 take(Cell v) { throw heap Exception(\"boom\"); }\n"
        "    public int32 plain() { throw heap Exception(\"boom\"); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s = heap Sink();\n"
        "        int64 b1 = Cajeta.liveCount();\n"
        "        try { s.plain(); } catch (Exception e) { }\n"
        "        int64 ctrl = Cajeta.liveCount() - b1;\n"      // the Exception leak
        "        int64 b2 = Cajeta.liveCount();\n"
        "        try { s.take(#heap Cell(4)); } catch (Exception e) { }\n"
        "        int64 owned = Cajeta.liveCount() - b2;\n"     // must equal ctrl
        "        if (owned != ctrl) { return -1; }\n"          // Cell NOT reclaimed
        "        Cell mine = heap Cell(5);\n"
        "        try { s.take(mine); } catch (Exception e) { }\n"
        "        return mine.n;\n"                             // lent: must survive
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 5.2.5c — mixed flags in one call: bit i of the transfer word must select
// per-formal, so an owned arg drops in the callee while a lent one beside it
// survives.
TEST(SignatureAbiTests, mixedFlagFormalsDropIndependently) {
    std::string src = std::string(kCellSrc) +
        "public class Pair {\n"
        "    public int32 both(Cell a, Cell b) { return a.n + b.n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Pair p = heap Pair();\n"
        "        Cell keep = heap Cell(10);\n"
        "        int32 t = p.both(#heap Cell(3), keep);\n"  // a owned, b lent
        "        return t + keep.n;\n"                      // 13 + 10 = 23
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 23);
}

// 5.1.11 (spec §7.1) — LAST-USE ADVISORY. A plain arg at a local's final use
// emits a WARNING with a `#` fixit (never an error — the build stays green;
// inference is rejected, spec §4.6.4). A later read of the local suppresses it,
// and so does spelling `#v`. Warning capture is the harness support added with
// 5.2.8: the JIT installs a DiagnosticEngine and parks what it collected.
namespace {

// Did the last compile warn about this specific variable? Filtering by name
// keeps the assertion immune to any advisory the stdlib itself trips.
bool warnedAbout(const char* varName) {
    for (auto& d : CajetaJit::lastDiagnostics()) {
        if (d.code == "CAJETA_WARN_LAST_USE_TRANSFER"
                && d.severity == "warning"
                && d.message.find(std::string("`") + varName + "`")
                       != std::string::npos) {
            return true;
        }
    }
    return false;
}

const char* kAdvisorySrc =
    "public class Sink {\n"
    "    public Cell held;\n"
    "    public void keep(Cell v) { this.held = #v; }\n"
    "    public int32 peek(Cell v) { return v.n; }\n"
    "}\n";

}  // namespace

TEST(SignatureAbiTests, lastUseOfLentLocalWarnsWithTransferFixit) {
    std::string src = std::string(kCellSrc) + kAdvisorySrc +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Sink s = heap Sink();\n"
        "        Cell doomed = heap Cell(1);\n"
        "        s.keep(doomed);\n"      // final use, lent -> ADVISE
        "        Cell reread = heap Cell(2);\n"
        "        s.peek(reread);\n"      // lent, but read again below -> quiet
        "        Cell moved = heap Cell(3);\n"
        "        s.keep(#moved);\n"      // spelled -> quiet
        "        return reread.n;\n"     // the later read that suppresses
        "    }\n"
        "}\n";
    // The advisory is a WARNING: the compile must still succeed and run.
    EXPECT_EQ(runI32(src), 2);
    EXPECT_TRUE(warnedAbout("doomed"));
    EXPECT_FALSE(warnedAbout("reread"));
    EXPECT_FALSE(warnedAbout("moved"));
}

// 6.2.6c — a consumed formal returned THROUGH A CAST is the same
// pass-through as `return formal;`: the title rides out on the return
// flag and the callee epilogue must NOT drop it. The disarm used to
// match only a bare IdentifierExpression, so Stream.cloneChainOver's
// `return (Stream<T>) newRoot;` freed every parallel share and the
// spawned workers dispatched next() on dead streams (the whole
// ParallelStreamP1 crash family).
TEST(SignatureAbiTests, castReturnOfConsumedFormalPassesTitleThrough) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell pass(#Object o) {\n"
        "        return (Cell) o;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cell c = heap Cell(7);\n"
        "        Cell d = pass(#c);\n"
        "        // churn so a freed `d` gets recycled and misreads\n"
        "        int32 junk = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            Cell t = heap Cell(9);\n"
        "            junk = junk + t.n;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return d.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Same shape with a caller-discretion plain formal: `#c` at the call
// site arms the formal's entry; the cast return must still disarm it
// and thread the flag out.
TEST(SignatureAbiTests, castReturnOfArmedPlainFormalPassesTitleThrough) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static Cell pass(Object o) {\n"
        "        return (Cell) o;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Cell c = heap Cell(5);\n"
        "        Cell d = pass(#c);\n"
        "        int32 junk = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            Cell t = heap Cell(9);\n"
        "            junk = junk + t.n;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return d.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
