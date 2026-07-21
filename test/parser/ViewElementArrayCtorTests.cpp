//
// VEA-2 — view element arrays: construction validation sweep + offset table.
//
// Spec: specs/view-element-arrays-spec.md §3.3. Constructing a view with a
// `V[]` / `String[]` field reads the u32 count, then per element recursively
// validates the element's internal length-prefixes, recording element start
// offsets (the O(1)-access table, exercised by VEA-3). A malformed buffer —
// truncated count, truncated element, or an inner prefix overrunning the
// buffer — throws from the constructor.
//
// Failure surface: same as every view-ctor failure today — a runtime throw
// catchable as `Exception` in Cajeta (`__cajeta_throw` tag; the typed
// ParseException upgrade is a view-wide follow-up, not part of this plan).
// Tests use the StructViewBoundsTests convention: construct inside
// try/catch, sentinel 7 = threw, 99 = constructed when it should not have.
//
// Wire layout used throughout (all @HostEndian, fields int32-aligned so the
// hand-packed int32[] literals stay readable — same trick as
// MultiVarSizeViewTests):
//
//   view D { int32 s; String name; }        // [s:4][len:4][name:len]
//   view M { int32 magic; D[] ds; }         // [magic:4][count:4][elements...]
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* kViews =
    "package test;\n"
    "@HostEndian\n"
    "public view D {\n"
    "    int32  s;\n"
    "    String name;\n"
    "}\n"
    "@HostEndian\n"
    "public view M {\n"
    "    int32 magic;\n"
    "    D[]   ds;\n"
    "}\n";

} // namespace

// --- b.1: golden well-formed buffer constructs -----------------------------

TEST(ViewElementArrayCtorTests, validSweepAccepts) {
    // [magic=7][count=2][d0.s=5][d0.len=4]["abcd"][d1.s=9][d1.len=4]["wxyz"]
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[8];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 2;\n"
        "        bytes[2] = 5;\n"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 1684234849;\n"   // \"abcd\"
        "        bytes[5] = 9;\n"
        "        bytes[6] = 4;\n"
        "        bytes[7] = 2054781047;\n"   // \"wxyz\"
        "        M m = M(bytes);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Zero elements is a valid frame (count=0, nothing after).
TEST(ViewElementArrayCtorTests, zeroElementsAccepted) {
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 0;\n"
        "        M m = M(bytes);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- b.2: malformed buffers throw from the constructor ---------------------

// Buffer ends before the count prefix (only the fixed prefix present).
TEST(ViewElementArrayCtorTests, truncatedCountThrows) {
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[1];\n"   // magic only — no count
        "        bytes[0] = 7;\n"
        "        try {\n"
        "            M m = M(bytes);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// count says 2 but the buffer ends after element 0 — element 1 is missing.
TEST(ViewElementArrayCtorTests, truncatedElementThrows) {
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[5];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 2;\n"              // claims two elements
        "        bytes[2] = 5;\n"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 1684234849;\n"     // \"abcd\" — buffer ends here
        "        try {\n"
        "            M m = M(bytes);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// An element's INNER String prefix overruns the buffer — the case the
// pre-VEA-2 sweep provably does not look at (it treated the whole D[] as
// one `4 + prefix` blob). This is the CVE-class hole for wire parsers.
TEST(ViewElementArrayCtorTests, innerPrefixOverrunThrows) {
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[5];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 1;\n"
        "        bytes[2] = 5;\n"
        "        bytes[3] = 2147483647;\n"     // d0.name.len = INT32_MAX
        "        bytes[4] = 1684234849;\n"
        "        try {\n"
        "            M m = M(bytes);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// A negative count is malformed (u32 semantics: reject the sign bit rather
// than walk 2^31 elements).
TEST(ViewElementArrayCtorTests, negativeCountThrows) {
    auto src = std::string(kViews) +
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[2];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = -1;\n"
        "        try {\n"
        "            M m = M(bytes);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- String[] flavor -------------------------------------------------------

TEST(ViewElementArrayCtorTests, stringArrayGoldenAndOverrun) {
    // view N { int32 kind; String[] names; }
    // golden: [kind=3][count=2][len=4]["abcd"][len=4]["wxyz"] → constructs.
    // overrun: second len = INT32_MAX → throws.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view N {\n"
        "    int32    kind;\n"
        "    String[] names;\n"
        "}\n"
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] good = heap int32[6];\n"
        "        good[0] = 3;\n"
        "        good[1] = 2;\n"
        "        good[2] = 4;\n"
        "        good[3] = 1684234849;\n"
        "        good[4] = 4;\n"
        "        good[5] = 2054781047;\n"
        "        N n = N(good);\n"
        "        int32[] bad = heap int32[6];\n"
        "        bad[0] = 3;\n"
        "        bad[1] = 2;\n"
        "        bad[2] = 4;\n"
        "        bad[3] = 1684234849;\n"
        "        bad[4] = 2147483647;\n"
        "        bad[5] = 2054781047;\n"
        "        try {\n"
        "            N n2 = N(bad);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- latent S5b advance bug: multi-byte T[] --------------------------------

// A T[] prefix is an ELEMENT COUNT (the read path copies count*elemSize
// bytes), but the pre-VEA-2 sweep advanced by `4 + count` — correct only
// for int8[]. Layout below is per Views.md (count*4 data bytes for
// int32[]); the wrong math would read `s.len` from inside xs's data
// (planted INT32_MAX) and reject this valid buffer.
TEST(ViewElementArrayCtorTests, multiByteArrayAdvanceValidated) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Q {\n"
        "    int32   kind;\n"
        "    int32[] xs;\n"
        "    String  s;\n"
        "}\n"
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 3;\n"              // kind
        "        bytes[1] = 2;\n"              // xs.count = 2
        "        bytes[2] = 2147483647;\n"     // xs[0] — traps the wrong math
        "        bytes[3] = 11;\n"             // xs[1]
        "        bytes[4] = 4;\n"              // s.len = 4
        "        bytes[5] = 1684234849;\n"     // \"abcd\"
        "        Q q = Q(bytes);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- fixed field after an element array ------------------------------------

// view P { int32 magic; D[] ds; int32 checksum; } — the trailing fixed field
// must fit AFTER the elements; a buffer that ends exactly at the last
// element (no room for checksum) throws.
TEST(ViewElementArrayCtorTests, postArrayFixedFieldValidated) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view D {\n"
        "    int32  s;\n"
        "    String name;\n"
        "}\n"
        "@HostEndian\n"
        "public view P {\n"
        "    int32 magic;\n"
        "    D[]   ds;\n"
        "    int32 checksum;\n"
        "}\n"
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        int32[] good = heap int32[6];\n"
        "        good[0] = 7;\n"
        "        good[1] = 1;\n"
        "        good[2] = 5;\n"
        "        good[3] = 4;\n"
        "        good[4] = 1684234849;\n"
        "        good[5] = 42;\n"              // checksum fits
        "        P p = P(good);\n"
        "        int32[] bad = heap int32[5];\n"
        "        bad[0] = 7;\n"
        "        bad[1] = 1;\n"
        "        bad[2] = 5;\n"
        "        bad[3] = 4;\n"
        "        bad[4] = 1684234849;\n"       // ends here — no checksum room
        "        try {\n"
        "            P p2 = P(bad);\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
