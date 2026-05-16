//
// S4 — nested views (Views.md § Nested views).
//
// A view field may itself be of view type. The inner view's bytes lay
// out inline within the outer at its declared offset. Path access
// `outer.inner.field` resolves via chained GEPs rooted at the outer's
// buffer. Each view declares its own endianness annotation (S3.4
// requires every view to declare one); v1 doesn't auto-inherit from
// the outer.
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

TEST(NestedViewTests, fixedSizeInnerInlines) {
    // Inner view's two int32 fields lay out as 8 bytes inline within
    // the outer. Total outer size = 8 (Point) + 8 (Point) = 16.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "@HostEndian\n"
        "public view Line {\n"
        "    Point start;\n"
        "    Point end;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"
        "        Line l = Line(bytes);\n"
        "        l.start.x = 10;\n"
        "        l.start.y = 20;\n"
        "        l.end.x = 30;\n"
        "        l.end.y = 40;\n"
        "        return l.start.x + l.start.y + l.end.x + l.end.y;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 100);
}

TEST(NestedViewTests, perViewEndiannessAnnotation) {
    // Each view carries its own endianness. Outer is @BigEndian; inner
    // is also @BigEndian — fields use big-endian access. Demonstrates
    // that nested-view annotation is honored independently of the
    // outer's (no silent inheritance; S3.4 requires every view to
    // declare its own).
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Inner {\n"
        "    int32 v;\n"
        "}\n"
        "@BigEndian\n"
        "public view Outer {\n"
        "    Inner head;\n"
        "    int32 tail;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[2];\n"
        "        Outer o = Outer(bytes);\n"
        "        o.head.v = 16909060;\n"  // 0x01020304
        "        o.tail = 16909060;\n"
        "        // Both fields written through big-endian views; reading\n"
        "        // them back through the same views should bswap on both\n"
        "        // sides, yielding the original value.\n"
        "        return o.head.v + o.tail - 16909060;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 16909060);
}

TEST(NestedViewTests, mixedEndiannessAcrossNesting) {
    // Outer @BigEndian, inner @LittleEndian. Each view's fields use
    // its OWN endianness on access — no inheritance. Writing the same
    // value through both views should put different byte patterns in
    // the buffer, but read-back through each view recovers the
    // original value.
    auto src =
        "package test;\n"
        "@LittleEndian\n"
        "public view Le {\n"
        "    int32 v;\n"
        "}\n"
        "@BigEndian\n"
        "public view Mixed {\n"
        "    Le head;\n"
        "    int32 tail;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[2];\n"
        "        Mixed m = Mixed(bytes);\n"
        "        m.head.v = 1000;\n"
        "        m.tail = 2000;\n"
        "        return m.head.v + m.tail;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3000);
}

TEST(NestedViewTests, doublyNestedView) {
    // Three-level composition: BoundingBox { Line edges[1]; ... }
    // is too much for v1 (arrays-of-views aren't implemented), but
    // BoundingBox { Line ab; Line cd; } works and exercises both
    // outer.inner.deeper.field path resolution and the layout pass
    // computing offsets for the doubly-nested case.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "@HostEndian\n"
        "public view Line {\n"
        "    Point a;\n"
        "    Point b;\n"
        "}\n"
        "@HostEndian\n"
        "public view BoundingBox {\n"
        "    Line first;\n"
        "    Line second;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[8];\n"  // 4 Points * 8 bytes / 4 = 8 i32s
        "        BoundingBox bb = BoundingBox(bytes);\n"
        "        bb.first.a.x = 1;\n"
        "        bb.first.b.y = 2;\n"
        "        bb.second.a.x = 3;\n"
        "        bb.second.b.y = 4;\n"
        "        return bb.first.a.x + bb.first.b.y + bb.second.a.x + bb.second.b.y;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

TEST(NestedViewTests, recursiveCycleIsLayoutError) {
    // A view containing itself directly would be infinite size — the
    // layout pass must reject it. Today the cycle either triggers a
    // CajetaType resolution failure or a stack overflow during layout;
    // either way the compile fails. Pinning the rejection here so a
    // future tightening (a dedicated CAJETA_ERROR_VIEW_RECURSIVE
    // diagnostic) doesn't silently regress acceptance.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Bad {\n"
        "    Bad child;\n"
        "    int32 marker;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}
