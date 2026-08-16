//
// S5 — multi-trailing variable-size view fields + length-prefix validation.
//
// A view may now declare multiple variable-size fields (String today;
// T[] support deferred). Wire layout:
//   [ fixed prefix bytes ][ S0.prefix(4) S0.data(N0) ][ S1.prefix(4) S1.data(N1) ]...
//
// Field access for the Kth variable-size field walks K prior prefixes at
// runtime to find its own. The view constructor validates every length-
// prefix at construction (sweep once, free per-access).
//
// Test data note: the wire layout is BYTE-PACKED (no alignment padding).
// All test lengths below are multiples of 4 so the int32[] buffer's
// element boundaries align with field boundaries — keeps the hand-packed
// numeric literals readable.
//
// Out of scope (deferred to S5b — see docs/history/StructsViewsStatus.md):
//   - Fixed fields after a variable-size field (needs runtime offset cache)
//   - T[] as a variable-size view field (needs per-element-size runtime helper)
//   - Nested variable-size view in another view's tail
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Multi-trailing String fields ------------------------------------------




TEST(MultiVarSizeViewTests, threeTrailingStrings) {
    // Three var-size fields, all 4-byte aligned. Access the third one;
    // codegen walks two prior prefixes.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view T {\n"
        "    String a;\n"
        "    String b;\n"
        "    String c;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        // Layout (no fixed prefix since no fixed fields):
        //   a.len(4) = 4, a.data(4) = "AAAA"
        //   b.len(4) = 4, b.data(4) = "BBBB"
        //   c.len(4) = 4, c.data(4) = "CCCC"
        // 24 bytes total = int32[6].
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 4;\n"
        "        bytes[1] = 1094795585;\n"  // "AAAA" = 0x41414141
        "        bytes[2] = 4;\n"
        "        bytes[3] = 1111638594;\n"  // "BBBB" = 0x42424242
        "        bytes[4] = 4;\n"
        "        bytes[5] = 1128481603;\n"  // "CCCC" = 0x43434343
        "        T t = T(bytes);\n"
        "        String cc = t.c;\n"
        "        if (cc.equals(\"CCCC\")) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- Length-prefix validation ----------------------------------------------




