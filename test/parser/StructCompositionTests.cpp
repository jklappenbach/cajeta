//
// Session 7 — inline composition: structs embedded as class fields.
//
// Per StructsViewsStatus.md S7, when a class has a struct-typed field the
// struct's body lays out INLINE inside the class's heap allocation (not
// as a pointer). Field access chains GEP through the class layout into
// the struct layout in one operation. Class drops recurse into the
// embedded struct's drop fn before freeing the heap block. Path-borrow
// tracking extends the existing dotted-path machinery to paths through
// embedded structs.
//
// This file pins the layout, access, drop, and borrow shapes across the
// S7.1–S7.5 sub-tasks.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// run/read pattern for drop-count assertions — drops fire at the
// producer method's exit, so a separate method reads the post-drop
// count. Same shape as the helper used in StructStackTests / ClassDropTests.
int64_t observeCompositionDrops(const std::string& topLevel,
                                 const std::string& runBody) {
    std::string src =
        std::string("package test;\n") + topLevel +
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Cajeta.dropCountReset();\n"
        "        " + runBody + "\n"
        "        return 0;\n"
        "    }\n"
        "    public static int64 read() {\n"
        "        return Cajeta.dropCount();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    jit->lookup<int32_t (*)()>("run")();
    return jit->lookup<int64_t (*)()>("read")();
}

} // namespace

// ---------------------------------------------------------------------
// S7.1 — class layout embeds struct fields inline. The struct's LLVM
// body type appears in the class struct at the field's offset, not a
// pointer slot. Visible behavior: writes to the embedded struct's
// fields land in the class's heap allocation directly.
// ---------------------------------------------------------------------

// Class with one embedded primitive-only struct field. Construct, write
// through the embedded struct, read back. The embedded struct's bytes
// live inside the class instance — no separate heap allocation needed.
TEST(StructCompositionTests, classWithEmbeddedPrimitiveStruct) {
    auto src =
        "package test;\n"
        "public struct Point { int32 x; int32 y; }\n"
        "public class Holder {\n"
        "    public Point pt;\n"
        "    public int32 tag;\n"
        "    public Holder() { this.tag = 0; return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = new Holder();\n"
        "        h.pt.x = 7;\n"
        "        h.pt.y = 11;\n"
        "        h.tag = 100;\n"
        "        return h.pt.x + h.pt.y + h.tag;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 118);
}

// Embedded struct surrounded by primitive fields. The class's layout
// must not bleed bytes between fields — writes to `tag` after the
// struct and `version` before it shouldn't disturb the struct's body.
TEST(StructCompositionTests, embeddedStructWithSurroundingFields) {
    auto src =
        "package test;\n"
        "public struct Point { int32 x; int32 y; }\n"
        "public class Holder {\n"
        "    public int32 version;\n"
        "    public Point pt;\n"
        "    public int32 tag;\n"
        "    public Holder() { this.version = 1; this.tag = 0; return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Holder h = new Holder();\n"
        "        h.pt.x = 3;\n"
        "        h.pt.y = 4;\n"
        "        h.tag = 50;\n"
        "        return h.version + h.pt.x + h.pt.y + h.tag;\n"  // 1+3+4+50
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 58);
}

// Two embedded structs in the same class. Each gets its own inline slot;
// writes to one don't leak into the other.
TEST(StructCompositionTests, classWithTwoEmbeddedStructs) {
    auto src =
        "package test;\n"
        "public struct Point { int32 x; int32 y; }\n"
        "public class Pair {\n"
        "    public Point a;\n"
        "    public Point b;\n"
        "    public Pair() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair p = new Pair();\n"
        "        p.a.x = 10;\n"
        "        p.a.y = 20;\n"
        "        p.b.x = 100;\n"
        "        p.b.y = 200;\n"
        "        return p.a.x + p.a.y + p.b.x + p.b.y;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 330);
}
