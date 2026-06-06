// Tests for the @AutoHash class annotation. When present, the
// compiler injects a structural hash() that walks the class's fields
// and combines them via Hash.combine + Hash.processSeed. Without the
// annotation, the class keeps the inherited identity hash from
// cajeta.lang.Object.
//
// v1 covers primitive fields (boolean / int8..int64 / uint8..uint64 /
// float32 / float64) and class-typed fields via vtable virtual
// dispatch. Struct, array, String, pointer, and extended-precision
// float fields cause a compile-time diagnostic naming the class,
// field, type, reason, and remediation.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include <cstdint>
using cajeta_test::CajetaJit;

// Two distinct instances of a @AutoHash'd class with identical
// primitive fields must hash identically — the HashMap-key contract.
// Without @AutoHash, identity hash would make them differ.
TEST(AutoHashTests, equalFieldsHashEqually) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point a = heap Point();\n"
        "        a.x = 42;\n"
        "        a.y = 99;\n"
        "        Point b = heap Point();\n"
        "        b.x = 42;\n"
        "        b.y = 99;\n"
        "        int64 ha = a.hash();\n"
        "        int64 hb = b.hash();\n"
        "        return ha == hb ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Different field values produce different hashes.
TEST(AutoHashTests, differentFieldsHashDifferently) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point a = heap Point();\n"
        "        a.x = 1;\n"
        "        a.y = 2;\n"
        "        Point b = heap Point();\n"
        "        b.x = 3;\n"
        "        b.y = 4;\n"
        "        return a.hash() != b.hash() ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Without @AutoHash, the class inherits Object.hash() (identity).
// Two distinct instances with same field values hash differently.
TEST(AutoHashTests, noAnnotationKeepsIdentityHash) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 x;\n"
        "    public Plain() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain a = heap Plain();\n"
        "        a.x = 7;\n"
        "        Plain b = heap Plain();\n"
        "        b.x = 7;\n"
        "        return a.hash() != b.hash() ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Manual hash() on a @AutoHash'd class wins — the synthesizer skips
// when the user has declared their own. Probe returns the manual
// hash value (12345) regardless of fields.
TEST(AutoHashTests, manualHashOverridesSynthesis) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class Custom {\n"
        "    public int32 x;\n"
        "    public Custom() { return; }\n"
        "    public int64 hash() { return 12345; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Custom c = heap Custom();\n"
        "        c.x = 7;\n"
        "        return c.hash();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 12345);
}

// Mixing field widths: boolean + int8 + int16 + int32 + int64 +
// float32 + float64. Verifies that each runtime helper gets called
// with the right coercion (zext / sext for narrow ints) and that
// the same field set hashes identically across two instances.
TEST(AutoHashTests, mixedPrimitiveWidthsHashConsistently) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class Wide {\n"
        "    public boolean b;\n"
        "    public int8 i8;\n"
        "    public int16 i16;\n"
        "    public int32 i32;\n"
        "    public int64 i64;\n"
        "    public float32 f32;\n"
        "    public float64 f64;\n"
        "    public Wide() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Wide a = heap Wide();\n"
        "        a.b = true;\n"
        "        a.i8 = 7;\n"
        "        a.i16 = 1000;\n"
        "        a.i32 = 100000;\n"
        "        a.i64 = 5000000000;\n"
        "        a.f32 = 3.14;\n"
        "        a.f64 = 2.71828;\n"
        "        Wide b = heap Wide();\n"
        "        b.b = true;\n"
        "        b.i8 = 7;\n"
        "        b.i16 = 1000;\n"
        "        b.i32 = 100000;\n"
        "        b.i64 = 5000000000;\n"
        "        b.f32 = 3.14;\n"
        "        b.f64 = 2.71828;\n"
        "        return a.hash() == b.hash() ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// @AutoHash on a class with an array field is rejected with a
// diagnostic. The user should see "field `xs` (type `int32[]`)
// cannot be auto-hashed — array-field hashing is not yet
// implemented in @AutoHash v1 ..."
TEST(AutoHashTests, arrayFieldRejectedWithAttribution) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class HasArray {\n"
        "    public int32[] xs;\n"
        "    public HasArray() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// String field on an @AutoHash class is accepted post Phase 2b-β:
// `cajeta.lang.String` is a CLASS with its own `hash()` method
// (XXH3-style content hash; runtime/src/cajeta/lang/String.cajeta
// § 107), so the synthesizer's CLASS_INLINE path delegates to
// String.hash() rather than rejecting the field. The original
// version of this test (which expected rejection) was written when
// String was a `char*` typedef without a hash method — that
// rationale no longer applies.
TEST(AutoHashTests, stringFieldDelegatesToStringHash) {
    auto src =
        "package test;\n"
        "@AutoHash\n"
        "public class HasString {\n"
        "    public String name;\n"
        "    public HasString() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.D"));
}
