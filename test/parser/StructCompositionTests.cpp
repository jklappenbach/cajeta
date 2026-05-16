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

// ---------------------------------------------------------------------
// S7.2 — class drop recurses into embedded struct fields. For each
// CajetaStruct-typed field of the class, the class's drop fn calls
// the struct's drop fn (which in turn calls drop on its own class-ref
// fields) BEFORE freeing the heap block. Order is reverse declaration
// per Structs.md § Drop chain.
// ---------------------------------------------------------------------

// Class drop must call the embedded struct's drop fn so the struct's
// owned class refs are released. Without the recursion, the embedded
// Tracer would leak — its destructor wouldn't run. Observable drops:
//   1 (class instance's chain entry firing)
//   1 (Tracer's destructor allocates int32[3]; that array drops when
//      ~Tracer scope exits)
// = 2. A missing class→struct→class drop recursion would yield 1
// (just the class entry, with the Tracer leaked).
//
// We populate the embedded slot via per-field assignment with `#`-move
// rather than `w.h = Holder { ... }` (the latter is an open codegen gap:
// aggregate-init returns a body pointer that doesn't copy cleanly into
// an inline embedded slot — separate from S7.2 proper, tracked as a
// follow-up).
TEST(StructCompositionTests, classDropRecursesIntoEmbeddedStruct) {
    EXPECT_EQ(observeCompositionDrops(
        "public class Tracer {\n"
        "    public Tracer() { return; }\n"
        "    public ~Tracer() {\n"
        "        int32[] arr = new int32[3];\n"
        "    }\n"
        "}\n"
        "public struct Holder { Tracer t; }\n"
        "public class Wrapper {\n"
        "    public Holder h;\n"
        "    public Wrapper() { return; }\n"
        "}\n",
        "Wrapper w = new Wrapper();\n"
        "        Tracer tracer = new Tracer();\n"
        "        w.h.t = #tracer;"
    ), 2);
}

// Class with two embedded structs, each owning a Tracer. Both structs'
// drops fire, both ~Tracer() destructors run. Observed:
//   1 (Wrapper class drop entry)
//   2 (two array drops, one from each ~Tracer)
// = 3.
TEST(StructCompositionTests, classDropFiresAllEmbeddedStructDrops) {
    EXPECT_EQ(observeCompositionDrops(
        "public class Tracer {\n"
        "    public Tracer() { return; }\n"
        "    public ~Tracer() {\n"
        "        int32[] arr = new int32[3];\n"
        "    }\n"
        "}\n"
        "public struct Single { Tracer t; }\n"
        "public class Wrapper {\n"
        "    public Single first;\n"
        "    public Single second;\n"
        "    public Wrapper() { return; }\n"
        "}\n",
        "Wrapper w = new Wrapper();\n"
        "        Tracer a = new Tracer();\n"
        "        Tracer b = new Tracer();\n"
        "        w.first.t = #a;\n"
        "        w.second.t = #b;"
    ), 3);
}

// All three embedded class refs across two embedded structs' slots
// fire their destructors. Observed:
//   1 (Wrapper class drop entry)
//   3 (one ~Tracer array drop per Tracer)
// = 4. Pins the "all struct drops + all referent class drops fire"
// invariant for a wider shape.
TEST(StructCompositionTests, classDropEmbeddedStructsAllRun) {
    EXPECT_EQ(observeCompositionDrops(
        "public class Tracer {\n"
        "    public Tracer() { return; }\n"
        "    public ~Tracer() {\n"
        "        int32[] arr = new int32[2];\n"
        "    }\n"
        "}\n"
        "public struct Pair { Tracer left; Tracer right; }\n"
        "public class Wrapper {\n"
        "    public Pair p;\n"
        "    public Pair q;\n"
        "    public Wrapper() { return; }\n"
        "}\n",
        "Wrapper w = new Wrapper();\n"
        "        Tracer l = new Tracer();\n"
        "        Tracer r = new Tracer();\n"
        "        Tracer x = new Tracer();\n"
        "        w.p.left = #l;\n"
        "        w.p.right = #r;\n"
        "        w.q.left = #x;"
    ), 4);
}
