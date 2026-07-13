//
// title-tracking 6.2.3 (second tranche) — the rev-2 borrow/transfer contract
// (spec §6) across the NON-HashMap collections: ArrayList, Heap, HashSet,
// LinkedList, Collectors. HashMap earned this surface in 6.2.2; these pin the
// same semantics everywhere else: dual-capable stores (the call site decides,
// the entry bit records), borrow reads, `#`-extraction via operator#[] (title
// out, entry resident, panic without a title), remove-shaped calls returning
// the value in the mode the entry held (flagged), and teardown walks that drop
// exactly the owned entries.
//
// Red-first: at authoring time every container here stores its formal PLAIN,
// so an owned add (`xs.add(#x)`) leaves the formal's drop entry armed — the
// callee exit-drop frees the element the container just stored (the DnsCache
// UAF class). None has entry bits, an extraction operator, or a flagged
// remove/pop.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Value-keyed test element: @AutoHash + operator== give HashSet value
// semantics (probe with a fresh Cell after surrendering the stored one);
// operator< gives Heap its ordering.
const char* kCellSrc =
    "package test;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.collection.Heap;\n"
    "import cajeta.collection.HashSet;\n"
    "import cajeta.collection.LinkedList;\n"
    "import cajeta.collection.Collectors;\n"
    "import cajeta.lang.stream.ArrayStream;\n"
    "import cajeta.collection.RedBlackTree;\n"
    "import cajeta.collection.BPlusTree;\n"
    "@AutoHash\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "    public static boolean operator== (Cell a, Cell b) { return a.n == b.n; }\n"
    "    public static boolean operator< (Cell a, Cell b) { return a.n < b.n; }\n"
    "}\n";

// Same-shaped allocation churn: reuses a freed element's storage so a
// dangling stored pointer reads clobbered bytes instead of stale-but-intact
// ones (the OwnershipLeakProbe idiom).
const char* kChurn =
    "        int32 c = 0;\n"
    "        int32 churn = 0;\n"
    "        while (c < 64) { Cell j = heap Cell(1000 + c); churn = churn + j.n; c = c + 1; }\n"
    "        if (churn < 0) { return churn; }\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// ===================== ArrayList =====================

// `xs.add(#v)` — the LIST takes the title: the element survives the add call
// (no formal-exit UAF), reads back, and the list's teardown reclaims it.
TEST(ContainerSweepTests, arrayListOwnedAddDropsAtListTeardown) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(#heap Cell(7));\n" +
        std::string(kChurn) +
        "            t = xs.get(0).n;\n"
        "        }\n"
        "        return t;\n"
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

// `xs.add(v)` — the list borrows; the local keeps the title and teardown
// must not free it out from under the owner.
TEST(ContainerSweepTests, arrayListBorrowAddLeavesOwnerAlive) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(mine);\n"
        "            if (xs.get(0).n != 9) { return -98; }\n"
        "        }\n"
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

// One owned element, one borrowed, in the SAME list — teardown drops exactly
// the owned one.
TEST(ContainerSweepTests, arrayListMixedOwnershipDropsOnlyOwned) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell lent = heap Cell(20);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(#heap Cell(1));\n"
        "            xs.add(lent);\n"
        "            t = xs.get(0).n + xs.get(1).n;\n"   // 21
        "        }\n"
        "        return t + lent.n;\n"                   // 41
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 41);
}

// `set(i, #b)` over an owned slot — the displaced element is released at the
// store (spec §5.1.3 displaced-release), the new one is owned, teardown
// reclaims it: no leak, no double free.
TEST(ContainerSweepTests, arrayListSetDropsDisplacedOwnedElement) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(#heap Cell(3));\n"
        "            xs.set(0, #heap Cell(5));\n" +
        std::string(kChurn) +
        "            t = xs.get(0).n;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// The doubling grow() must carry the ownership bits alongside the elements —
// 24 owned adds cross the initial capacity of 16.
TEST(ContainerSweepTests, arrayListGrowCarriesOwnedBits) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            int32 i = 0;\n"
        "            while (i < 24) { xs.add(#heap Cell(i)); i = i + 1; }\n"
        "            t = xs.get(0).n + xs.get(23).n;\n"   // 0 + 23
        "        }\n"
        "        return t;\n"
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

// Subscript sugar + extraction: `xs[i] = #v` stores owned through
// operator[]=, `#xs[i]` binds operator#[] — title to the assignee, slot
// RESIDENT and readable, bit decayed so teardown skips it.
TEST(ContainerSweepTests, arrayListSubscriptStoreAndExtract) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "            xs.add(#heap Cell(1));\n"
        "            xs[0] = #heap Cell(7);\n"       // displaced-release the 1
        "            Cell got = #xs[0];\n"           // title out, slot resident
        "            if (xs[0].n != 7) { return -97; }\n"
        "            t = got.n;\n"
        "        }\n"                                // got drops; teardown skips slot
        "        return t;\n"
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

// A second `#xs[i]` finds no title — Recoverable panic, never a second owner.
TEST(ContainerSweepTests, arrayListSecondExtractionPanics) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Cell> xs = heap ArrayList<Cell>();\n"
        "        xs.add(#heap Cell(4));\n"
        "        Cell first = #xs[0];\n"
        "        try {\n"
        "            Cell second = #xs[0];\n"
        "            return -99;\n"
        "        } catch (Exception e) {\n"
        "            return first.n;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// ===================== Heap =====================

// Owned push / pop round-trip: pop hands the title back (flagged return —
// the receiving local drops it), teardown reclaims what's still queued.
// Also the class-T ordering probe: Cell's operator< drives the sift.
TEST(ContainerSweepTests, heapOwnedPushPopReclaims) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Heap<Cell> h = heap Heap<Cell>();\n"
        "            h.push(#heap Cell(5));\n"
        "            h.push(#heap Cell(1));\n"
        "            h.push(#heap Cell(3));\n"
        "            Cell lo = h.pop();\n"         // flagged: owned → lo owns
        "            t = lo.n;\n"                  // 1 (min-heap order)
        "        }\n"                              // lo drops; teardown drops 3,5
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Borrow push: the owner keeps the title through sift, pop, and teardown.
TEST(ContainerSweepTests, heapBorrowPushLeavesOwnerAlive) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            Heap<Cell> h = heap Heap<Cell>();\n"
        "            h.push(mine);\n"
        "            Cell back = h.pop();\n"       // flagged: borrowed → back lends
        "            if (back.n != 9) { return -98; }\n"
        "        }\n"
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

// ===================== HashSet =====================

// `set.add(#v)` — the SET owns the member: it survives the add (probe by
// value equality after allocator churn), and teardown reclaims it.
TEST(ContainerSweepTests, hashSetOwnedAddContainsAfterChurn) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashSet<Cell> set = heap HashSet<Cell>(16);\n"
        "            Cell probe = heap Cell(7);\n"
        "            set.add(#heap Cell(7));\n" +
        std::string(kChurn) +
        "            if (!set.contains(probe)) { return -97; }\n"
        "            t = (int32) set.count();\n"   // 1
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// remove() of an owned member reclaims it right there (the map drops its
// owned key in place) — the live count steps down AT the remove, and the
// scope still balances after teardown.
TEST(ContainerSweepTests, hashSetRemoveReclaimsOwnedMember) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashSet<Cell> set = heap HashSet<Cell>(16);\n"
        "            Cell probe = heap Cell(7);\n"
        "            set.add(#heap Cell(7));\n"
        "            int64 afterAdd = Cajeta.liveCount();\n"
        "            set.remove(probe);\n"
        "            if (Cajeta.liveCount() != afterAdd - 1) { return -96; }\n"
        "            if (set.count() != 0) { return -98; }\n"
        "            t = 1;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// ===================== LinkedList =====================

// `list.add(#v)` — the node's value store forwards the caller's flag (the
// CacheNode treatment): the element survives the call and the list's
// teardown reclaims it through the node chain.
TEST(ContainerSweepTests, linkedListOwnedAddDropsAtTeardown) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            LinkedList<Cell> list = heap LinkedList<Cell>();\n"
        "            list.add(#heap Cell(7));\n" +
        std::string(kChurn) +
        "            t = list.get(0).n;\n"
        "        }\n"
        "        return t;\n"
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

// `list.add(v)` — the node borrows (the ctor forward carries the caller's
// REAL flag; a static-1 word here once stamped borrowed adds owned and the
// teardown freed the caller's element — the CreatorRest composition bug).
TEST(ContainerSweepTests, linkedListBorrowAddLeavesOwnerAlive) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            LinkedList<Cell> list = heap LinkedList<Cell>();\n"
        "            list.add(mine);\n"
        "            if (list.get(0).n != 9) { return -98; }\n"
        "        }\n"
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

// popHead is remove-shaped: membership ends, the value returns in the mode
// the entry held (flagged) — an owned element's title rides out to the
// receiving local, which reclaims it; the node itself is freed either way.
TEST(ContainerSweepTests, linkedListPopHeadReturnsFlagged) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            LinkedList<Cell> list = heap LinkedList<Cell>();\n"
        "            list.add(#heap Cell(7));\n"
        "            {\n"
        "                Cell got = list.popHead();\n"   // flagged: owned → got owns
        "                t = got.n;\n"
        "            }\n"                                // got reclaims the Cell
        "            if (list.count() != 0) { return -98; }\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// ===================== Collectors / stream sinks =====================

// Collect is leak-free end to end (6.2.5) with a NAMED stream root: the
// Collector temp arg surrenders via its forwarded flag (the collector is
// reclaimed by collect's formal at exit), and collect flags the
// accumulated list out via Cajeta.flagged so the receiving local's entry
// arms (the closure-call path doesn't ride the flag protocol yet). A
// TEMP stream root (`(heap ArrayStream(...)).collect(...)`) still leaks
// its header — class-returning calls never reclaim their receiver (the
// wrapper-composite hazard; see SinkTempOwnershipTests).
TEST(ContainerSweepTests, collectToListSumsCorrectly) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32[] data = { 3, 1, 2 };\n"
        "        ArrayStream<int32> src = heap ArrayStream<int32>(data, 3);\n"
        "        #ArrayList<int32> xs = src.collect(Collectors.toList<int32>());\n"
        "        return xs.get(0) + xs.get(1) + xs.get(2);\n"   // 6
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

// ===================== Trees (6.2.4) =====================
// Node-graph ownership is the flat REGISTRY (the tree owns its nodes via an
// owning ArrayList; every link field stays a borrow, so rotations and splits
// never fight displaced-release). Values ride the node ctor's consuming
// stores (RedBlack) / the per-slot leaf sidecar (BPlus).

// Owned puts through rotation territory: 1,2,3 ascending forces a left
// rotation at the root. Values owned by the tree; teardown reclaims them;
// reads stay correct through the rotation.
TEST(ContainerSweepTests, redBlackOwnedPutReclaimsAtTeardown) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            RedBlackTree<int32, Cell> m = heap RedBlackTree<int32, Cell>();\n"
        "            m.put(1, #heap Cell(10));\n"
        "            m.put(2, #heap Cell(20));\n"
        "            m.put(3, #heap Cell(30));\n" +
        std::string(kChurn) +
        "            t = m.get(1).n + m.get(3).n;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 40);
}

// Borrowed put: the local keeps the title through fixup and teardown.
TEST(ContainerSweepTests, redBlackBorrowPutLeavesOwnerAlive) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            RedBlackTree<int32, Cell> m = heap RedBlackTree<int32, Cell>();\n"
        "            m.put(1, mine);\n"
        "            if (m.get(1).n != 9) { return -98; }\n"
        "        }\n"
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

// Same-key put over an owned value displaced-releases the old one.
TEST(ContainerSweepTests, redBlackReplaceDropsDisplacedOwnedValue) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            RedBlackTree<int32, Cell> m = heap RedBlackTree<int32, Cell>();\n"
        "            m.put(1, #heap Cell(3));\n"
        "            m.put(1, #heap Cell(5));\n" +
        std::string(kChurn) +
        "            t = m.get(1).n;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// The graph pin with ownership: 64 owned inserts drive the full fixup
// (recolours + rotations both directions); everything reads back and the
// teardown reclaims all 64.
TEST(ContainerSweepTests, redBlackManyOwnedInsertsBalance) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            RedBlackTree<int32, Cell> m = heap RedBlackTree<int32, Cell>();\n"
        "            int32 i = 0;\n"
        "            while (i < 64) { m.put(i, #heap Cell(i * 2)); i = i + 1; }\n"
        "            t = m.get(50).n + (int32) m.count();\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 164);
}

// BPlus: owned values survive the leaf shifts and the split cascade (40 >
// order 32 forces a split), and the teardown reclaims all of them.
TEST(ContainerSweepTests, bplusOwnedPutSurvivesSplitReclaimsAtTeardown) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            BPlusTree<int32, Cell> m = heap BPlusTree<int32, Cell>();\n"
        "            int32 i = 0;\n"
        "            while (i < 40) { m.put(i, #heap Cell(i * 3)); i = i + 1; }\n" +
        std::string(kChurn) +
        "            t = m.get(35).n + (int32) m.count();\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 145);
}

// BPlus borrowed put: the owner keeps the title through shift/split/teardown.
TEST(ContainerSweepTests, bplusBorrowPutLeavesOwnerAlive) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            BPlusTree<int32, Cell> m = heap BPlusTree<int32, Cell>();\n"
        "            m.put(1, mine);\n"
        "            int32 i = 10;\n"
        "            while (i < 50) { m.put(i, #heap Cell(i)); i = i + 1; }\n"
        "            if (m.get(1).n != 9) { return -98; }\n"
        "        }\n"
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

// BPlus same-key put over an owned value displaced-releases the old one.
TEST(ContainerSweepTests, bplusReplaceDropsDisplacedOwnedValue) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            BPlusTree<int32, Cell> m = heap BPlusTree<int32, Cell>();\n"
        "            m.put(1, #heap Cell(3));\n"
        "            m.put(1, #heap Cell(5));\n" +
        std::string(kChurn) +
        "            t = m.get(1).n;\n"
        "        }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
