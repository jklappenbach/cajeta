//
// title-tracking Unit 6 (plan 6.1) — container surface, spec §6. Entries are
// dual-capable per rev 2: the CALL SITE decides ownership, the entry bit
// records it, and every read-back mode follows the bit. `#map[k]` binds to
// operator#[] (title out, entry stays RESIDENT, panic when there is no title
// to give); `remove(k)` ends membership and returns the value in whatever
// mode the entry held (the flagged return). Red-first: at authoring time
// `map[k] = #v` silently DROPS the transfer (the operator[]= lowering passes
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

// 6.1.1a — `map[k] = #v`: the entry takes the title, and the MAP's teardown
// (not the caller's scope) reclaims the value. Leak oracle 0 and the value
// readable until the map dies.
TEST(ContainerTitleTests, indexedOwnedStoreDropsAtMapTeardown) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] = #heap Cell(7);\n"     // owned entry
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

// 6.1.1b — `map[k] = v`: the entry borrows; the local keeps the title and the
// map's teardown must NOT free it out from under the owner.
TEST(ContainerTitleTests, indexedBorrowStoreLeavesOwnerAlive) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] = mine;\n"              // borrowed entry
        "            if (m[1].n != 9) { return -98; }\n"
        "        }\n"                             // teardown skips the entry
        "        return mine.n;\n"                // owner still alive
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

// 6.1.1c — mixed map: one owned entry, one borrowed, in the SAME map. The
// teardown walk drops exactly the owned one.
TEST(ContainerTitleTests, mixedOwnershipMapDropsOnlyOwnedEntries) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell lent = heap Cell(20);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m[1] = #heap Cell(3);\n"     // owned
        "            m[2] = lent;\n"              // borrowed
        "            t = m[1].n + m[2].n;\n"      // 23
        "        }\n"
        "        return t + lent.n;\n"            // 23 + 20 = 43
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
        "        m[1] = #heap Cell(5);\n"
        "        {\n"
        "            Cell got = #m[1];\n"         // title out; got owns
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
        "        m[1] = #heap Cell(6);\n"
        "        Cell got = #m[1];\n"
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
        "        m[1] = #heap Cell(4);\n"
        "        Cell first = #m[1];\n"
        "        try {\n"
        "            Cell second = #m[1];\n"      // no title left → panic
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
        "        m[1] = #heap Cell(4);\n"
        "        try {\n"
        "            Cell ghost = #m[2];\n"       // absent → panic
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
        "        m[1] = #heap Cell(8);\n"
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

// 6.1.3b — remove of a BORROWED entry: membership ends, the return is a
// borrow, and the original owner keeps the single drop.
TEST(ContainerTitleTests, removeReturnsBorrowOwnerKeepsTitle) {
    std::string src = std::string(kCellMapSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(11);\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m[1] = mine;\n"
        "        {\n"
        "            Cell v = m.remove(1);\n"     // flagged return: borrow
        "            if (v.n != 11) { return -96; }\n"
        "        }\n"                             // v's exit must NOT free mine
        "        if (m.containsKey(1)) { return -95; }\n"
        "        return mine.n;\n"
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
        "        this.sessions = #heap HashMap<int32, Cell>();\n"
        "    }\n"
        "    public void enroll(int32 id, Cell s) {\n"
        "        this.sessions[id] = #s;\n"       // forwards the caller's flag
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
        "                Cell handoff = #r.sessions[1];\n"  // title to driver
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
