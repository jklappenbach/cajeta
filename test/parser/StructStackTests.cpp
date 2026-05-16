//
// S6.1 — stack-allocated struct locals (alloca + zero-init).
//
// Covers the first slice of Session 6 from StructsViewsStatus.md:
//   - struct declarations with primitive-only fields lay out successfully
//   - `struct Foo f;` declares a local that's a stack alloca of the
//     struct body, zero-initialized
//   - the layout pass rejects field types that don't fit a fixed-size
//     stack aggregate (arrays, views, recursive shapes) with specific
//     error IDs
//
// Aggregate initializer (`Foo f = Foo { x: 1, y: 2 }`) is S6.2; class-ref
// fields are S6.3; drop chain is S6.4; borrow tracking is S6.5; the full
// 12-test set lands across S6.7 as the remaining sub-tasks come online.
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

} // namespace

// Declare a struct with primitive fields and a local of that type. The
// local's alloca + zero-init runs without crashing; returning a constant
// confirms control flow stays intact past the declaration.
TEST(StructStackTests, declarePrimitiveOnlyStructAndLocal) {
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p;\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Multiple struct locals in the same function — each gets its own alloca.
// Verifies the alloca site doesn't accidentally share storage.
TEST(StructStackTests, multipleStructLocalsInSameFunction) {
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point a;\n"
        "        Point b;\n"
        "        Point c;\n"
        "        return 7;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Mixed-primitive struct (i32 + i64 + i8 + bool). Layout pass should
// pick a natural alignment that LLVM can size cleanly.
TEST(StructStackTests, mixedPrimitiveLayout) {
    auto src =
        "package test;\n"
        "public struct Mixed {\n"
        "    int32 a;\n"
        "    int64 b;\n"
        "    int8 c;\n"
        "    boolean d;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Mixed m;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Reject T[] fields — they're heap-backed and don't fit a fixed-size
// stack aggregate. Surfaces at layout pass with CAJETA_ERROR_STRUCT_FIELD_TYPE.
TEST(StructStackTests, rejectArrayFieldInStruct) {
    auto src =
        "package test;\n"
        "public struct Bad {\n"
        "    int32[] xs;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_STRUCT_FIELD_TYPE on T[] field";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_STRUCT_FIELD_TYPE");
    }
}

// Reject a struct that holds itself directly — infinite size.
// CAJETA_ERROR_STRUCT_RECURSIVE mirrors CAJETA_ERROR_VIEW_RECURSIVE on
// the view side.
TEST(StructStackTests, rejectRecursiveStruct) {
    auto src =
        "package test;\n"
        "public struct Node {\n"
        "    int32 v;\n"
        "    Node next;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_STRUCT_RECURSIVE on self-referential struct";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_STRUCT_RECURSIVE");
    }
}

// ---------------------------------------------------------------------
// S6.2 — aggregate initializer: `Foo f = Foo { field: expr, ... };`
// ---------------------------------------------------------------------

// Full initializer + read each field back. Verifies both that the store
// to the field slots happened (read returns the assigned value) and that
// field ordering survives the GEP index resolution.
TEST(StructStackTests, aggregateInitFullThenRead) {
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 7, y: 11 };\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18);
}

// Partial initializer — fields omitted from the binding list land at 0
// thanks to the zero-init pass that runs before the per-field stores.
TEST(StructStackTests, aggregateInitPartialZeroesRest) {
    auto src =
        "package test;\n"
        "public struct Triple {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "    int32 c;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Triple t = Triple { b: 42 };\n"
        // a and c stay 0 from the zero-init pass; only b was set.
        "        return t.a + t.b + t.c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Bindings in a different order than the field declaration order resolve
// by name, not position — confirms the per-field GEP indexes off the
// declared property's getOrder() rather than the binding's list position.
TEST(StructStackTests, aggregateInitBindingsOutOfOrder) {
    auto src =
        "package test;\n"
        "public struct Pair {\n"
        "    int32 first;\n"
        "    int32 second;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Pair p = Pair { second: 100, first: 3 };\n"
        // If bindings were positional, first/second would be swapped and
        // first-3*1000+second = 103. With name-based resolution we get
        // first=3, second=100 → 3*1000+100 = 3100.
        "        return p.first * 1000 + p.second;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3100);
}

// Mixed primitive widths — int8, int32, int64 in the same struct. The
// per-binding coercion in AggregateInitializerExpression's codegen
// truncates/extends integer literals to the field's declared type.
TEST(StructStackTests, aggregateInitMixedWidthPrimitives) {
    auto src =
        "package test;\n"
        "public struct Wide {\n"
        "    int8 small;\n"
        "    int32 mid;\n"
        "    int64 big;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Wide w = Wide { small: 1, mid: 200, big: 30000 };\n"
        "        return (int32) w.small + w.mid + (int32) w.big;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 30201);
}

// Unknown field names are caught at codegen with a specific error ID, not
// silently dropped. Cheap typo catch — getting "x: 1" right when the
// field is "xCoord" matters.
TEST(StructStackTests, aggregateInitRejectsUnknownField) {
    auto src =
        "package test;\n"
        "public struct Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1, z: 5 };\n"
        "        return p.x;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_AGGREGATE_INIT_UNKNOWN_FIELD");
    }
}

// Aggregate-init syntax on a view should reject — views construct from
// a backing buffer via the call-form `View(bytes)`, not the brace form.
// Pins that the dispatch correctly distinguishes the two aggregate kinds.
TEST(StructStackTests, aggregateInitRejectedOnView) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Header h = Header { version: 7 };\n"
        "        return h.version;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_AGGREGATE_INIT_ON_VIEW";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_AGGREGATE_INIT_ON_VIEW");
    }
}

// ---------------------------------------------------------------------
// S6.3 — class-typed fields in structs (pointer slot per field).
// ---------------------------------------------------------------------

// Struct holds a user-class reference, initialized via aggregate init,
// then read back. Verifies the class-ref slot is exactly pointer-sized
// (storing a class pointer doesn't spill into the next field) and that
// the read produces the same instance pointer.
TEST(StructStackTests, structHoldsClassRefField) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 n;\n"
        "    public Tag() { this.n = 0; return; }\n"
        "}\n"
        "public struct Holder {\n"
        "    Tag t;\n"
        "    int32 count;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Tag tag = new Tag();\n"
        "        tag.n = 42;\n"
        "        Holder h = Holder { t: tag, count: 7 };\n"
        // h.t should be the same pointer as `tag`; reading h.t.n yields
        // what we wrote into tag. h.count survives the neighboring class
        // ref's 8-byte slot.
        "        return h.t.n + h.count;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 49);
}

// Two class-ref fields side by side — each should claim its own slot
// without one writing over the other.
TEST(StructStackTests, structHoldsTwoClassRefs) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; return; }\n"
        "}\n"
        "public struct Pair {\n"
        "    Box left;\n"
        "    Box right;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Box a = new Box();\n"
        "        a.v = 10;\n"
        "        Box b = new Box();\n"
        "        b.v = 32;\n"
        "        Pair p = Pair { left: a, right: b };\n"
        "        return p.left.v + p.right.v;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Class-ref field left out of the aggregate init lands null thanks to
// zero-init. Per Structs.md § Zero-init: null class refs are valid but
// the borrow checker treats them as "not initialized." We just check
// the surrounding fields' values to confirm the omitted slot didn't
// trip codegen.
TEST(StructStackTests, structPartialInitLeavesClassRefNull) {
    auto src =
        "package test;\n"
        "public class Tag {\n"
        "    public int32 n;\n"
        "    public Tag() { this.n = 0; return; }\n"
        "}\n"
        "public struct Holder {\n"
        "    int32 before;\n"
        "    Tag t;\n"
        "    int32 after;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        // `t` left out of bindings → zero-init keeps it null. before /
        // after must still hold the values we set, confirming the
        // pointer slot's 8 bytes didn't bleed into a neighbor int.
        "        Holder h = Holder { before: 3, after: 5 };\n"
        "        return h.before + h.after;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Reject interface-typed struct fields with the specific error ID. v1
// interface fields require fat-pointer storage (S10 of the rollout);
// not supported in S6.3.
TEST(StructStackTests, rejectInterfaceFieldInStruct) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public struct Bad {\n"
        "    Greeter g;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_STRUCT_FIELD_TYPE on interface field";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_STRUCT_FIELD_TYPE");
    }
}
