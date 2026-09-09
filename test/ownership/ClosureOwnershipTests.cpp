//
// ownership-title-classifier Unit 9 (spec 5.13, 5.14) — closures are titled
// like class values; a store of a borrow of the slot's own value keeps the
// slot's title.
//
// Measured before the unit (main and the branch alike): a capturing lambda
// passed straight as an argument leaked its record and capture block (two
// live objects per call) because a function-typed argument never rode the
// transfer word and a function-typed formal never got a drop entry; a
// field self-store through a borrow released the value and stored a borrow
// of freed memory; an element self-store cleared the slot's bit and leaked
// (ParallelDriver.foldWorker's `partials[slot] = acc`).
//
// Each verdict program returns 0 on pass; `Cajeta.liveCount()` is balanced
// over 8 calls when every value is dropped exactly once.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.collection.Collector;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static int32 takeFn((int32) -> int32 f) { return f(1); }\n"   // a transient callee
    "    static class K {\n"                                            // a keeper: the sink model
    "        (int32) -> int32 f;\n"
    "        public K((int32) -> int32 f) { this.f #= f; }\n"
    "        public int32 call(int32 x) { return this.f(x); }\n"
    "    }\n";

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

std::string loop(const std::string& members, const std::string& probeBody,
                 const std::string& want, int failBase) {
    return std::string(PRE) + members
        + "    static int32 probe() {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        A.probe(); A.probe();\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 0;\n"
        + "        while (i < 8) {\n"
        + "            if (A.probe() != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

std::string compileExpectError(const std::string& src, const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.A");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode) << "wrong diagnostic: " << e.getMessage();
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

} // namespace

// 9.1.1 — a capturing literal to a transient callee: the title moves in on
// the transfer word and the callee's formal drops it on exit.
TEST(ClosureOwnershipTests, lambdaLiteralToATransientFormalIsDroppedByTheCallee) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        return A.takeFn((x) -> x + c.n);\n", "4", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = the closure (record + captures) leaked or was freed twice";
}

// 9.1.2 — the capture block is one allocation whatever it holds.
TEST(ClosureOwnershipTests, lambdaLiteralWithTwoCapturesIsDroppedOnce) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        Cell d = heap Cell(4);\n"
        "        return A.takeFn((x) -> x + c.n + d.n);\n", "8", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = the closure leaked or was freed twice";
}

// 9.1.3 — a closure local lends: the callee arms nothing, the local still
// drops it, and it is callable after the call. (A control.)
TEST(ClosureOwnershipTests, closureLocalLentToAFormalStaysTheCallers) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 f = (x) -> x + c.n;\n"
        "        int32 a = A.takeFn(f);\n"
        "        return a + f(10);\n", "17", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value (the callee freed the caller's closure); 31 = leaked or freed twice";
}

// 9.1.4 — a keeper: `this.f #= f` records the arriving title and the
// holder's drop releases the closure with the object.
TEST(ClosureOwnershipTests, lambdaLiteralKeptWithASharpStoreIsDroppedWithTheHolder) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        K k = heap K((x) -> x + c.n);\n"
        "        return k.call(1);\n", "4", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the kept closure leaked or was freed twice";
}

// 9.1.5 — a plain `=` of a function-typed parameter into a field is the
// captured borrow it is for any class parameter.
TEST(ClosureOwnershipTests, plainStoreOfAFunctionParameterIsACapturedBorrow) {
    std::string src = std::string(PRE)
        + "    static class KPlain {\n"
        + "        (int32) -> int32 f;\n"
        + "        public KPlain((int32) -> int32 f) { this.f = f; }\n"
        + "    }\n"
        + "    public static int32 run() {\n"
        + "        KPlain k = heap KPlain((x) -> x + 1);\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
    std::string msg = compileExpectError(src, "CAJETA_ERROR_CAPTURED_BORROW_PARAM");
    EXPECT_NE(msg.find("#="), std::string::npos) << "the fix names the sink spelling: " << msg;
}

// 9.1.6 — a closure local kept through `#=` records the borrow: the local
// drops it once, the holder releases nothing, both can still call it.
TEST(ClosureOwnershipTests, closureLocalKeptThroughASharpStoreRecordsTheBorrow) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 p = (x) -> x + c.n;\n"
        "        K k = heap K(p);\n"
        "        return k.call(1) + p(1);\n", "8", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = wrong value (the holder freed the local's closure); 61 = leaked or freed twice";
}

// 9.1.7 — `#p` moves the local's title into the keeper.
TEST(ClosureOwnershipTests, sharpMoveOfAClosureLocalIntoAKeeperTransfers) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 p = (x) -> x + c.n;\n"
        "        K k = heap K(#p);\n"
        "        return k.call(2);\n", "5", 70);
    EXPECT_EQ(runVerdict(src), 0) << "70 = wrong value; 71 = the moved closure leaked or was freed twice";
}

// 9.1.8 — spec 5.14 at a class field: storing a borrow of the field's own
// value over it changes no hands; the field keeps its title.
TEST(ClosureOwnershipTests, fieldSelfStoreThroughABorrowKeepsTheTitle) {
    std::string src = loop(
        "    static class H {\n"
        "        public Cell f;\n"
        "        public H() { this.f = heap Cell(9); }\n"
        "    }\n",
        "        H h = heap H();\n"
        "        Cell b = h.f;\n"
        "        h.f = b;\n"                      // the same object, as a borrow
        "        return h.f.n;\n", "9", 80);
    EXPECT_EQ(runVerdict(src), 0) << "80 = wrong value (the field released its own value first — a UAF); 81 = leaked or freed twice";
}

// 9.1.9 — spec 5.14 at an element slot (foldWorker's shape).
TEST(ClosureOwnershipTests, slotSelfStoreThroughABorrowKeepsTheTitle) {
    std::string src = loop("",
        "        Cell[] s = heap Cell[1];\n"
        "        s[0] = heap Cell(5);\n"
        "        Cell b = s[0];\n"
        "        s[0] = b;\n"                     // the same object, as a borrow
        "        return s[0].n;\n", "5", 90);
    EXPECT_EQ(runVerdict(src), 0) << "90 = wrong value; 91 = the slot lost its title and leaked (or freed twice)";
}

// 9.1.10 — the parallel collect with an accumulator that returns a NEW list
// each step: the worker's `partials[slot] #= acc` moves the fresh title into
// the slot (and the identity case keeps the slot's own). The merged value is
// the witness; the chain root's reclaim is out of this unit's scope, so the
// live count is not asserted here.
TEST(ClosureOwnershipTests, parallelCollectWithAFreshAccumulatorIsRight) {
    std::string src = std::string(PRE)
        + "    public static int32 run() {\n"
        + "        int32[] xs = heap int32[64];\n"
        + "        int32 i = 0;\n"
        + "        while (i < 64) { xs[i] = i + 1; i = i + 1; }\n"
        + "        Collector<int32, ArrayList<int32>> c #= heap Collector<int32, ArrayList<int32>>(\n"
        + "            () -> heap ArrayList<int32>(),\n"
        + "            (acc, x) -> { ArrayList<int32> n = heap ArrayList<int32>(); n.appendAll(acc); n.add(x); return n; },\n"
        + "            (l, r) -> { l.appendAll(r); return l; });\n"
        + "        ArrayList<int32> out = xs.stream().parallel().collect(c);\n"
        + "        int32 total = 0;\n"
        + "        i = 0;\n"
        + "        while (i < out.count()) { total = total + out.get(i); i = i + 1; }\n"
        + "        return total;\n"
        + "    }\n"
        + "}\n";
    EXPECT_EQ(runVerdict(src), 2080) << "the merged partials must be 1..64 exactly once each";
}

// 9.1.11 — a factory hands its titled function-typed formal on with `#fn`,
// as a class formal is forwarded: the keeper takes the title that arrived
// (a literal's), the factory's own entry is deactivated by the move. Passed
// on PLAINLY the keeper records a borrow and the factory's exit frees the
// record under it — cabra's WebFront crashed on exactly that through
// `Middleware.of(fn) { return heap Middleware(fn); }` (measured 2026-09-09:
// the freed record's memory was reused by the next capture block, so the
// handler jumped to a Middleware object). Both a `#`-declared and a plain
// formal forward the same way.
TEST(ClosureOwnershipTests, titledFormalForwardedWithSharpIntoAKeeperIsOwnedOnce) {
    std::string src = loop(
        "    static #K ofSharp(#(int32) -> int32 fn) { return heap K(#fn); }\n"
        "    static #K ofPlain((int32) -> int32 fn) { return heap K(#fn); }\n",
        "        Cell c = heap Cell(3);\n"
        "        K a #= A.ofSharp((x) -> x + c.n);\n"
        "        K b #= A.ofPlain((x) -> x + c.n);\n"
        "        Cell pad = heap Cell(9);\n"              // reuses a freed record if one was freed
        "        return a.call(1) + b.call(2);\n", "9", 120);
    EXPECT_EQ(runVerdict(src), 0) << "120 = a keeper read a freed record; 121 = a closure leaked or was freed twice";
}

// 9.1.12 (spec 5.14, the edge 9.2.6 recorded) — a parallel reduce whose
// combiner hands back its RIGHT input after fresh-producing steps. The
// workers' fresh partials are owned by the partials array; the merge's
// `acc = fn(acc, partials[ci])` returned a BORROW of a slot's value, `return
// acc` rode that borrow out, and the chain's frame freed the array under the
// caller. The merge now moves each partial into the combiner
// (`fn(acc, #partials[ci])`): returning `b` hands its title out, returning a
// fresh value drops it. `pad` reuses a freed cell so a dangling `best`
// reads a wrong value rather than a stale right one.
TEST(ClosureOwnershipTests, parallelReduceCombinerReturningItsRightInputOwnsTheResult) {
    // The chain root (`xs.stream()` under a class-returning terminal) is a
    // known bounded leak (6.2.5), so the live count is not asserted here:
    // the witness is the VALUE. Thirty-two sentinel cells allocated after
    // the reduce reuse a freed cell, so a dangling `best` reads a sentinel.
    std::string src = std::string(PRE)
        + "    static #Cell keepMax(Cell a, Cell b) {\n"
        + "        if (b.n > a.n) { return b; }\n"           // its right input: a borrow under #Cell
        + "        return heap Cell(a.n);\n"                 // fresh: owned
        + "    }\n"
        + "    static int32 probe() {\n"
        + "        Cell[] xs = heap Cell[256];\n"
        + "        int32 i = 0;\n"
        + "        while (i < 256) { xs[i] = heap Cell((i * 37) % 200); i = i + 1; }\n"
        + "        xs[0] = heap Cell(250);\n"    // the max FIRST: the tail is the array's front after the back-half splits,
        + "        xs[1] = heap Cell(0);\n"      // and its last step copies the max fresh; that partial merges last and wins
        + "        Cell seed = heap Cell(-1);\n"
        + "        Cell best #= xs.stream().parallel().reduce(seed, (a, b) -> A.keepMax(a, b));\n"
        + "        Cell[] pads = heap Cell[32];\n"
        + "        int32 k = 0;\n"
        + "        while (k < 32) { pads[k] = heap Cell(-100 - k); k = k + 1; }\n"
        + "        return best.n;\n"
        + "    }\n"
        + "    public static int32 run() {\n"
        + "        int32 i = 0;\n"
        + "        while (i < 8) {\n"
        + "            if (A.probe() != 250) { return 130; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
    EXPECT_EQ(runVerdict(src), 0) << "130 = best read a freed (reused) cell";
}

// 9.1.13 (spec 5.14, the 8.2.2 flow gap at run time) — a borrow re-assign
// leaves the local's entry registered on the DISPLACED value (by design:
// `n = n.next` keeps reading through it). A reader then took that entry's
// flag as the local's title: `#k` handed a callee a title on an object the
// frame never owned, and the callee freed it under its real owner. The
// readers now count the flag only while the entry describes the local's
// current object; the displaced value is still freed at scope exit.
TEST(ClosureOwnershipTests, staleEntryAfterABorrowReassignLendsNoTitle) {
    std::string src = loop(
        "    static #Cell pick(Cell a, Cell b) { return b; }\n"     // hands back its right input: a borrow at run time
        "    static int32 consume(#Cell c) { return c.n; }\n",      // owns what it is handed
        "        Cell k = heap Cell(1);\n"                            // k owns cell 1
        "        Cell o = heap Cell(5);\n"                            // o owns cell 5
        "        k #= A.pick(k, o);\n"                                // a borrow arrives at run time (flag 0): the entry stays on cell 1
        "        int32 v = A.consume(#k);\n"                          // forwards k's MODE on cell 5: none — a lend
        "        Cell pad = heap Cell(-7);\n"                         // would reuse a freed cell 5
        "        return v + o.n;\n", "10", 140);
    EXPECT_EQ(runVerdict(src), 0) << "140 = the callee freed o under its owner (a forged title); 141 = leaked or freed twice";
}
