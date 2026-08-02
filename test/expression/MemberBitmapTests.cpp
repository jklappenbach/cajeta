// title-stores Unit 4 (spec §3.3.2, §3.4): inline value-struct elements
// carry PER-MEMBER ownership bits, replicated per slot — each slot holds
// exactly the ownership word its heap counterpart would (one bit per
// droppable member, same indices). RED until 4.2.x lands: today value-type
// elements are excluded from slot bits entirely, so member stores record
// nothing and teardown drops nothing.
//
// Fixture shape: the MapEntry reality — a @ValueType element with TWO
// class members, stored key-owned / val-borrowed in the SAME slot.

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
    "@ValueType public final class Entry<A, B> {\n"
    "    public A a;\n"
    "    public B b;\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 4.1.1a — MapEntry shape: key-owned / val-borrowed in ONE slot. Teardown
// drops exactly the owned member; the borrowed member's object survives.
TEST(MemberBitmapTests, mixedMembersInOneSlotTearDownIndependently) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell mine = heap Cell(7);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Entry<Cell, Cell>[] slots = heap Entry<Cell, Cell>[4];\n"
        "            slots[1].a #= heap Cell(3);\n"   // member a: OWNED
        "            slots[1].b = mine;\n"            // member b: borrowed
        "            t = slots[1].a.n * 10 + slots[1].b.n;\n"   // 37
        "        }\n"                                  // drops slots: a freed, b left
        "        if (mine.n != 7) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // mine itself
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 37);
}

// 4.1.1b — both members owned in one slot: both drop; a fully-borrowed
// sibling slot drops nothing.
TEST(MemberBitmapTests, bothOwnedBothDropBorrowedSlotUntouched) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell keepA = heap Cell(1);\n"
        "        Cell keepB = heap Cell(2);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Entry<Cell, Cell>[] slots = heap Entry<Cell, Cell>[4];\n"
        "            slots[0].a #= heap Cell(5);\n"
        "            slots[0].b #= heap Cell(6);\n"
        "            slots[2].a = keepA;\n"
        "            slots[2].b = keepB;\n"
        "            t = slots[0].a.n + slots[0].b.n;\n"          // 11
        "        }\n"
        "        if (keepA.n != 1 || keepB.n != 2) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 2;\n"  // the keeps
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// 4.1.1c — displaced release is PER MEMBER: overwriting an owned member
// drops only the displaced occupant; the sibling member's bit and object
// are untouched. Churn reuses freed storage so a stale pointer misreads.
TEST(MemberBitmapTests, memberOverwriteDisplacesOnlyThatMember) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Entry<Cell, Cell>[] slots = heap Entry<Cell, Cell>[4];\n"
        "            slots[1].a #= heap Cell(3);\n"
        "            slots[1].b #= heap Cell(4);\n"
        "            slots[1].a #= heap Cell(8);\n"   // displaces the 3
        "            int32 c = 0;\n"
        "            int32 churn = 0;\n"
        "            while (c < 32) { Cell j = heap Cell(900 + c); churn = churn + j.n; c = c + 1; }\n"
        "            if (churn < 0) { return churn; }\n"
        "            t = slots[1].a.n * 10 + slots[1].b.n;\n"     // 84
        "        }\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 84);
}

// 4.1.1d — member move-out `#slots[i].a` clears exactly that member's bit
// and forwards the title: the receiver drops it, the slot's OTHER member
// still drops with the array, nothing double-frees.
TEST(MemberBitmapTests, memberMoveOutForwardsAndClearsOnlyThatBit) {
    std::string src = std::string(kFixtureSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Entry<Cell, Cell>[] slots = heap Entry<Cell, Cell>[4];\n"
        "            slots[1].a #= heap Cell(3);\n"
        "            slots[1].b #= heap Cell(4);\n"
        "            {\n"
        "                Cell taken #= #slots[1].a;\n"   // title out, bit clears
        "                t = taken.n * 10;\n"            // 30
        "            }\n"                                 // taken drops here
        "            t = t + slots[1].b.n;\n"             // 34
        "        }\n"                                      // array drops b only
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 34);
}

// 4.1.2a — HashMap mixed ownership CONTRACT (green today against owned[],
// must stay green when 4.2.2 deletes it): owned key + borrowed value per
// put; replace displaced-releases the owned old value; teardown drops
// exactly what the map took.
TEST(MemberBitmapTests, hashMapMixedOwnershipContract) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 nn) { this.n = nn; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell held = heap Cell(9);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m.put(1, #heap Cell(3));\n"     // owned value
        "            m.put(2, held);\n"              // borrowed value
        "            m.put(1, #heap Cell(5));\n"     // replace: drops the 3
        "            t = m.get(1).n * 10 + m.get(2).n;\n"   // 59
        "        }\n"                                  // map drops the 5 only
        "        if (held.n != 9) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // held
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 59);
}

// 4.1.2b — HashMap remove hands the value back in the mode the entry held
// (flagged): an owned remove transfers the title to the caller (whose
// local drops it); a borrowed remove hands back a borrow. CONTRACT pin.
TEST(MemberBitmapTests, hashMapRemoveFlaggedContract) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 nn) { this.n = nn; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell held = heap Cell(9);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "            m.put(1, #heap Cell(3));\n"
        "            m.put(2, held);\n"
        "            {\n"
        "                Cell out = m.remove(1);\n"   // owned → title to out
        "                t = out.n * 10;\n"           // 30
        "            }\n"                              // out drops the 3
        "            {\n"
        "                Cell b = m.remove(2);\n"     // borrowed → borrow back
        "                t = t + b.n;\n"              // 39
        "            }\n"
        "            if ((int32) m.count() != 0) { return -3; }\n"
        "        }\n"
        "        if (held.n != 9) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // held
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 39);
}

// PROBE — split the flagged-remove contract: does the single-sharp fused claim
// break the OWNED path, the BORROWED path, or both?
TEST(MemberBitmapTests, PROBE_removeOwnedOnly) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell { public int32 n; public Cell(int32 nn) { this.n = nn; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m.put(1, #heap Cell(3));\n"
        "        Cell out = m.remove(1);\n"
        "        return out.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

TEST(MemberBitmapTests, PROBE_removeBorrowedOnly) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public class Cell { public int32 n; public Cell(int32 nn) { this.n = nn; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell held = heap Cell(9);\n"
        "        HashMap<int32, Cell> m = heap HashMap<int32, Cell>();\n"
        "        m.put(2, held);\n"
        "        Cell b = m.remove(2);\n"
        "        return b.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9);
}
