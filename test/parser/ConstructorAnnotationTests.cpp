// Tests for the constructor Lombok-mirror synthesizers
// (cajeta-docs/stdlib/Annotations.md § Constructors):
//   - @NoArgsConstructor
//   - @AllArgsConstructor
//   - @RequiredArgsConstructor (currently selects `final` fields only;
//     @NonNull selection lands when @NonNull does)
//
// Each synthesizer skips silently when a same-arity ctor already
// exists on the class.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// @NoArgsConstructor on a class with no user-declared ctor → that
// ctor exists and zero-inits every field.
TEST(ConstructorAnnotationTests, noArgsConstructorZeroInits) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        return p.a + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// @AllArgsConstructor takes every field in declaration order.
TEST(ConstructorAnnotationTests, allArgsConstructor) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point(3, 4);\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// @AllArgsConstructor with mixed primitive widths.
TEST(ConstructorAnnotationTests, allArgsConstructorMixedWidths) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class M {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        M m = heap M(1, 2, 3, 4);\n"
        "        return ((int64) m.a) + ((int64) m.b)\n"
        "             + ((int64) m.c) + m.d;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
}

// @RequiredArgsConstructor picks only `final` fields.
TEST(ConstructorAnnotationTests, requiredArgsCtorPicksFinalFields) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class C {\n"
        "    public final int32 fixed;\n"
        "    public int32 mutable_;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        C c = heap C(42);\n"  // single-arg matches the only `final` field
        "        return c.fixed + c.mutable_;\n"  // mutable_ stays 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// @RequiredArgsConstructor with no qualifying fields → zero-arg ctor.
TEST(ConstructorAnnotationTests, requiredArgsCtorNoFinalFields) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class E {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        E e = heap E();\n"
        "        return e.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// User-declared ctor with matching arity wins — synthesizer skips.
TEST(ConstructorAnnotationTests, userCtorWinsOverSynthesized) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class P {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public P(int32 a, int32 b) { this.x = a + 100; this.y = b + 100; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(3, 4);\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 207);  // (103 + 104)
}

// @NoArgsConstructor + @AllArgsConstructor on the same class produces
// BOTH ctors.
TEST(ConstructorAnnotationTests, noArgsAndAllArgsCoexist) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor @AllArgsConstructor public class P {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P a = heap P();\n"
        "        P b = heap P(7, 8);\n"
        "        return (a.x + a.y) * 100 + (b.x + b.y);\n"  // 0*100 + 15 = 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 15);
}

// Sanity: no annotation → no synthesized ctor (default behavior unchanged).
TEST(ConstructorAnnotationTests, noAnnotationKeepsDefaultBehavior) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(7);\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}
