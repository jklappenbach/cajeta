// Tests for the @With Lombok-mirror synthesizer
// (cajeta-docs/stdlib/Annotations.md § Immutability friend).
//
// Naming: `withCamelCase` rather than the Cajeta size()-style (which
// would collide with @Setter). For field `x`, the method is `withX(T v)`
// and returns a new heap instance with that field replaced.
//
// Body: __cajeta_alloc + memcpy from `this` (preserves vtable + every
// field), then overwrite the named slot. Original instance is unchanged.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Single field: withX returns new instance with x replaced.
TEST(WithAnnotationTests, singleFieldWithReplacesValue) {
    auto src =
        "package test;\n"
        "@With public class P {\n"
        "    public int32 x;\n"
        "    public P(int32 v) { this.x = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(7);\n"
        "        P p2 = p.withX(99);\n"
        "        return p2.x;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// Original instance is unchanged by withX.
TEST(WithAnnotationTests, originalUnchanged) {
    auto src =
        "package test;\n"
        "@With public class P {\n"
        "    public int32 x;\n"
        "    public P(int32 v) { this.x = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(7);\n"
        "        P p2 = p.withX(99);\n"
        "        return p.x;\n"  // original
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Multiple fields: other fields preserved when one is replaced.
TEST(WithAnnotationTests, otherFieldsPreserved) {
    auto src =
        "package test;\n"
        "@With public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point(int32 a, int32 b) { this.x = a; this.y = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point(3, 4);\n"
        "        Point p2 = p.withX(10);\n"
        "        return p2.x * 100 + p2.y;\n"  // x replaced, y preserved
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1004);
}

// Multiple withs chained.
TEST(WithAnnotationTests, chainedWithCalls) {
    auto src =
        "package test;\n"
        "@With public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point(int32 a, int32 b) { this.x = a; this.y = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point(0, 0);\n"
        "        Point p2 = p.withX(5).withY(7);\n"
        "        return p2.x * 100 + p2.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 507);
}

// Field-level @With: only annotated field gets a `with` method.
TEST(WithAnnotationTests, fieldLevelWith) {
    auto src =
        "package test;\n"
        "public class Person {\n"
        "    @With public int32 age;\n"
        "    public int32 secret;\n"
        "    public Person(int32 a, int32 s) { this.age = a; this.secret = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(30, 42);\n"
        "        Person p2 = p.withAge(40);\n"
        "        return p2.age * 100 + p2.secret;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4042);
}

// User-declared withX wins.
TEST(WithAnnotationTests, userWithMethodWins) {
    auto src =
        "package test;\n"
        "@With public class P {\n"
        "    public int32 x;\n"
        "    public P(int32 v) { this.x = v; }\n"
        "    public #P withX(int32 v) { return heap P(v + 1000); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(5);\n"
        "        return p.withX(99).x;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1099);
}
