//
// Re-assignment of a local that owns (or may own) its value.
//
// Measured 2026-09-07 (tmp/probe-emit X.cajeta / W.cajeta): a binding with a
// drop entry re-assigned an OWNED value kept its entry on the DISPLACED value
// — `Cell k = heap Cell(1); k = heap Cell(2);` freed Cell(1) at scope exit
// and leaked Cell(2); `k = #t` retargeted the entry and orphaned Cell(1); a
// borrow-initialized local (`Cell k = h.a;`) had no entry at all, so
// `k = heap Cell(7)` leaked Cell(7). The documented "reassign-leak family".
//
// The rule these pin: assigning an OWNED value (a `#x` move, a fresh heap
// value, a `#R` call, a non-arena concat, or a runtime-flagged value whose
// flag is 1) into a binding releases the displaced value if the binding still
// held a title and re-arms the entry on the new value. Assigning a BORROW
// changes nothing about the entry: the old value stays registered until scope
// exit, so a walk like `n = n.next` over an owned head never frees the node it
// is reading through. A borrow-initialized local that some later assignment
// could arm gets an inactive entry at its declaration.
//
// Verdicts read back through the runtime: Cajeta.liveCount() balanced after a
// loop of calls (a leak moves it up, a double free moves it down or crashes),
// and the lent field's bytes intact.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runVerdict(const char* src, const char* cls) {
    auto jit = CajetaJit::compile(src, cls);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 v;\n"
    "        public Cell(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Node {\n"
    "        public int32 v;\n"
    "        public Node next;\n"
    "        public Node(int32 v) { this.v = v; return; }\n"
    "    }\n"
    "    static class Holder {\n"
    "        public Cell a;\n"
    "        public Cell b;\n"
    "        public String name;\n"
    "        public Holder() {\n"
    "            this.a #= heap Cell(1);\n"
    "            this.b #= heap Cell(2);\n"
    "            this.name #= \"hello\" + 1;\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    static #Cell fresh(int32 v) { return heap Cell(v); }\n"
    "    static Cell lend(Holder h) { return h.b; }\n"
    "    static #String mkOwned(int32 n) { return \"s\" + n; }\n"
    "    static void take(#Cell c) { return; }\n";

} // namespace

// `Cell k = heap Cell(1); k = heap Cell(2);` — the displaced Cell(1) is
// released at the assignment and Cell(2) is dropped at scope exit.
TEST(ReassignOwnershipTests, ownedLocalReassignedFreshReleasesTheDisplaced) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Cell k = heap Cell(1);\n"
        "        k = heap Cell(2);\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 2) { return 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "1 = wrong value bound; 2 = live count moved (a cell leaked or was "
           "freed twice)";
}

// `Cell k = h.a; k = heap Cell(7);` — a borrow-initialized local gets an
// inactive entry at its declaration; the assignment arms it; Cell(7) is
// dropped at scope exit; h.a is never touched.
TEST(ReassignOwnershipTests, borrowLocalReassignedFreshIsDroppedAtScopeExit) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h) {\n"
        "        Cell k = h.a;\n"
        "        k = heap Cell(7);\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h) != 7) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 4; }\n"
        "        if (Cajeta.liveCount() != l0) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "3 = wrong value bound; 4 = the lent field was freed; 5 = the "
           "fresh cells leaked";
}

// `Cell k = heap Cell(1); k = h.a;` — a borrow assigned over an owned value
// leaves the entry alone: Cell(1) is released at scope exit, h.a survives.
TEST(ReassignOwnershipTests, ownedLocalReassignedBorrowKeepsTheOldUntilScopeExit) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h) {\n"
        "        Cell k = heap Cell(1);\n"
        "        k = h.a;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h) != 1) { return 6; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 7; }\n"
        "        if (Cajeta.liveCount() != l0) { return 8; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "6 = wrong value bound; 7 = the lent field was freed; 8 = Cell(1) "
           "leaked or was freed twice";
}

// `k = #t` over an owned k releases the displaced value; t's title moves
// into k and is dropped once at scope exit.
TEST(ReassignOwnershipTests, moveAssignReleasesTheDisplaced) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Cell k = heap Cell(1);\n"
        "        Cell t = heap Cell(3);\n"
        "        k = #t;\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 3) { return 9; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 10; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "9 = wrong value bound; 10 = Cell(1) orphaned, or Cell(3) freed "
           "twice";
}

// Two `#String` results in a row: the first is released when the second
// arrives; the second at scope exit.
TEST(ReassignOwnershipTests, stringLocalReassignedOwnedReleasesTheDisplaced) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        String s #= A.mkOwned(1);\n"
        "        s #= A.mkOwned(22);\n"
        "        return s.byteLength();\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 3) { return 11; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 12; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "11 = wrong value bound; 12 = the displaced wrapper leaked";
}

// A call result follows its flag: `k #= A.fresh(5)` (owned) releases and
// re-arms; `k = A.lend(h)` (a borrow-returning plain call) leaves the entry
// on the previous owned value until scope exit.
TEST(ReassignOwnershipTests, callResultReassignFollowsTheCalleeFlag) {
    std::string src = std::string(PRE) +
        "    static int32 probeOwned() {\n"
        "        Cell k = heap Cell(1);\n"
        "        k #= A.fresh(5);\n"
        "        return k.v;\n"
        "    }\n"
        "    static int32 probeLent(Holder h) {\n"
        "        Cell k = heap Cell(1);\n"
        "        k = A.lend(h);\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probeOwned() != 5) { return 13; }\n"
        "            if (A.probeLent(h) != 2) { return 14; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.b.v != 2) { return 15; }\n"
        "        if (Cajeta.liveCount() != l0) { return 16; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "13/14 = wrong value bound; 15 = the lent field was freed; 16 = "
           "live count moved (a fresh cell leaked, or a displaced one was "
           "not released)";
}

// The binding is declared in an outer scope and assigned in an inner block:
// the entry lives in the declaring frame, so the value survives the block
// and is dropped when the binding's own scope ends.
TEST(ReassignOwnershipTests, reassignInInnerBlockKeepsTheValueUntilTheDeclaringScopeEnds) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h, boolean c) {\n"
        "        Cell k = h.a;\n"
        "        if (c) { k = heap Cell(9); }\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h, true) != 9) { return 17; }\n"
        "            if (A.probe(h, false) != 1) { return 18; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 19; }\n"
        "        if (Cajeta.liveCount() != l0) { return 20; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "17/18 = wrong value (freed inside the block?); 19 = the lent "
           "field was freed; 20 = Cell(9) leaked";
}

// `n = n.next` over an owned head: the borrow assignment must not release the
// head while it is being read through. The whole chain is dropped once, at
// scope exit.
TEST(ReassignOwnershipTests, walkingAnOwnedChainThroughItsOwnBindingIsSafe) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Node n = heap Node(1);\n"
        "        n.next #= heap Node(2);\n"
        "        n.next.next #= heap Node(3);\n"
        "        int32 sum = 0;\n"
        "        while (n != null) {\n"
        "            sum = sum + n.v;\n"
        "            n = n.next;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 6) { return 21; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 22; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "21 = the walk read a freed node; 22 = the chain leaked or was "
           "freed twice";
}

// Re-assignment inside a loop: each iteration's fresh value is released by
// the next; the last one at scope exit.
TEST(ReassignOwnershipTests, reassignInALoopReleasesEachDisplacedValue) {
    std::string src = std::string(PRE) +
        "    static int32 probe(Holder h) {\n"
        "        Cell k = h.a;\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            k = heap Cell(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe(h) != 7) { return 23; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (h.a.v != 1) { return 24; }\n"
        "        if (Cajeta.liveCount() != l0) { return 25; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "23 = wrong value; 24 = the lent field was freed; 25 = the "
           "per-iteration cells leaked";
}

// A moved-out binding re-armed by a fresh value (the shape the old rule
// already covered) still works, and `take(#k)` still owns what it took.
TEST(ReassignOwnershipTests, movedOutBindingRearmedByFreshValue) {
    std::string src = std::string(PRE) +
        "    static int32 probe() {\n"
        "        Cell k = heap Cell(1);\n"
        "        A.take(#k);\n"
        "        k = heap Cell(4);\n"
        "        return k.v;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 l0 = Cajeta.liveCount();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (A.probe() != 4) { return 26; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (Cajeta.liveCount() != l0) { return 27; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runVerdict(src.c_str(), "test.A"), 0)
        << "26 = wrong value; 27 = Cell(1) freed twice (the release ran on a "
           "moved-out entry) or Cell(4) leaked";
}
