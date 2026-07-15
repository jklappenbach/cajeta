//
// title-tracking Unit 4 (plan 4.1) — array slot ownership bits (spec §5
// slots, §6.3.3). Class-reference array slots carry a per-slot ownership
// bit set by the store's spelling: `a[i] = #x` → owned (array teardown
// drops the slot), plain store → borrowed (source books untouched,
// teardown skips). `#a[i]` extracts the title and decays the slot bit;
// extraction from a borrowed or null slot panics. The compiler owns the
// array layout, so no operator is involved (§6.3.3).
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

} // namespace

// 4.1.1a — owned slot store: `a[i] = #x` sets the slot bit; the array's
// teardown drops the element. Net liveCount delta 0.
TEST(ArraySlotBitsTests, ownedSlotStoreDropsAtTeardown) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell[] a = heap Cell[3];\n"
        "        a[0] = #heap Cell(1);\n"
        "        a[1] = #heap Cell(2);\n"
        "        return a[0].n + a[1].n;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 4.1.1b — plain slot store is a borrow store: the source local keeps its
// drop entry (fires at scope exit); the array's teardown skips the slot.
// Exactly one drop — no leak, no double free.
TEST(ArraySlotBitsTests, plainSlotStoreBorrowsTeardownSkips) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            Cell[] a = heap Cell[2];\n"
        "            a[0] = mine;\n"
        "            if (a[0].n != 9) { return -98; }\n"
        "        }\n"                       // array drops; must NOT free mine
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

// 4.1.1c — overwriting an OWNED slot drops the displaced element at the
// store; overwriting a borrowed slot drops nothing.
TEST(ArraySlotBitsTests, overwriteOwnedSlotDropsDisplaced) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell[] a = heap Cell[1];\n"
        "        a[0] = #heap Cell(1);\n"
        "        int64 mid = Cajeta.liveCount();\n"
        "        a[0] = #heap Cell(2);\n"     // displaces Cell(1) → dropped
        "        int64 after = Cajeta.liveCount();\n"
        "        if (after != mid) { return -97; }\n"
        "        return a[0].n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// 4.1.2a — `#a[i]` extraction: title to the assignee, slot decays to
// borrowed (stays resident and readable), array teardown skips it.
TEST(ArraySlotBitsTests, slotExtractionMovesTitle) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell[] a = heap Cell[1];\n"
        "        a[0] = #heap Cell(6);\n"
        "        Cell taken = #a[0];\n"       // title out; bit decays
        "        if (a[0].n != 6) { return -96; }\n"  // resident, readable
        "        return taken.n;\n"           // taken owns; drops at exit
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

// 4.1.2b — extraction from a borrowed slot panics (no forged title).
TEST(ArraySlotBitsTests, extractionFromBorrowedSlotPanics) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell mine = heap Cell(8);\n"
        "        Cell[] a = heap Cell[1];\n"
        "        a[0] = mine;\n"
        "        try {\n"
        "            Cell taken = #a[0];\n"   // borrowed slot → panic
        "            return -95;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.2c — extraction from a NULL slot panics.
TEST(ArraySlotBitsTests, extractionFromNullSlotPanics) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell[] a = heap Cell[1];\n"
        "        try {\n"
        "            Cell taken = #a[0];\n"   // never stored → panic
        "            return -94;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.3 — `String[]` keeps its inline-slot share path (§5.1.6): String
// elements are shared-capable values, not title-carrying slots. The
// existing local-array reclaim semantics (slices 9.2.1) stay intact.
TEST(ArraySlotBitsTests, stringArraysSharePathUnaffected) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        String[] xs = heap String[2];\n"
        "        String a = \"hello\";\n"
        "        xs[0] = a + \"!\";\n"
        "        xs[1] = a + \"?\";\n"
        "        return (int32) (xs[0].count() + xs[1].count());\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 t = work();\n"
        "        int64 leaked = Cajeta.liveCount() - base;\n"
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 12);
}
