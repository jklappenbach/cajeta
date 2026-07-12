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
