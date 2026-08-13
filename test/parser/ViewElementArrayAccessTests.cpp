//
// VEA-3 — view element arrays: element access, length, bounds, write-through.
//
// Spec: specs/view-element-arrays-spec.md §3.2. `outer.f[i]` yields a view
// of the element type borrowing the outer's buffer (no copy), `outer.f.length`
// is the element count, out-of-range indexing throws, and writing a fixed
// field of an element writes through to the buffer. Element access is O(1)
// via the offset table the constructor fills (VEA-2's sweep records element
// start offsets; this unit makes them observable).
//
// Wire layouts mirror ViewElementArrayCtorTests (all @HostEndian, int32-
// aligned so hand-packed int32[] literals stay readable):
//
//   view D { int32 s; String name; }        // [s:4][len:4][name:len]
//   view M { int32 magic; D[] ds; }         // [magic:4][count:4][elements...]
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.A");
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

// [magic=7][count=2][d0.s=5][d0.len=4]["abcd"][d1.s=9][d1.len=4]["wxyz"]
const char* kGoldenFill =
    "        int32[] bytes = heap int32[8];\n"
    "        bytes[0] = 7;\n"
    "        bytes[1] = 2;\n"
    "        bytes[2] = 5;\n"
    "        bytes[3] = 4;\n"
    "        bytes[4] = 1684234849;\n"   // "abcd"
    "        bytes[5] = 9;\n"
    "        bytes[6] = 4;\n"
    "        bytes[7] = 2054781047;\n"   // "wxyz"
    "        M m = M(bytes);\n";

} // namespace

// --- c.1: element access reads golden values -------------------------------

TEST(ViewElementArrayAccessTests, elementFixedFieldGolden) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        if (m.ds[0].s != 5) return 10;\n"
        "        if (m.ds[1].s != 9) return 11;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(ViewElementArrayAccessTests, elementStringFieldGolden) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        String n0 = m.ds[0].name;\n"
        "        String n1 = m.ds[1].name;\n"
        "        if (!n0.equals(\"abcd\")) return 10;\n"
        "        if (!n1.equals(\"wxyz\")) return 11;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// An element view bound to a local reads the same values (the element is a
// first-class view value, not only a chained-expression temporary).
TEST(ViewElementArrayAccessTests, elementViewAsLocal) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        D d = m.ds[1];\n"
        "        if (d.s != 9) return 10;\n"
        "        String n = d.name;\n"
        "        if (!n.equals(\"wxyz\")) return 11;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- c.2: length + bounds --------------------------------------------------

TEST(ViewElementArrayAccessTests, lengthReadsCount) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        if (m.ds.count() != 2) return 10;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(ViewElementArrayAccessTests, outOfRangeIndexThrows) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        try {\n"
        "            int32 x = m.ds[5].s;\n"
        "            return 99;\n"
        "        } catch (Exception e) {\n"
        "            return 7;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// --- c.3: String[] element read --------------------------------------------

TEST(ViewElementArrayAccessTests, stringArrayElementRead) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view N {\n"
        "    int32    kind;\n"
        "    String[] names;\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 3;\n"
        "        bytes[1] = 2;\n"
        "        bytes[2] = 4;\n"
        "        bytes[3] = 1684234849;\n"   // \"abcd\"
        "        bytes[4] = 4;\n"
        "        bytes[5] = 2054781047;\n"   // \"wxyz\"
        "        N n = N(bytes);\n"
        "        if (n.names.count() != 2) return 10;\n"
        "        String s1 = n.names[1];\n"
        "        if (!s1.equals(\"wxyz\")) return 11;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- c.4: write-through on element fixed fields ----------------------------

TEST(ViewElementArrayAccessTests, elementFixedFieldWriteThrough) {
    auto src = std::string(kViews) +
        "public final class A {\n"
        "    public static int32 run() {\n"
        + kGoldenFill +
        "        m.ds[1].s = 42;\n"
        "        if (m.ds[1].s != 42) return 10;\n"
        "        if (bytes[5] != 42) return 11;\n"   // wrote through to buffer
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- c.5: fixed-size element stride path -----------------------------------

TEST(ViewElementArrayAccessTests, fixedElementStrideGolden) {
    // view Pt { int32 x; int32 y; } — fixed 8-byte elements; offsets are
    // stride math (no per-element table entries).
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Pt { int32 x; int32 y; }\n"
        "@HostEndian\n"
        "public view Poly {\n"
        "    int32 kind;\n"
        "    Pt[]  points;\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 1;\n"              // kind
        "        bytes[1] = 2;\n"              // count
        "        bytes[2] = 10;\n"             // points[0].x
        "        bytes[3] = 20;\n"             // points[0].y
        "        bytes[4] = 30;\n"             // points[1].x
        "        bytes[5] = 40;\n"             // points[1].y
        "        Poly p = Poly(bytes);\n"
        "        if (p.points.count() != 2) return 10;\n"
        "        if (p.points[1].x != 30) return 11;\n"
        "        if (p.points[1].y != 40) return 12;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- post-array fixed field read (the walk-crossing case) ------------------

// Reading a fixed field declared AFTER an element array requires crossing
// the elements — via the table's end offset, not a re-walk.
TEST(ViewElementArrayAccessTests, postArrayFixedFieldRead) {
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
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[6];\n"
        "        bytes[0] = 7;\n"
        "        bytes[1] = 1;\n"
        "        bytes[2] = 5;\n"
        "        bytes[3] = 4;\n"
        "        bytes[4] = 1684234849;\n"
        "        bytes[5] = 12345;\n"          // checksum
        "        P p = P(bytes);\n"
        "        if (p.checksum != 12345) return 10;\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// --- element views may not carry their own element arrays ------------------
//
// Element arrays are single-level by spec (view-element-arrays-spec):
// an element view that itself declares an element-array field is rejected
// at declaration with a diagnostic naming the outer view, the field, and
// the offending inner field — not at first access, and not silently.
TEST(ViewElementArrayAccessTests, elementViewWithOwnElementArrayRejected) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Inner {\n"
        "    int32    s;\n"
        "    String[] tags;\n"
        "}\n"
        "@HostEndian\n"
        "public view M {\n"
        "    int32   magic;\n"
        "    Inner[] items;\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.A");
        FAIL() << "expected CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY");
        EXPECT_NE(e.getMessage().find("items"), std::string::npos)
            << e.getMessage();
        EXPECT_NE(e.getMessage().find("tags"), std::string::npos)
            << e.getMessage();
    }
}
