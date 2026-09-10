// ownership-title-classifier Unit 9 (spec 5.13, 5.14) — closures are titled
// like class values; a store of a borrow of the slot's own value keeps the
// slot's title. Each verdict program returns 0 on pass.

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

// 9.1.1 — a capturing literal: the callee's formal takes the title and drops it.
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

// 9.1.3 — control: a closure local lends; it stays the caller's and callable.
TEST(ClosureOwnershipTests, closureLocalLentToAFormalStaysTheCallers) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 f = (x) -> x + c.n;\n"
        "        int32 a = A.takeFn(f);\n"
        "        return a + f(10);\n", "17", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value (the callee freed the caller's closure); 31 = leaked or freed twice";
}

// 9.1.4 — a keeper: `this.f #= f` takes the title; the holder's drop releases it.
TEST(ClosureOwnershipTests, lambdaLiteralKeptWithASharpStoreIsDroppedWithTheHolder) {
    std::string src = loop("",
        "        Cell c = heap Cell(3);\n"
        "        K k = heap K((x) -> x + c.n);\n"
        "        return k.call(1);\n", "4", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = the kept closure leaked or was freed twice";
}

// 9.1.5 — a plain `=` of a function-typed parameter into a field is a captured borrow.
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

// 9.1.6 — a closure local kept through `#=` records the borrow; the holder frees nothing.
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

// 9.1.8 — spec 5.14 at a class field: a self-store through a borrow keeps the title.
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

// 9.1.10 — a parallel collect whose accumulator returns a NEW list each step:
// the worker's `partials[slot] #= acc` moves the fresh title into the slot.
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

// 9.1.11 — a factory forwarding its function-typed formal with `#fn` hands the
// title on once; a `#`-declared and a plain formal forward the same way.
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

// 9.1.12 (spec 5.14) — a parallel reduce whose combiner hands back its RIGHT
// input: the merge moves each partial in, so the result is owned. The sentinel
// cells reuse a freed one, so a dangling `best` reads a wrong value.
TEST(ClosureOwnershipTests, parallelReduceCombinerReturningItsRightInputOwnsTheResult) {
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

// 9.1.13 (spec 5.14) — after a borrow re-assign the local's drop entry stays on
// the DISPLACED value, so `#k` must forge no title on the object it now names.
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
