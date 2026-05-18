// Tests for the @Builder Lombok-mirror synthesizer
// (cajeta-docs/stdlib/Annotations.md § Builders).
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

#include <cstdint>

using cajeta_test::CajetaJit;

// Basic flow: builder() factory + chained setters + build().
TEST(BuilderAnnotationTests, basicFlow) {
    auto src =
        "package test;\n"
        "@Builder public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point.builder().x(3).y(7).build();\n"
        "        return p.x * 100 + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 307);
}

// Field setters can be called in any order.
TEST(BuilderAnnotationTests, settersInArbitraryOrder) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "    public int32 c;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().c(3).a(1).b(2).build();\n"
        "        return p.a * 100 + p.b * 10 + p.c;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 123);
}

// Not all fields need be set — the others stay at their zero-init
// value from the Builder's no-arg ctor.
TEST(BuilderAnnotationTests, partialFillUsesZeros) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.builder().a(7).build();\n"
        "        return p.a * 100 + p.b;\n"  // b stays 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 700);
}

// build() can be called multiple times on the same Builder, each
// producing a distinct Outer instance.
TEST(BuilderAnnotationTests, builderReusable) {
    auto src =
        "package test;\n"
        "@Builder public class P {\n"
        "    public int32 v;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point.Builder b = P.builder().v(10);\n"  // (intentional rename — should be P.Builder; gtest below uses it)
        "        P first = b.build();\n"
        "        P second = b.build();\n"
        "        return first.v + second.v;\n"
        "    }\n"
        "}\n";
    // The above intentionally uses `Point.Builder` as a typo — but
    // since the class is `P`, this won't compile. Replace with
    // P.Builder before running. We do it inline via std::string ops.
    std::string s = src;
    size_t pos = s.find("Point.Builder");
    while (pos != std::string::npos) {
        s.replace(pos, 13, "P.Builder");
        pos = s.find("Point.Builder", pos + 9);
    }
    auto jit = CajetaJit::compile(s, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 20);
}

// Mixed primitive widths exercise the storage-shape load logic.
TEST(BuilderAnnotationTests, mixedPrimitiveWidths) {
    auto src =
        "package test;\n"
        "@Builder public class M {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        M m = M.builder().a(1).b(2).c(3).d(4).build();\n"
        "        return ((int64) m.a) + ((int64) m.b)\n"
        "             + ((int64) m.c) + m.d;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
}

// Empty class (no fields) — builder().build() still works.
TEST(BuilderAnnotationTests, emptyClass) {
    auto src =
        "package test;\n"
        "@Builder public class E {\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        E e = E.builder().build();\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
