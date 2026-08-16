//
// title-tracking Unit 3 (plan 3.1) — field ownership bits (spec §5).
// Class-reference fields carry a runtime ownership bit set by the store's
// spelling: `this.f #= x` → owned (teardown drops), `this.f = x` → borrowed
// (source books untouched, teardown skips). Overwriting an owned field
// drops the displaced value; `#this.f` extracts the title and decays the
// bit; extraction from a titleless field panics.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

const char* kHolderSrc =
    "package test;\n"
    "public class Cell {\n"
    "    public int32 n;\n"
    "    public Cell(int32 nn) { this.n = nn; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Cell f;\n"
    "    public void setOwned(#Cell c) { this.f #= c; }\n"
    "    public void setBorrow(Cell c) { this.f = c; }\n"
    "}\n";

int32_t runI32(const std::string& src, const char* entryClass = "test.D") {
    auto jit = CajetaJit::compile(src, entryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// 3.1.1a — owned store: `this.f #= c` sets the owned bit; the holder's
// teardown drops the payload. Net liveCount delta 0.
TEST(FieldOwnershipBitsTests, ownedStoreDropsAtTeardown) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Holder h = heap Holder();\n"
        "        h.setOwned(#heap Cell(7));\n"
        "        return h.f.n;\n"
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

// 3.1.1b — plain store is a borrow store: the source local keeps its books
// (its own drop entry fires at scope exit), the holder's teardown skips the
// field. Exactly one drop — no leak, no double free.
TEST(FieldOwnershipBitsTests, plainStoreBorrowsTeardownSkips) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(9);\n"
        "        {\n"
        "            Holder h = heap Holder();\n"
        "            h.setBorrow(mine);\n"
        "            if (h.f.n != 9) { return -98; }\n"
        "        }\n"                       // holder drops; must NOT free mine
        "        return mine.n;\n"          // borrow source still alive
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

// 3.1.2a — overwriting an OWNED field drops the displaced value at the
// store (the overwrite is its scope exit).
TEST(FieldOwnershipBitsTests, overwriteOwnedDropsDisplaced) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Holder h = heap Holder();\n"
        "        h.setOwned(#heap Cell(1));\n"
        "        int64 mid = Cajeta.liveCount();\n"
        "        h.setOwned(#heap Cell(2));\n"   // displaces Cell(1) → dropped
        "        int64 after = Cajeta.liveCount();\n"
        "        if (after != mid) { return -97; }\n"  // net zero across overwrite
        "        return h.f.n;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// 3.1.2b — overwriting a BORROWED field drops nothing (the borrow source
// still owns its value and reads it after).
TEST(FieldOwnershipBitsTests, overwriteBorrowedDropsNothing) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cell a = heap Cell(3);\n"
        "        Cell b = heap Cell(4);\n"
        "        Holder h = heap Holder();\n"
        "        h.setBorrow(a);\n"
        "        h.setBorrow(b);\n"          // overwrite: must not free a
        "        return a.n + h.f.n;\n"      // 3 + 4
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 3.1.4 — regression of the implicit-transfer bug (spec §1.2 problem 2):
// a plain field store must NOT deactivate the source local's drop entry.
// Pre-fix, `this.f = c` silently moved the books; the local leaked its
// entry and teardown double-accounted.
TEST(FieldOwnershipBitsTests, plainStoreLeavesSourceBooksUntouched) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell c = heap Cell(5);\n"
        "        Holder h = heap Holder();\n"
        "        h.setBorrow(c);\n"
        "        return c.n;\n"              // source readable: no move happened
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

// 3.1.3a — `#h.f` extraction: title moves to the assignee, the field bit
// decays to borrowed (field stays readable), holder teardown skips it.

// 3.1.3b — mode-carrying claim (mode-carrying-claim §5.1): `#=` from a
// field holding no title (borrowed) yields a BORROW rather than panicking
// or minting a forged title. The field stays resident; the true owner
// still drops exactly once.
TEST(FieldOwnershipBitsTests, extractionFromBorrowedFieldYieldsBorrow) {
    std::string src = std::string(kHolderSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Cell mine = heap Cell(8);\n"
        "        Holder h = heap Holder();\n"
        "        h.setBorrow(mine);\n"
        "        Cell taken #= h.f;\n"       // no title here → borrow
        "        if (h.f.n != 8) { return -95; }\n"  // field resident
        "        return taken.n;\n"          // borrow: no drop of its own
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

// Unit 4 close-out discovery — STATIC fields carry no ownership bit, so a
// plain store into one keeps the LEGACY implicit transfer: `Reg.shared = r`
// hands the object to the static; the source local's drop entry deactivates
// and the singleton survives its constructing frame (the `instance()` shape
// the pre-3A block covered; regression seen as IfxRegistry/Ws/Https SIGSEGVs
// reading a poisoned singleton). Statics get a real owner story in Unit 5.
TEST(FieldOwnershipBitsTests, staticFieldStoreTransfersSingleton) {
    std::string src =
        "package test;\n"
        "public final class Reg {\n"
        "    private static Reg shared;\n"
        "    public int32 tag;\n"
        "    public Reg() { this.tag = 42; }\n"
        "    public static Reg instance() {\n"
        "        if (Reg.shared == null) {\n"
        "            Reg r = heap Reg();\n"
        "            Reg.shared = r;\n"
        "        }\n"
        "        return Reg.shared;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Reg a = Reg.instance();\n"
        "        Reg b = Reg.instance();\n"
        "        if (a != null && b != null && a.tag == 42 && b.tag == 42) {\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
