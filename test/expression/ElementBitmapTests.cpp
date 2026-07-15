// title-stores Unit 2 (spec §3.1): eager tail bitmap — allocation + addressing.
// RED until 2.2.x lands.
//
// Layout contract: a droppable-element `heap T[n]` allocates ONE block
// `header(8) | data[n*stride] | bits(ceil(n/8))`; primitive-element arrays
// are unchanged. `Cajeta.allocatedBytes()` accounts exact requested bytes
// (__cajeta_note_alloc), so the deltas pin exact numbers.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

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

}  // namespace

// 2.1.1a — class-pointer elements: heap Cell[64] = 8 + 64*8 + ceil(64/8)
// = 528 for the array block, PLUS the 9.2.1 move-sidecar (64*8 = 512) that
// survives until Unit 3.2.3 reconciles it away — Unit-2 total 1040. The
// sidecar-free 528 re-pins in Unit 3.
TEST(ElementBitmapTests, classElementArrayCarriesTailBitmap) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 before = Cajeta.allocatedBytes();\n"
        "        Cell[] a = heap Cell[64];\n"
        "        int64 delta = Cajeta.allocatedBytes() - before;\n"
        "        if (a.count() != 64L) { return -1; }\n"
        "        if (delta != 1040L) { return (int32) delta; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.1b — non-multiple-of-8 count rounds the tail up: heap Cell[3] =
// 8 + 24 + 1 = 33 array block + 24 sidecar (Unit-2 total 57; 33 after 3.2.3).
TEST(ElementBitmapTests, tailRoundsUpToWholeBytes) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 before = Cajeta.allocatedBytes();\n"
        "        Cell[] a = heap Cell[3];\n"
        "        int64 delta = Cajeta.allocatedBytes() - before;\n"
        "        if (a.count() != 3L) { return -1; }\n"
        "        if (delta != 57L) { return (int32) delta; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.1c — primitive elements unchanged: a non-escaping local int64[]
// is FRAME-ARENA allocated (not heap-noted), so its allocatedBytes delta
// is 0 today and must stay 0 — no tail, no accounting change.
TEST(ElementBitmapTests, primitiveArrayUnchanged) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 before = Cajeta.allocatedBytes();\n"
        "        int64[] a = heap int64[64];\n"
        "        int64 delta = Cajeta.allocatedBytes() - before;\n"
        "        if (a.count() != 64L) { return -1; }\n"
        "        if (delta != 0L) { return (int32) delta; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.2 — bits are zero-initialized and inert: plain stores + reads + drop
// of a class-element array behave exactly as today (borrowed elements,
// nothing freed by the array, zero leak from the array itself).
TEST(ElementBitmapTests, bitsZeroInitializedAndInert) {
    std::string src = std::string(kCellSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Cell keep = heap Cell(5);\n"
        "        int32 t = 0;\n"
        "        {\n"
        "            Cell[] a = heap Cell[8];\n"
        "            a[0] = keep;\n"                    // plain store: borrow
        "            t = a[0].n;\n"
        "        }\n"                                    // array drops; keep survives
        "        if (keep.n != 5) { return -2; }\n"
        "        int64 leaked = Cajeta.liveCount() - base - 1;\n"  // keep itself
        "        return (int32) (leaked * 100) + t;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
