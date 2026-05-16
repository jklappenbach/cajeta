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

// ---------------------------------------------------------------------
// S7.3 — chained field access through embedded structs. `obj.s.field`
// GEPs through the class layout into the struct layout in one chained
// operation. Same DotExpression chain that handles class-class field
// paths; struct embedded slots just become inline-LLVM-struct slots
// in the parent class layout.
// ---------------------------------------------------------------------

// Compound expression: read + write through the embedded struct in the
// same line. Verifies the GEP/load/store chain isn't disturbed by
// other intervening expression sites.
TEST(StructCompositionTests, embeddedStructReadAndWriteInSameExpression) {
    auto src =
        "package test;\n"
        "public struct Counter { int32 value; }\n"
        "public class Box {\n"
        "    public Counter c;\n"
        "    public Box() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box b = new Box();\n"
        "        b.c.value = 10;\n"
        "        b.c.value = b.c.value + 5;\n"
        "        return b.c.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Deeply nested chained access: class → embedded struct → embedded
// struct → primitive field. Three GEP layers, written and read.
TEST(StructCompositionTests, doublyNestedEmbeddedStructAccess) {
    auto src =
        "package test;\n"
        "public struct Inner { int32 leaf; }\n"
        "public struct Outer { Inner inner; }\n"
        "public class Container {\n"
        "    public Outer outer;\n"
        "    public Container() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Container c = new Container();\n"
        "        c.outer.inner.leaf = 77;\n"
        "        return c.outer.inner.leaf;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 77);
}

// ---------------------------------------------------------------------
// S7.4 — path-borrow tracking through embedded structs. Paths like
// `obj.embeddedStruct.classField` participate in the same Scope-based
// markMovedPath / isPathMoved machinery as class field paths; `#path`
// at any depth marks the prefix moved-from, and subsequent reads
// through it (or through any deeper extension) trip use-after-move.
// Same DotExpression::buildPath that handles every other dotted path.
// ---------------------------------------------------------------------

// Move out of an embedded struct's class-ref field. Reading the same
// path afterward must trip use-after-move.
TEST(StructCompositionTests, useAfterMoveOnPathThroughEmbeddedStruct) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 n;\n"
        "    public Tag() { this.n = 0; return; }\n"
        "}\n"
        "public struct Holder { Tag t; }\n"
        "public class Wrapper {\n"
        "    public Holder h;\n"
        "    public Wrapper() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Wrapper w = new Wrapper();\n"
        "        Tag tag = new Tag();\n"
        "        w.h.t = #tag;\n"
        "        Tag moved = #w.h.t;\n"  // transfer out of the embedded path
        "        return w.h.t.n;\n"      // path was moved
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_USE_AFTER_MOVE on w.h.t.n after #w.h.t";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
    }
}

// Moving a prefix marks all deeper extensions too. Here `#w.h.t`
// should poison reads of `w.h.t.n` (deeper than the moved prefix).
TEST(StructCompositionTests, prefixMoveBlocksDeeperPathReads) {
    auto src =
        "package test;\n"
        "public class Inner {\n"
        "    public int32 v;\n"
        "    public Inner() { this.v = 0; return; }\n"
        "}\n"
        "public struct Holder { Inner inner; }\n"
        "public class Wrapper {\n"
        "    public Holder h;\n"
        "    public Wrapper() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Wrapper w = new Wrapper();\n"
        "        Inner i = new Inner();\n"
        "        w.h.inner = #i;\n"
        "        Inner stolen = #w.h.inner;\n"  // move out the inner
        "        return w.h.inner.v;\n"          // any read through the moved prefix is poisoned
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_USE_AFTER_MOVE on w.h.inner.v after #w.h.inner";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_USE_AFTER_MOVE");
    }
}

// ---------------------------------------------------------------------
// S7.5 — remaining breadth: sizeof / layout pin, nested embedded
// (already covered by S7.3's doublyNestedEmbeddedStructAccess but
// extended here with classes-in-the-middle), and array-of-class
// elements where each class instance carries an embedded struct.
// ---------------------------------------------------------------------

// Layout-size pin via surrounding-field probe. Distinct unique values
// into `before`, the embedded struct's two fields, and `after`. If the
// embedded struct's slot were sized wrong, writes would leak across
// field boundaries and the read-back sum would diverge from the sum
// of literals.
TEST(StructCompositionTests, embeddedStructLayoutNoFieldOverlap) {
    auto src =
        "package test;\n"
        "public struct Pair { int32 x; int32 y; }\n"
        "public class Box {\n"
        "    public int32 before;\n"
        "    public Pair pair;\n"
        "    public int32 after;\n"
        "    public Box() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box b = new Box();\n"
        "        b.before = 1000;\n"
        "        b.pair.x = 200;\n"
        "        b.pair.y = 30;\n"
        "        b.after = 4;\n"
        "        return b.before + b.pair.x + b.pair.y + b.after;\n"  // 1234
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1234);
}

// S7.5's "array of embedded structs" test is deferred — it requires a
// class-array element-layout fix that's orthogonal to embedded-struct
// composition. CajetaArray::getElementLlvmType currently uses the
// element type's full LLVM struct (16 bytes for `Cell { Point pt; }`)
// for class elements, but the reader paths (loadIfLValue for
// ArrayIndexExpression) treat slots as 8-byte pointers. The mismatch
// silently misreads when class elements carry an embedded struct,
// because the post-S7 read picks up vtable bytes instead of the
// expected pointer. Fixing it correctly means deciding whether class
// arrays store inline class values or pointer references — a wider
// design call. Captured under "S7.5 limitations called out" in the
// rollout doc.

// Class containing a struct that contains a class ref. End-to-end:
// alloc Class → allocates the embedded struct inline → struct's class
// ref slot starts null → user assigns a heap Tag → class drop walks
// struct drop → struct drop frees Tag. No leaks, no double-frees.
// Combines layout, access, drop, and ownership-transfer pieces.
TEST(StructCompositionTests, classStructClassEndToEnd) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 n;\n"
        "    public Tag() { this.n = 0; return; }\n"
        "}\n"
        "public struct Holder { Tag t; int32 extra; }\n"
        "public class Container {\n"
        "    public Holder h;\n"
        "    public Container() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Container c = new Container();\n"
        "        Tag tag = new Tag();\n"
        "        tag.n = 41;\n"
        "        c.h.t = #tag;\n"
        "        c.h.extra = 1;\n"
        "        return c.h.t.n + c.h.extra;\n"  // 42
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Sibling paths (not the moved one) stay readable. The move-mark on
// `w.p.a` must NOT poison reads of `w.p.b`. Compile-only check —
// reaching this point means buildPath / isPathMoved scoped the move
// correctly to the specific path string. (The full run-and-return
// shape would also exercise a runtime drop-chain on the moved-out
// struct field, which is a separate gap: struct drop walks the
// post-move slot as if it still owned the original pointer. Tracked
// under S7.4 limitations in the rollout doc.)
TEST(StructCompositionTests, siblingPathStaysReadableAfterMove) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 n;\n"
        "    public Tag() { this.n = 7; return; }\n"
        "}\n"
        "public struct Pair { Tag a; Tag b; }\n"
        "public class Wrapper {\n"
        "    public Pair p;\n"
        "    public Wrapper() { return; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Wrapper w = new Wrapper();\n"
        "        Tag a = new Tag();\n"
        "        Tag b = new Tag();\n"
        "        w.p.a = #a;\n"
        "        w.p.b = #b;\n"
        "        Tag stolen = #w.p.a;\n"
        // w.p.a is moved-from; the read below targets w.p.b which is
        // a different path — must compile.
        "        int32 ignored = w.p.b.n;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
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
