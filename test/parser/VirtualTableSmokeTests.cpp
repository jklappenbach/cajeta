//
// Smoke tests for the vtable build pipeline.
//
// `CajetaClass::writeVirtualTable` now runs at the end of `generatePrototype`.
// It calls `buildVirtualTable` (walks the hierarchy, populates the slot list)
// then `StructureMetadata::populate` (materializes the LLVM vtable type +
// global). Tests here verify the pipeline doesn't blow up on a few common
// class shapes; meaningful dynamic-dispatch coverage will follow when
// inheritance + virtual calls are wired into MethodCallExpression.
//
// Today's coverage is structural:
//   - Only-static-methods class — vtable's slot list is empty after filters.
//   - Class with a non-static method — slot list contains the method;
//     vtable type/global build without LLVM rejection.
//   - Two classes in the same file — each gets its own vtable global without
//     name collisions.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.V");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- Only-static class ------------------------------------------------------

TEST(VirtualTableSmokeTests, onlyStaticMethodsCompile) {
    // buildVirtualTable filters static methods and constructors. The slot
    // list ends up empty; the vtable is `{ i16 version, i16 count = 0 }`.
    // We just need codegen to not reject the empty-slot struct.
    auto src =
        "package test;\n"
        "public final class V {\n"
        "    public static int32 run() { return 42; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// --- Class with a non-static method ----------------------------------------

TEST(VirtualTableSmokeTests, classWithNonStaticMethodCompiles) {
    // Foo has one non-static method `plain`. buildVirtualTable picks it up as
    // a single slot; createVirtualTableType emits `{ i16, i16, ptr }`;
    // createVirtualTableConstant initializes the slot with `&Foo::plain`.
    // Test passes if the whole module verifies and the unrelated static
    // `run()` still resolves.
    auto src =
        "package test;\n"
        "public class Foo {\n"
        "    public int32 plain() { return 7; }\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(VirtualTableSmokeTests, classWithMultipleNonStaticMethodsCompiles) {
    // Two slots; verifies the type/constant agree on arity and order.
    auto src =
        "package test;\n"
        "public class Bag {\n"
        "    public int32 first() { return 1; }\n"
        "    public int32 second() { return 2; }\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// --- Multiple classes coexist ---------------------------------------------

TEST(VirtualTableSmokeTests, twoClassesGetTheirOwnVtables) {
    // Each class produces its own `<canonical>#VTable` global. If `populate`
    // accidentally shared state between classes, the second build would
    // collide with the first.
    auto src =
        "package test;\n"
        "public class A {\n"
        "    public int32 foo() { return 1; }\n"
        "}\n"
        "public class B {\n"
        "    public int32 bar() { return 2; }\n"
        "    public int32 baz() { return 3; }\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 99; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// --- Mixed static + non-static --------------------------------------------

TEST(VirtualTableSmokeTests, mixedStaticAndNonStaticFiltersStatics) {
    // The filter inside buildVirtualTable should drop statics; only the
    // non-static method ends up in the slot list. We can't directly inspect
    // the global from this test framework today, but the compile must
    // succeed without a type/constant arity mismatch.
    auto src =
        "package test;\n"
        "public class Mixed {\n"
        "    public static int32 staticHelper() { return 1; }\n"
        "    public int32 instance() { return 2; }\n"
        "    public static int32 anotherStatic() { return 3; }\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 5; }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}
