// Tests for the @Getter / @Setter Lombok-mirror annotations
// (docs/specification/reflect/Annotations.md § Accessors). Each annotation
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

// Field-level @Getter: only the annotated field gets a getter.

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

// Multiple primitives of different widths exercise the slot-type path.

// Class-level @Setter: synthesizes a setter for every non-static
// non-final field.

// Field-level @Setter: only annotated field gets the setter.

// @Setter on a final field is silently skipped — no compile error,
// but no setter is emitted. Calling `p.x(42)` would then fail because
// the method doesn't exist.

// @Getter + @Setter together on the same class.

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

// `@Getter(access="protected")` is callable from within the declaring
// class (own-class access — protected always permits self).
TEST(GetterSetterTests, accessProtectedCallableFromOwnClass) {
    auto src =
        "package test;\n"
        "@Getter(access=\"protected\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "    public int32 readSelf() { return this.v(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(3);\n"
        "        return b.readSelf();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// `@Getter(access="private")` is NOT callable cross-class — calling
// `b.v()` from outside the declaring class is a compile-time error.
TEST(GetterSetterTests, accessPrivateRejectedCrossClass) {
    auto src =
        "package test;\n"
        "@Getter(access=\"private\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(3);\n"
        "        return b.v();\n"  // private accessor — not visible here.
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_METHOD_NOT_ACCESSIBLE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_METHOD_NOT_ACCESSIBLE");
    }
}

// `@Getter(access="protected")` is callable from same-package classes
// even when not a subclass — Java semantics: protected = same-package
// OR descendant.
TEST(GetterSetterTests, accessProtectedSamePackageAllowed) {
    auto src =
        "package test;\n"
        "@Getter(access=\"protected\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(3);\n"
        "        return b.v();\n"  // same package: OK
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// `@Getter(access="protected")` IS callable from a subclass (Java
// semantics: protected is visible to descendants).
TEST(GetterSetterTests, accessProtectedAllowedFromSubclass) {
    auto src =
        "package test;\n"
        "@Getter(access=\"protected\") public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public class Heir extends Box {\n"
        "    public Heir(int32 x) { this.v = x; }\n"
        "    public int32 readInherited() { return this.v(); }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Heir h = heap Heir(5);\n"
        "        return h.readInherited();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
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
