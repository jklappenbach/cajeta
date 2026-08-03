//
// title-tracking Unit 6 (plan 6.1) — container surface, spec §6. Entries are
// dual-capable per rev 2: the CALL SITE decides ownership, the entry bit
// records it, and every read-back mode follows the bit. `#map[k]` binds to
// operator#[] (title out, entry stays RESIDENT, panic when there is no title
// to give); `remove(k)` ends membership and returns the value in whatever
// mode the entry held (the flagged return). Red-first: at authoring time
// `map[k] #= v` silently DROPS the transfer (the operator[]= lowering passes
// no transfer word) and `#map[k]` silently degrades to a plain read.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kCellMapSrc =
    "package test;\n"
    "import cajeta.collection.HashMap;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 6.1.1a — `map[k] #= v`: the entry takes the title, and the MAP's teardown
// (not the caller's scope) reclaims the value. Leak oracle 0 and the value
// readable until the map dies.
TEST(ContainerTitleTests, indexedOwnedStoreDropsAtMapTeardown) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] #= heap Cell(7);\n"     // owned entry
        "            t = m[1].n;\n"
        "        }\n"                             // map teardown drops the Cell
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

// ---- uniform-transfer-semantics 2.1.9 — the mixed-ownership rewrite -------
//
// This file's original subject was MIXED ownership: a map holding some owned
// and some borrowed entries, with the teardown walk dropping only the owned
// ones. Spec 2.3 removes the distinction — `operator[]=` takes `#K, #V`, so
// every entry is owned and lending into one is a compile error.
//
// The affected tests are rewritten as pairs (lend rejected / transfer works),
// not deleted and not `#`-patched: patching turns the compile error into a
// use-after-free at the trailing read, which reads as the test starting to
// pass. Where a test's whole point was "the borrowed half survives", the
// surviving contract is the simpler one — everything is owned, everything is
// reclaimed once.

// 6.1.1b — `map[k] = v` lends into an owning entry, which no longer type-checks.
TEST(ContainerTitleTests, indexedLendIsRejected) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] = mine;\n"
        "        return m[1].n;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        ADD_FAILURE() << "expected the lend into an owning entry to be rejected";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TRANSFER_REQUIRED");
        EXPECT_NE(e.getMessage().find("#mine"), std::string::npos)
            << e.getMessage();
    }
}

// …and `map[k] #= v` is the spelling that works. The source is demoted to a
// borrow, not invalidated, so it still reads while the map is alive.
TEST(ContainerTitleTests, indexedTransferStoreLeavesSourceReadable) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell mine = heap Cell(9);\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] #= mine;\n"
        "            if (m[1].n != 9) { return -98; }\n"
        "            t = mine.n;\n"               // demoted read, still live
        "        }\n"                             // map teardown drops the Cell
        "        return t;\n"
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

// 6.1.1c — was `mixedOwnershipMapDropsOnlyOwnedEntries`. With one kind of
// entry left, the contract is that the teardown walk drops ALL of them.
TEST(ContainerTitleTests, ownedMapDropsEveryEntryAtTeardown) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell second = heap Cell(20);\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] #= heap Cell(3);\n"
        "            m[2] #= second;\n"
        "            t = m[1].n + m[2].n;\n"      // 23
        "            t = t + second.n;\n"         // 43 — demoted read
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
    EXPECT_EQ(runI32(src), 43);
}

// 6.1.2a — `#map[k]` extraction: the title moves to the assignee (whose scope
// now drops it), the entry stays RESIDENT and readable, and the map's
// teardown skips it.
TEST(ContainerTitleTests, extractionTransfersTitleEntryStaysResident) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= heap Cell(5);\n"
        "        {\n"
        "            Cell got #= m[1];\n"         // title out; got owns
        "            t = got.n;\n"
        "        }\n"                             // got drops the Cell HERE
        "        return t;\n"
        "    }\n"                                 // map teardown: nothing owned
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 6.1.2a' — residency: after extraction the key still READS (the entry keeps
// the pointer; membership is not ownership). The extractor holds the value
// alive past the read.
TEST(ContainerTitleTests, extractedEntryStillReadable) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= heap Cell(6);\n"
        "        Cell got #= m[1];\n"
        "        return m[1].n;\n"                // resident read-back
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// 6.1.2b — extracting the same key twice: the second `#m[1]` finds a
// borrowed entry (the bit decayed) and PANICS instead of minting a second
// title. Recoverable — a catch sees it.
TEST(ContainerTitleTests, secondExtractionPanics) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= heap Cell(4);\n"
        "        Cell first #= m[1];\n"
        "        try {\n"
        "            Cell second #= m[1];\n"      // no title left → panic
        "            return -99;\n"
        "        } catch (Exception e) {\n"
        "            return first.n;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// 6.1.2c — `#map[k]` on an absent key panics (total: title or throw, never a
// null title).
TEST(ContainerTitleTests, absentKeyExtractionPanics) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= heap Cell(4);\n"
        "        try {\n"
        "            Cell ghost #= m[2];\n"       // absent → panic
        "            return -99;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.3a — remove of an OWNED entry: membership ends and the returned value
// carries the title — the receiving local drops it.
TEST(ContainerTitleTests, removeReturnsOwnedValueCallerDrops) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= heap Cell(8);\n"
        "        {\n"
        "            Cell v = m.remove(1);\n"     // flagged return: owned
        "            t = v.n;\n"
        "        }\n"                             // v drops the Cell here
        "        if (m.containsKey(1)) { return -97; }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// 6.1.3b — was `removeReturnsBorrowOwnerKeepsTitle`. There is no borrowed
// entry to remove any more, but the surviving question is the one that
// mattered: the value must be dropped ONCE. It went into the map from a local
// that is now a demoted borrow, and it comes back out to a receiving local
// that owns it — so exactly one of those two can reclaim it, and it is the
// receiver. A demoted source that still armed a drop entry would double-free
// here, which is precisely what the liveCount oracle catches.
TEST(ContainerTitleTests, removeAfterTransferDropsExactlyOnce) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        Cell mine = heap Cell(11);\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] #= mine;\n"
        "        {\n"
        "            Cell v = m.remove(1);\n"     // flagged return: owned
        "            if (v.n != 11) { return -96; }\n"
        "            t = v.n;\n"
        "        }\n"                             // v drops the Cell here
        "        if (m.containsKey(1)) { return -95; }\n"
        "        return t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// 6.1.4 — the Registry/Driver round-trip (the 2026-07-11 design-session
// example; the prose did not survive, so THIS test is now the worked
// example). A Registry owns most of its sessions and indexes one it does not
// own. The driver: registers owned sessions via `#`, lends one, EXTRACTS one
// owned session to hand off (registry keeps serving reads of it), and
// removes another (title back to the driver, who drops it). End state:
// every allocation reclaimed exactly once, the lent session survives to the
// end of the driver, and the registry teardown drops exactly the one owned
// session still in place.
TEST(ContainerTitleTests, registryDriverRoundTripLeakFree) {
    std::string src = std::string(kCellMapSrc) +
        "public class Registry {\n"
        "    public HashMap<int32, Cell> sessions;\n"
        "    public Registry() {\n"
        "        this.sessions #= heap HashMap<int32, Cell>();\n"
        "    }\n"
        "    public void enroll(int32 id, Cell s) {\n"
        "        this.sessions[id] #= s;\n"       // forwards the caller's flag
        "    }\n"
        "    public int32 peek(int32 id) { return this.sessions[id].n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell lent = heap Cell(100);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Registry r = heap Registry();\n"
        "            r.enroll(1, #heap Cell(1));\n"   // owned by registry
        "            r.enroll(2, #heap Cell(2));\n"   // owned by registry
        "            r.enroll(3, lent);\n"            // indexed, not owned
        "            {\n"
        "                Cell handoff #= r.sessions[1];\n"  // title to driver
        "                t = t + handoff.n + r.peek(1);\n"  // still readable
        "            }\n"                                    // driver drops #1
        "            {\n"
        "                Cell closed = r.sessions.remove(2);\n"  // owned out
        "                t = t + closed.n;\n"
        "            }\n"                                    // driver drops #2
        "            t = t + r.peek(3);\n"                   // lent still live
        "        }\n"       // registry teardown: only entry 3 remains, borrowed
        "        return t + lent.n;\n"    // 1+1+2+100 + 100 = 204
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 204);
}

// ===================== 6.3.1 acceptance — mixed-ownership cache =====================

namespace {

// Cache<K,V> fixture: needs the import; Cell reused from kCellMapSrc.
const char* kCellCacheSrc =
    "package test;\n"
    "import cajeta.collection.Cache;\n"
    "import cajeta.lang.Optional;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n";

const char* kCellChurn =
    "        int32 cc = 0;\n"
    "        int32 churn = 0;\n"
    "        while (cc < 64) { Cell j = heap Cell(1000 + cc); churn = churn + j.n; cc = cc + 1; }\n"
    "        if (churn < 0) { return churn; }\n";

}  // namespace

// 6.3.1 / spec §6.5, rewritten by 2.1.9. The original held OWNED and BORROWED
// values in one cache and checked that the teardown walk told them apart.
// Spec 2.3 leaves one kind of value, so the contract is: LRU eviction reclaims
// the evicted entry, teardown reclaims the rest, net liveCount delta 0.
TEST(ContainerTitleTests, cacheOwnedEvictionAndTeardownLeakFree) {
    std::string src = std::string(kCellCacheSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cache<String, Cell> c = heap Cache<String, Cell>(2);\n"
        "            String k0 = \"own-\" + 0;\n"
        "            c.put(#k0, #heap Cell(1));\n"
        "            String k1 = \"own-\" + 1;\n"
        "            c.put(#k1, #heap Cell(50));\n"
        "            Optional<Cell> a = c.get(\"own-0\");\n"
        "            Optional<Cell> b = c.get(\"own-1\");\n"    // own-1 now MRU
        "            if (a.isPresent()) { t = t + a.get().n; }\n"   // +1
        "            if (b.isPresent()) { t = t + b.get().n; }\n"   // +50
        "            String k2 = \"own-\" + 2;\n"
        "            c.put(#k2, #heap Cell(2));\n"      // evicts own-0 -> freed
        + std::string(kCellChurn) +
        "            Optional<Cell> b2 = c.get(\"own-1\");\n"
        "            if (b2.isPresent()) { t = t + b2.get().n; }\n"  // +50 resident
        "        }\n"                                   // teardown frees own-1, own-2
        "        return t + 50;\n"                      // 101 + 50 -> 151
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 151);
}

// Was `cacheEvictedBorrowedValueSurvivesWithCaller`. The half of that contract
// which survives spec 2.3 is sharper than the original and worth its own test:
// eviction reclaims the value THERE, at the put that displaces it, not later
// at teardown. Asserted on the live count across the evicting put rather than
// on a trailing read of the evicted object — which would be a use-after-free.
TEST(ContainerTitleTests, cacheEvictionReclaimsAtThePutNotAtTeardown) {
    std::string src = std::string(kCellCacheSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cache<String, Cell> c = heap Cache<String, Cell>(2);\n"
        "            String k0 = \"own-\" + 0;\n"
        "            c.put(#k0, #heap Cell(60));\n"
        "            String k1 = \"own-\" + 1;\n"
        "            c.put(#k1, #heap Cell(2));\n"
        "            Optional<Cell> a = c.get(\"own-1\");\n"   // own-0 now LRU
        "            if (a.isPresent()) { t = t + a.get().n; }\n"    // +2
        "            int64 before = Cajeta.liveCount();\n"
        "            String k2 = \"own-\" + 2;\n"
        "            c.put(#k2, #heap Cell(3));\n"      // evicts own-0
        "            int64 after = Cajeta.liveCount();\n"
        // one Cell + one String key in, one Cell + one String key out: the
        // eviction must balance the insertion at the put itself.
        "            if (after != before) { return 0 - 90; }\n"
        + std::string(kCellChurn) +
        "            t = t + 120;\n"
        "        }\n"                                   // teardown frees own-1, own-2
        "        return t;\n"                           // 122
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 122);
}

// 6.3.1 / spec §6.5 — the cache SCENARIO in user code: a hand-rolled
// capacity-2 LRU over HashMap. Rewritten by 2.1.9: `putBorrow` is gone, since
// its body (`this.map[k] = v`) lends into an owning entry and no longer
// compiles. Every eviction now reclaims its value, and teardown reclaims what
// is still resident. Net liveCount delta 0.
TEST(ContainerTitleTests, cacheScenarioMiniLruOwnedLeakFree) {
    std::string src = std::string(kCellMapSrc) +
        "public class MiniLru {\n"
        "    public HashMap<int32, Cell> map;\n"
        "    public int32 a;\n"        // LRU key   (-1 = empty)
        "    public int32 b;\n"        // MRU key   (-1 = empty)
        "    public MiniLru() {\n"
        "        this.map = heap HashMap<int32, Cell>();\n"
        "        this.a = 0 - 1;\n"
        "        this.b = 0 - 1;\n"
        "    }\n"
        "    public void put(int32 k, #Cell v) {\n"
        "        if (this.a >= 0 && this.b >= 0) {\n"
        "            int32 victim = this.a;\n"
        "            Cell evicted = this.map.remove(victim);\n"  // flagged return:
        "            this.a = this.b;\n"                          // drops if owned,
        "            this.b = 0 - 1;\n"                           // survives if borrowed
        "        }\n"
        "        this.map[k] #= v;\n"
        "        if (this.a < 0) { this.a = k; } else { this.b = k; }\n"
        "    }\n"
        "    public int32 read(int32 k) { return this.map[k].n; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniLru c = heap MiniLru();\n"
        "            c.put(1, #heap Cell(1));\n"
        "            c.put(2, #heap Cell(50));\n"
        "            t = c.read(1) + c.read(2);\n"     // 1 + 50 = 51
        "            c.put(3, #heap Cell(2));\n"       // evicts key 1 -> freed
        "            int32 cc = 0;\n"
        "            int32 churn = 0;\n"
        "            while (cc < 64) { Cell j = heap Cell(1000 + cc); churn = churn + j.n; cc = cc + 1; }\n"
        "            if (churn < 0) { return churn; }\n"
        "            t = t + c.read(2);\n"             // +50 still resident
        "            c.put(4, #heap Cell(3));\n"       // evicts key 2 -> freed
        "            cc = 0;\n"
        "            while (cc < 64) { Cell j = heap Cell(2000 + cc); churn = churn + j.n; cc = cc + 1; }\n"
        "            if (churn < 0) { return churn; }\n"
        "            t = t + 50;\n"
        "        }\n"                                  // teardown frees owned 3,4 + map
        "        return t + 50;\n"                     // 151 + 50 -> 201
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 201);
}
