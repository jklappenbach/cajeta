// title-stores Unit 3 (spec §3.2, §3.3.1, §3.3.3-4, §3.5): element-slot
// title semantics — store, displaced release, move-out, teardown. RED
// until 3.2.x lands.
//
// Fixture shape: a user container with a `Cell[] data` FIELD (the spec
// §2.5.1 container-author story). Today field arrays drop no elements
// (the 15.13 family) and slot stores don't record titles; every liveCount
// oracle here fails red.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kFixtureSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n"
    "public class MiniVec {\n"
    "    public Cell[] data;\n"
    "    public int32 size;\n"
    "    public MiniVec(int32 cap) {\n"
    "        this.data = heap Cell[cap];\n"
    "        this.size = 0;\n"
    "    }\n"
    "    public void add(Cell v) {\n"
    "        this.data[this.size] #= v;\n"
    "        this.size = this.size + 1;\n"
    "    }\n"
    "    public Cell get(int32 i) {\n"
    "        return this.data[i];\n"
    "    }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 3.1.1 — `data[i] #= v` records the FORWARDED flag: owned adds drop with
// the container, a lent add leaves the caller's object alone. Mixed in one
// array.
TEST(ElementSlotSemanticsTests, storeForwardsFlagTeardownDropsOwnedOnly) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell mine = heap Cell(4);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(8);\n"
        "            v.add(#heap Cell(1));\n"          // owned slot
        "            v.add(mine);\n"                    // lent slot
        "            v.add(#heap Cell(2));\n"           // owned slot
        "            t = v.get(0).n + v.get(1).n + v.get(2).n;\n"
        "        }\n"                                    // drops slots 0+2 only
        "        if (mine.n != 4) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // mine
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 3.1.2 — displaced release: overwriting an OWNED slot drops the occupant;
// overwriting a BORROWED slot leaves the caller's object live. The borrow
// arrives as a runtime-owner formal with flag 0 (`#=` of a statically-owned
// LOCAL is a hard move per spec §2.1 — the lint rightly rejects reading it
// after; caller discretion is the borrow spelling).
TEST(ElementSlotSemanticsTests, displacedReleaseOnOwnedSlotOnly) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static void putAt(MiniVec v, int32 i, Cell c) {\n"
        "        v.data[i] #= c;\n"                      // forwards the caller's flag
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell keep = heap Cell(5);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(4);\n"
        "            v.add(#heap Cell(1));\n"
        "            v.data[0] #= heap Cell(2);\n"      // displaces + frees Cell(1)
        "            int64 mid = Cajeta.liveCount() - base;\n"
        "            if (mid != 4L) { return (int32) (mid * -1); }\n"  // keep+vec+data+Cell(2)
        "            putAt(v, 1, keep);\n"               // plain arg: flag 0 -> borrow bit
        "            v.data[1] #= heap Cell(3);\n"       // displaces borrow: NO free
        "            if (keep.n != 5) { return -3; }\n"
        "            t = v.data[0].n + v.data[1].n;\n"
        "        }\n"
        "        if (keep.n != 5) { return -4; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// 3.1.3 — move-out `#data[i]` clears the bit and forwards it: the grow
// loop `bigger[i] #= #this.data[i]` transfers every owned element to the
// new array; the old array then tears down empty. Zero leak, zero UAF.
TEST(ElementSlotSemanticsTests, moveOutClearsAndForwards) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(2);\n"
        "            v.add(#heap Cell(1));\n"
        "            v.add(#heap Cell(2));\n"
        "            Cell[] bigger = heap Cell[4];\n"
        "            int32 i = 0;\n"
        "            while (i < v.size) {\n"
        "                bigger[i] #= #v.data[i];\n"
        "                i = i + 1;\n"
        "            }\n"
        "            v.data #= bigger;\n"                // old array drops EMPTY
        "            t = v.data[0].n + v.data[1].n;\n"
        "        }\n"                                     // new array drops 2 cells
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 3.1.4 — `Cajeta.dropValue(#data[i])` is bit-guarded: owned slot frees +
// clears (teardown then skips it); borrowed slot no-ops.
TEST(ElementSlotSemanticsTests, dropValueBitGuarded) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell keep = heap Cell(9);\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(4);\n"
        "            v.add(#heap Cell(1));\n"
        "            v.add(keep);\n"
        "            Cajeta.dropValue(#v.data[0]);\n"    // owned: freed + cleared
        "            Cajeta.dropValue(#v.data[1]);\n"    // borrowed: no-op
        "            if (keep.n != 9) { return -2; }\n"
        "        }\n"                                     // teardown double-frees nothing
        "        if (keep.n != 9) { return -3; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"
        "        return (int32) (leaked * 100) + 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.6 — vacant-slot safety: a partially-populated array tears down clean
// with NO @ElementCount — the bitmap says which of [0..cap) are owned.
TEST(ElementSlotSemanticsTests, vacantSlotsSafeWithoutElementCount) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(64);\n"    // 62 slots stay vacant
        "            v.add(#heap Cell(1));\n"
        "            v.add(#heap Cell(2));\n"
        "            t = v.get(0).n + v.get(1).n;\n"
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 3.1.5 — String elements: a PLAIN slot store keeps the dual-role resolve
// (borrowed bytes copy; the 5.2.6 rule at slot granularity), while the
// bitmap machinery stays out of String[] entirely in v1
// (arrayElementCarriesSlotBits excludes String — pinned so a later widening
// is a deliberate decision, not drift).
TEST(ElementSlotSemanticsTests, stringSlotPlainStoreStillResolves) {
    std::string src =
        "package test;\n"
        "public class Keep {\n"
        "    public String[] tags;\n"
        "    public Keep(int32 cap) { this.tags = heap String[cap]; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Keep k = heap Keep(4);\n"
        "        {\n"
        "            String s = \"tag\" + Cajeta.liveCount();\n"
        "            k.tags[0] = s;\n"                   // plain: resolve-copy
        "        }\n"                                     // s drops; copy survives
        "        if (k.tags[0].size() < 4L) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.2.3 — a LOCAL bit-array's scope-exit drop walks the tail before the
// buffer frees (the sidecar's class-element role retires; one mechanism).
TEST(ElementSlotSemanticsTests, localArrayTeardownWalksBits) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell[] a = heap Cell[4];\n"
        "            a[0] #= heap Cell(1);\n"
        "            a[1] #= heap Cell(2);\n"
        "            t = a[0].n + a[1].n;\n"
        "        }\n"                                    // walk drops 2, buffer frees
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 3.1.5 (alias half) — bits live in the array HEADER, so an alias sees the
// same titles: a take through the alias decays the one true bit and the
// owner's teardown does not double-free.
TEST(ElementSlotSemanticsTests, aliasSeesSameBits) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(4);\n"
        "            v.add(#heap Cell(6));\n"
        "            Cell[] alias = v.data;\n"           // lend of the array
        "            Cell got #= #alias[0];\n"           // take via the alias
        "            t = got.n;\n"
        "        }\n"                                     // v drops nothing owned; got drops
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// §2.1 fused forwarding — `dst[i] #= #src[j]` moves WHATEVER title the
// source slot holds: owned transfers (dst drops it), borrow forwards as
// borrow (no panic, the true owner keeps its single drop). The container
// author's shift/sift primitive.
TEST(ElementSlotSemanticsTests, fusedForwardingMovesBorrowsWithoutPanic) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static void putAt(MiniVec v, int32 i, Cell c) {\n"
        "        v.data[i] #= c;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell keep = heap Cell(4);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            MiniVec v = heap MiniVec(4);\n"
        "            v.add(#heap Cell(1));\n"           // slot 0 owned
        "            putAt(v, 1, keep);\n"               // slot 1 borrowed
        "            v.data[2] #= #v.data[0];\n"         // forward owned 0 -> 2
        "            v.data[3] #= #v.data[1];\n"         // forward BORROW 1 -> 3 (no panic)
        "            t = v.data[2].n + v.data[3].n;\n"
        "        }\n"                                     // teardown drops slot 2 only
        "        if (keep.n != 4) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
