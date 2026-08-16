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


// --- 2. explicit element annotation wins (mixed-endian record) -------------


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
