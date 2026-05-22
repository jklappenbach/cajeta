// Tests for the @Getter / @Setter Lombok-mirror annotations
// (cajeta-docs/stdlib/Annotations.md § Accessors). Each annotation
// can sit on the class (applies to every non-static field) or on
// an individual field (only that field gets the accessor). Naming
// follows size()-style: getter for `name` is `name()`, setter is
// `name(value)`.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Class-level @Getter: synthesizes a getter for every non-static field.
TEST(GetterSetterTests, classLevelGetterEmitsPerField) {
    auto src =
        "package test;\n"
        "@Getter public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point(int32 a, int32 b) { this.x = a; this.y = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point(3, 4);\n"
        "        return p.x() + p.y();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Field-level @Getter: only the annotated field gets a getter.
TEST(GetterSetterTests, fieldLevelGetterOnlySingleField) {
    auto src =
        "package test;\n"
        "public class Person {\n"
        "    @Getter public int32 age;\n"
        "    public int32 secret;\n"
        "    public Person(int32 a, int32 s) { this.age = a; this.secret = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(30, 99);\n"
        "        return p.age();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 30);
}

// User-declared same-name no-arg method wins — synthesizer skips.
TEST(GetterSetterTests, userMethodWinsOverSynthesized) {
    auto src =
        "package test;\n"
        "@Getter public class Box {\n"
        "    public int32 value;\n"
        "    public Box(int32 v) { this.value = v; }\n"
        "    public int32 value() { return this.value * 100; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(5);\n"
        "        return b.value();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 500);
}

// Static fields are skipped — no getter synthesized for them.
TEST(GetterSetterTests, staticFieldsSkipped) {
    auto src =
        "package test;\n"
        "@Getter public class Counter {\n"
        "    public static int32 total = 100;\n"
        "    public int32 n;\n"
        "    public Counter(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter(7);\n"
        "        return c.n();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Multiple primitives of different widths exercise the slot-type path.
TEST(GetterSetterTests, getterAcrossMultiplePrimitiveWidths) {
    auto src =
        "package test;\n"
        "@Getter public class Mix {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "    public Mix(int8 a, int16 b, int32 c, int64 d) {\n"
        "        this.a = a; this.b = b; this.c = c; this.d = d;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Mix m = heap Mix(1, 2, 3, 4);\n"
        "        return ((int64) m.a()) + ((int64) m.b())\n"
        "             + ((int64) m.c()) + m.d();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
}

// Class-level @Setter: synthesizes a setter for every non-static
// non-final field.
TEST(GetterSetterTests, classLevelSetterEmitsPerField) {
    auto src =
        "package test;\n"
        "@Setter public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point() { this.x = 0; this.y = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point();\n"
        "        p.x(10);\n"
        "        p.y(32);\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Field-level @Setter: only annotated field gets the setter.
TEST(GetterSetterTests, fieldLevelSetterOnlyOneField) {
    auto src =
        "package test;\n"
        "public class Person {\n"
        "    @Setter public int32 age;\n"
        "    public int32 secret;\n"
        "    public Person() { this.age = 0; this.secret = 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person();\n"
        "        p.age(42);\n"
        "        return p.age;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// @Setter on a final field is silently skipped — no compile error,
// but no setter is emitted. Calling `p.x(42)` would then fail because
// the method doesn't exist.
TEST(GetterSetterTests, setterSkipsFinalFields) {
    auto src =
        "package test;\n"
        "@Setter public class Frozen {\n"
        "    public final int32 fixed;\n"
        "    public int32 mutable_;\n"
        "    public Frozen(int32 f) { this.fixed = f; this.mutable_ = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Frozen f = heap Frozen(7);\n"
        "        f.mutable_(35);\n"
        "        return f.fixed + f.mutable_;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// @Getter + @Setter together on the same class.
TEST(GetterSetterTests, getterAndSetterTogether) {
    auto src =
        "package test;\n"
        "@Getter @Setter public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        c.n(40);\n"
        "        return c.n() + 2;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// `@Getter(access="public")` — explicit public, identical behavior to default.
TEST(GetterSetterTests, accessPublicExplicitWorksLikeDefault) {
    auto src =
        "package test;\n"
        "@Getter(access=\"public\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(11);\n"
        "        return b.v();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 11);
}

// `@Getter(access="private")` callable from within the declaring class.
TEST(GetterSetterTests, accessPrivateCallableFromOwnClass) {
    auto src =
        "package test;\n"
        "@Getter(access=\"private\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "    public int32 doubled() { return this.v() * 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(7);\n"
        "        return b.doubled();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 14);
}

// `@Getter(access="protected")` records the modifier and compiles.
// v1 doesn't enforce protected vs. public at call sites (the doc-level
// promise is the same as for private — modifier lands on the synthesized
// method, future enforcement work consumes it).
TEST(GetterSetterTests, accessProtectedCompiles) {
    auto src =
        "package test;\n"
        "@Getter(access=\"protected\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(3);\n"
        "        return b.v();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// Unknown `access` value is rejected with a clear error.
TEST(GetterSetterTests, accessUnknownRejected) {
    auto src =
        "package test;\n"
        "@Getter(access=\"bogus\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ACCESSOR_BAD_ACCESS";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ACCESSOR_BAD_ACCESS");
    }
}

// `@Setter(access="private")` same story as @Getter.
TEST(GetterSetterTests, setterAccessPrivateCallableFromOwnClass) {
    auto src =
        "package test;\n"
        "@Setter(access=\"private\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 0; }\n"
        "    public int32 setAndRead(int32 x) {\n"
        "        this.v(x);\n"
        "        return this.v;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        return b.setAndRead(9);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9);
}

// Field-level `@Getter(access="private")` overrides the class-level default
// (or, here, applies on a class that has no class-level @Getter).
TEST(GetterSetterTests, fieldLevelAccessPrivate) {
    auto src =
        "package test;\n"
        "public class Person {\n"
        "    @Getter(access=\"private\") public int32 age;\n"
        "    public Person(int32 a) { this.age = a; }\n"
        "    public int32 readAge() { return this.age(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Person p = heap Person(33);\n"
        "        return p.readAge();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 33);
}

// Sanity: class without @Getter doesn't accidentally synthesize.
TEST(GetterSetterTests, noAnnotationNoSynthesis) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 x;\n"
        "    public Plain(int32 v) { this.x = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain p = heap Plain(42);\n"
        "        return p.x;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
