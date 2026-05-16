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
// Out of scope (deferred to S5b — see cajeta-docs/history/StructsViewsStatus.md):
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

TEST(MultiVarSizeViewTests, multipleTrailingStringsCompile) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "    String last;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

TEST(MultiVarSizeViewTests, firstStringReadCorrectly) {
    // Layout (each int32 is 4 bytes, all lengths are 4 so offsets align):
    //   bytes[0] = id (i32) = 7
    //   bytes[1] = first.length = 4
    //   bytes[2] = "abcd" (LE: 'a'=0x61, 'b'=0x62, 'c'=0x63, 'd'=0x64 → 0x64636261 = 1684234849)
    //   bytes[3] = last.length = 4
    //   bytes[4] = "wxyz" (LE: 0x7A797877 = 2054781047)
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "    String last;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[5];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 4;\n"
        "        bytes[2] = 1684234849;\n"  // "abcd"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 2054781047;\n"  // "wxyz"
        "        R r = R(bytes);\n"
        "        String f = r.first;\n"
        "        if (f.equals(\"abcd\")) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(MultiVarSizeViewTests, secondStringReadByWalkingFirstPrefix) {
    // Same layout as above; this time read the SECOND string. The codegen
    // must walk past the first length-prefix at runtime to find the second.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "    String last;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[5];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 4;\n"
        "        bytes[2] = 1684234849;\n"  // "abcd"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 2054781047;\n"  // "wxyz"
        "        R r = R(bytes);\n"
        "        String l = r.last;\n"
        "        if (l.equals(\"wxyz\")) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

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
        "        int32[] bytes = new int32[6];\n"
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

TEST(MultiVarSizeViewTests, oversizeLengthPrefixThrowsAtConstruction) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[3];\n"  // 12 bytes total
        "        bytes[0] = 7;\n"
        "        bytes[1] = 1000000;\n"              // way too big
        "        try {\n"
        "            R r = R(bytes);\n"
        "            return 0;\n"
        "        } catch (Throwable t) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(MultiVarSizeViewTests, exactFitLengthAccepted) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[3];\n"   // exactly 12 bytes
        "        bytes[0] = 7;\n"
        "        bytes[1] = 4;\n"                     // length = 4 fills trailing 4 bytes
        "        bytes[2] = 1684234849;\n"           // "abcd"
        "        R r = R(bytes);\n"
        "        if (r.first.equals(\"abcd\")) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(MultiVarSizeViewTests, oneByteShortRejected) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    int32 id;\n"
        "    String first;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[3];\n"   // 12 bytes
        "        bytes[0] = 7;\n"
        "        bytes[1] = 5;\n"                     // length = 5, needs 13 bytes
        "        try {\n"
        "            R r = R(bytes);\n"
        "            return 0;\n"
        "        } catch (Throwable t) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(MultiVarSizeViewTests, secondVarSizePrefixValidatedToo) {
    // Two var-size fields. The first's length is valid; the second's
    // would overrun. Sweep should catch the second.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view R {\n"
        "    String a;\n"
        "    String b;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"   // 16 bytes
        "        bytes[0] = 4;\n"                     // a.length = 4 (offsets 4..8)
        "        bytes[1] = 0;\n"                     // a.data = 4 bytes
        "        bytes[2] = 100;\n"                   // b.length = 100 (overrun)
        "        try {\n"
        "            R r = R(bytes);\n"
        "            return 0;\n"
        "        } catch (Throwable t) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
