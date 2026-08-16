// Tests for the @Builder Lombok-mirror synthesizer
// (docs/specification/reflect/Annotations.md § Builders).
//
// Synthesizes a nested `Outer.Builder` class (uses the nested-class
// infrastructure landed alongside) + a `static Outer.Builder
// builder()` factory on Outer. Builder mirrors Outer's fields as
// private slots; per-field chained setters return `this`; `build()`
// allocates Outer and calls its all-args ctor with the accumulated
// field values.
//
// @Builder implicitly enables @AllArgsConstructor on Outer (build()
// calls that ctor).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Basic flow: builder() factory + chained setters + build().

// Field setters can be called in any order.

// Not all fields need be set — the others stay at their zero-init
// value from the Builder's no-arg ctor.

// build() can be called multiple times on the same Builder, each
// producing a distinct Outer instance.

// Mixed primitive widths exercise the storage-shape load logic.

// Empty class (no fields) — builder().build() still works.

// `@Builder(builderMethodName="newBuilder")` renames the static factory
// on Outer. The Builder class shape is unchanged.

// `@Builder(buildMethodName="create")` renames build() on Builder.

// `@Builder(setterPrefix="with")` prepends the prefix and capitalizes the
// field name on each chained setter (Lombok parity: setterPrefix="with"
// turns `name(v)` into `withName(v)`).

// All three customizations composed.
TEST(BuilderAnnotationTests, allNamingCustomizationsComposed) {
    auto src =
        "package test;\n"
        "@Builder("
        "    builderMethodName=\"of\","
        "    buildMethodName=\"make\","
        "    setterPrefix=\"set\""
        ") public class Cfg {\n"
        "    public int32 v;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Cfg c = Cfg.of().setV(99).make();\n"
        "        return c.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// `@Builder.Default int32 x = 5;` — when the caller doesn't call x(),
// build() uses the declared default instead of the zero-init value.
TEST(BuilderAnnotationTests, defaultUsedWhenSetterNotCalled) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    @Builder.Default public int32 x = 5;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Only y is set; x should keep its declared default of 5.
        "        P p = P.builder().y(2).build();\n"
        "        return p.x * 10 + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 52);
}

// Calling the setter overrides the default.
TEST(BuilderAnnotationTests, defaultOverriddenWhenSetterCalled) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    @Builder.Default public int32 x = 99;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().x(7).build();\n"
        "        return p.x;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Multiple @Builder.Default fields each carry their own default.
TEST(BuilderAnnotationTests, multipleDefaults) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    @Builder.Default public int32 a = 10;\n"
        "    @Builder.Default public int32 b = 20;\n"
        "    public int32 c;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().c(3).build();\n"
        "        return p.a + p.b + p.c;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 33);
}

// Float default.
TEST(BuilderAnnotationTests, floatDefault) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    @Builder.Default public float64 ratio = 0.5;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().build();\n"
        // Multiply by 100 to get an integer-encoded result.
        "        return (int32) (p.ratio * 100.0);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 50);
}

// Boolean default.
TEST(BuilderAnnotationTests, booleanDefault) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    @Builder.Default public boolean active = true;\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().n(0).build();\n"
        "        if (p.active) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
