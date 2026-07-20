//
// VEA-4 — view element arrays: endianness inheritance + wire-order prefixes.
//
// Two rules pinned here (Views.md § Endianness inheritance — first wired by
// VEA-4):
//   1. An UNANNOTATED view used as a `V[]` element inherits the outer
//      view's annotation at prototype time; its fields AND its internal
//      length-prefixes read in the inherited order.
//   2. An element view with its OWN annotation keeps it (mixed-endian
//      records).
// Also: every length/count prefix read (ctor sweep, fill pass, access)
// byte-swaps per the owning view's wire order — before VEA-4 no prefix
// read swapped at all (latent: all prior view tests were @HostEndian).
//
// Packing note: tests hand-pack int32[] literals on a little-endian host.
// A logical value X that must appear BIG-endian on the wire is written as
// the literal bswap32(X): 7→117440512, 2→33554432, 5→83886080, 4→67108864,
// 9→150994944. Byte strings ("abcd") are order-independent.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.E");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- 1. inheritance: unannotated element view takes the outer's order -----

TEST(ViewElementArrayEndianTests, bigEndianOuterInheritedByElements) {
    auto src =
        "package test;\n"
        "public view D {\n"                    // UNANNOTATED — inherits Big
        "    int32  s;\n"
        "    String name;\n"
        "}\n"
        "@BigEndian\n"
        "public view M {\n"
        "    int32 magic;\n"
        "    D[]   ds;\n"
        "}\n"
        "public final class E {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[8];\n"
        "        bytes[0] = 117440512;\n"      // magic = 7 (BE)
        "        bytes[1] = 33554432;\n"       // count = 2 (BE)
        "        bytes[2] = 83886080;\n"       // d0.s = 5 (BE, inherited)
        "        bytes[3] = 67108864;\n"       // d0.len = 4 (BE, inherited)
        "        bytes[4] = 1684234849;\n"     // \"abcd\"
        "        bytes[5] = 150994944;\n"      // d1.s = 9 (BE)
        "        bytes[6] = 67108864;\n"       // d1.len = 4 (BE)
        "        bytes[7] = 2054781047;\n"     // \"wxyz\"
        "        M m = M(bytes);\n"
        "        if (m.magic != 7) return 10;\n"
        "        if (m.ds.count() != 2) return 11;\n"
        "        if (m.ds[0].s != 5) return 12;\n"
        "        if (m.ds[1].s != 9) return 13;\n"
        "        String n1 = m.ds[1].name;\n"
        "        if (!n1.equals(\"wxyz\")) return 14;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- 2. explicit element annotation wins (mixed-endian record) -------------

TEST(ViewElementArrayEndianTests, elementOwnAnnotationKept) {
    auto src =
        "package test;\n"
        "@LittleEndian\n"                      // explicit — NOT inherited
        "public view D {\n"
        "    int32  s;\n"
        "    String name;\n"
        "}\n"
        "@BigEndian\n"
        "public view M {\n"
        "    int32 magic;\n"
        "    D[]   ds;\n"
        "}\n"
        "public final class E {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[8];\n"
        "        bytes[0] = 117440512;\n"      // magic = 7 (BE — M's order)\n"
        "        bytes[1] = 33554432;\n"       // count = 2 (BE — M's field)\n"
        "        bytes[2] = 5;\n"              // d0.s = 5 (LE — D's order)
        "        bytes[3] = 4;\n"              // d0.len = 4 (LE)
        "        bytes[4] = 1684234849;\n"     // \"abcd\"
        "        bytes[5] = 9;\n"              // d1.s = 9 (LE)
        "        bytes[6] = 4;\n"              // d1.len = 4 (LE)
        "        bytes[7] = 2054781047;\n"     // \"wxyz\"
        "        M m = M(bytes);\n"
        "        if (m.magic != 7) return 10;\n"
        "        if (m.ds.count() != 2) return 11;\n"
        "        if (m.ds[0].s != 5) return 12;\n"
        "        if (m.ds[1].s != 9) return 13;\n"
        "        String n0 = m.ds[0].name;\n"
        "        if (!n0.equals(\"abcd\")) return 14;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- 3. conflicting inheritance from two outers is rejected ----------------

TEST(ViewElementArrayEndianTests, ambiguousInheritanceRejected) {
    auto src =
        "package test;\n"
        "public view D {\n"                    // unannotated, used by both
        "    int32  s;\n"
        "    String name;\n"
        "}\n"
        "@BigEndian\n"
        "public view M1 { int32 a; D[] xs; }\n"
        "@LittleEndian\n"
        "public view M2 { int32 b; D[] ys; }\n"
        "public final class E {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.E");
        FAIL() << "expected CAJETA_ERROR_VIEW_ENDIAN_AMBIGUOUS";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_ENDIAN_AMBIGUOUS");
    }
}
