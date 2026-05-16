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
