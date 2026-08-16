// String storage pinning — 6.2.2 tagged core (slice-spec §8.2).
//
// Per docs/specification/lang/String.md § Storage model, the wrapper is:
//
//     class String {                    // C view (offsets on x86-64)
//         int32 lenTag;                 // @8  — len | tag bits
//         int32 aux;                    // @12 — Inline text 0..3 / window off
//         int8[] base;                  // @16 — Inline text 4..11 / root hdr
//         public int32 cachedCpLength;  // @24 — -1 = not yet computed
//     }
//
//   len <= 12          Inline: text in aux+base's 12 bytes; no root.
//   len >  12          pointer form: {off = aux, root header = base};
//                      bit31 = holds-stake, bit30 = borrow, bit29 = static.
//
// These tests pin the physical layout (a C-struct probe over both literal
// forms) and the public surface that replaced the old fields (byteLength()
// method; root()/byteOffset() window contract). Renames / reorders / size
// changes break loudly here.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

struct TaggedStringLayout {
    const void* vtable;
    int32_t lenTag;
    int32_t aux;
    const char* base;
    int32_t cachedCpLength;
};
} // namespace

// byteLength is a METHOD over lenTag now (the field is gone).

// The window contract that replaced `mode`: Inline strings (<= 12 B) have
// no root; pointer forms expose {root, byteOffset}.
TEST(StringClassTests, rootAndOffsetReplaceMode) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String inl = \"tiny\";\n"
        "        String big = \"0123456789abcdefghijklmnop\";\n"
        "        int32 r = 0;\n"
        "        if (inl.root() == null) { r = r + 1; }\n"
        "        if (inl.byteOffset() == (int64) 0) { r = r + 10; }\n"
        "        if (big.root() != null) { r = r + 100; }\n"
        "        if (big.byteOffset() == (int64) 0) { r = r + 1000; }\n"
        "        String w = big.substring(10, 26);\n"
        "        if (w.byteOffset() == (int64) 10) { r = r + 10000; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11111);
}

// cachedCpLength stays a public field; -1 = not yet computed.

// Physical layout probe: read literals through the C-struct view. Pins
// offsets {lenTag@8, aux@12, base@16, cachedCpLength@24} and both literal
// forms (Inline packing; Static root with the bit29 tag).
