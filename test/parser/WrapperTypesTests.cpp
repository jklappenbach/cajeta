// Wrapper-type family (cajeta.lang boxed primitives), Phase W1:
// Number + Boolean/Int32/Int64/Float32/Float64. Each test compiles a small
// program whose test.M.run() returns an int32 the C++ side asserts on (1 = the
// in-cajeta checks all held). See plans/lang/wrapper-types-plan.md.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// An M.run() entry that the body fills in, with the W1 wrappers imported.
std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.lang.Number;\n"
        "import cajeta.lang.Boolean;\n"
        "import cajeta.lang.Int8;\n"
        "import cajeta.lang.Int16;\n"
        "import cajeta.lang.Int32;\n"
        "import cajeta.lang.Int64;\n"
        "import cajeta.lang.UInt8;\n"
        "import cajeta.lang.UInt16;\n"
        "import cajeta.lang.UInt32;\n"
        "import cajeta.lang.UInt64;\n"
        "import cajeta.lang.Char;\n"
        "import cajeta.lang.Float16;\n"
        "import cajeta.lang.BFloat16;\n"
        "import cajeta.lang.Float32;\n"
        "import cajeta.lang.Float64;\n"
        "import cajeta.lang.Float128;\n"
        "import cajeta.lang.Float8E4M3;\n"
        "import cajeta.lang.Float8E5M2;\n"
        "import cajeta.lang.Float8E4M3Fnuz;\n"
        "import cajeta.lang.Float8E5M2Fnuz;\n"
        "import cajeta.lang.Float6E2M3;\n"
        "import cajeta.lang.Float6E3M2;\n"
        "import cajeta.lang.Float4E2M1;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.reflect.Class;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(prog(body), "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// of()/value() round-trip across Int32, Int64, Boolean.
TEST(WrapperTypesTests, roundTrip) {
    EXPECT_EQ(runI32(
        "Int32 a = Int32.of(42);\n"
        "Int64 b = Int64.of(9000000000L);\n"     // > 2^32, proves 64-bit storage
        "Boolean c = Boolean.of(true);\n"
        "if (a.value() != 42) { return 0; }\n"
        "if (b.value() != 9000000000L) { return 0; }\n"
        "if (c.value()) { return 1; }\n"          // c was boxed true
        "return 0;\n"), 1);
}

// Value equality flows through the hash() override + Object.operator==.
TEST(WrapperTypesTests, valueEquality) {
    EXPECT_EQ(runI32(
        "if (Int32.of(5) == Int32.of(5)) {\n"        // equal values -> equal
        "    if (Int32.of(5) == Int32.of(6)) { return 0; }\n"  // distinct -> not
        "    if (Boolean.of(true) == Boolean.of(false)) { return 0; }\n"
        "    if (Boolean.of(true) == Boolean.of(true)) { return 1; }\n"
        "}\n"
        "return 0;\n"), 1);
}

// Number upcast + virtual dispatch of asInt64()/asFloat64() across wrappers.
TEST(WrapperTypesTests, numberPolymorphism) {
    EXPECT_EQ(runI32(
        "Number n = Int32.of(7);\n"
        "if (n.asInt64() != 7L) { return 0; }\n"
        "if (n.asFloat64() != 7.0) { return 0; }\n"
        "Number m = Int64.of(123L);\n"
        "if (m.asInt64() != 123L) { return 0; }\n"
        "Number p = Float64.of(2.5);\n"
        "if (p.asFloat64() != 2.5) { return 0; }\n"
        "if (p.asInt64() != 2L) { return 0; }\n"         // truncating conversion
        "Number q = Float32.of(1.5f);\n"
        "if (q.asFloat64() != 1.5) { return 0; }\n"
        "return 1;\n"), 1);
}

// Float equality is bitwise (not numeric truncation): 1.5 != 1.2, equal selves.
TEST(WrapperTypesTests, floatBitwiseEquality) {
    EXPECT_EQ(runI32(
        "if (Float64.of(1.5) == Float64.of(1.2)) { return 0; }\n"  // distinct
        "if (Float32.of(1.5f) == Float32.of(1.2f)) { return 0; }\n"
        "if (Float64.of(1.5) == Float64.of(1.5)) {\n"
        "    if (Float32.of(3.25f) == Float32.of(3.25f)) { return 1; }\n"
        "}\n"
        "return 0;\n"), 1);
}

// A boxed value is reflection-introspectable: its `value` is field index 0,
// so Class.of(box).getInt32(box, 0) reads it (and the box has exactly 1 field).
TEST(WrapperTypesTests, reflectionIntrospectable) {
    EXPECT_EQ(runI32(
        "Int32 box = Int32.of(99);\n"
        "Class c = Class.of(box);\n"
        "if (c.getFieldCount() != 1) { return -1; }\n"
        "return c.getInt32(box, 0);\n"), 99);
}

// W2 wrappers: of/value round-trip across the narrow/unsigned ints + Char, plus
// virtual asInt64() through a Number upcast. value() is compared via an int32/64
// cast (the wrapper's primitive type differs in width from a bare literal).
TEST(WrapperTypesTests, w2RoundTrip) {
    EXPECT_EQ(runI32(
        "Int8 a = Int8.of((int8) -5);\n"
        "if ((int32) a.value() != -5) { return 1; }\n"
        "Int16 b = Int16.of((int16) -300);\n"
        "if ((int32) b.value() != -300) { return 2; }\n"
        "UInt8 c = UInt8.of((uint8) 200);\n"
        "if ((int32) c.value() != 200) { return 3; }\n"
        "UInt16 d = UInt16.of((uint16) 60000);\n"
        "if ((int32) d.value() != 60000) { return 4; }\n"
        "UInt32 e = UInt32.of((uint32) 200000);\n"
        "if (e.asInt64() != 200000L) { return 5; }\n"          // via Number override
        "UInt64 f = UInt64.of((uint64) 9000000000L);\n"
        "if ((int64) f.value() != 9000000000L) { return 6; }\n"
        "Char g = Char.of('A');\n"
        "if ((int32) g.value() != 65) { return 7; }\n"
        "Number n = Int8.of((int8) 7);\n"                      // upcast + virtual dispatch
        "if (n.asInt64() != 7L) { return 8; }\n"
        "return 0;\n"), 0);
}

// W3 wrappers: the wide/half IEEE floats. of/value round-trip + asFloat64
// (lossless for half/bfloat; rounded for binary128) via a Number upcast.
TEST(WrapperTypesTests, w3RoundTrip) {
    EXPECT_EQ(runI32(
        "Float16 a = Float16.of((float16) 1.5f);\n"
        "if (a.asFloat64() != 1.5) { return 1; }\n"            // half 1.5 is exact
        "BFloat16 b = BFloat16.of((bfloat16) 2.5f);\n"
        "if (b.asFloat64() != 2.5) { return 2; }\n"            // bfloat 2.5 is exact
        "Float128 c = Float128.of((float128) 3.5);\n"
        "if (c.asFloat64() != 3.5) { return 3; }\n"
        "if (c.asInt64() != 3L) { return 4; }\n"               // truncating
        "Number n = Float16.of((float16) 4.0f);\n"             // upcast + virtual dispatch
        "if (n.asFloat64() != 4.0) { return 5; }\n"
        "return 0;\n"), 0);
}

// W3 equality is value/bitwise: equal values compare equal, distinct distinct.
// Float128 hashes the full 128 bits (no lossy float64 collapse), so two values
// that round to the same double still compare unequal.
TEST(WrapperTypesTests, w3FloatEquality) {
    EXPECT_EQ(runI32(
        "if (Float16.of((float16) 1.5f) == Float16.of((float16) 1.25f)) { return 1; }\n"
        "if (Float128.of((float128) 1.5) == Float128.of((float128) 1.2)) { return 2; }\n"
        "if (Float16.of((float16) 1.5f) == Float16.of((float16) 1.5f)) {\n"
        "    if (Float128.of((float128) 7.5) == Float128.of((float128) 7.5)) {\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "return 3;\n"), 0);
}

// W4 wrappers: the opaque ML fp8/sub-byte floats. value()/asInt64() expose the
// RAW STORAGE BITS (no fp decode yet — plan W4); construct from a small bit
// pattern and read it back. Uses < 2^(bits-1) so the sign-extending widen is a
// no-op and the comparison is exact.
TEST(WrapperTypesTests, w4RawBitsRoundTrip) {
    EXPECT_EQ(runI32(
        "Float8E4M3 a = Float8E4M3.of((float8e4m3) 42);\n"
        "if ((int32) a.value() != 42) { return 1; }\n"
        "if (a.asInt64() != 42L) { return 2; }\n"
        "Float8E5M2 b = Float8E5M2.of((float8e5m2) 100);\n"
        "if ((int32) b.value() != 100) { return 3; }\n"
        "Float8E4M3Fnuz c = Float8E4M3Fnuz.of((float8e4m3fnuz) 7);\n"
        "if ((int32) c.value() != 7) { return 4; }\n"
        "Float8E5M2Fnuz d = Float8E5M2Fnuz.of((float8e5m2fnuz) 9);\n"
        "if ((int32) d.value() != 9) { return 5; }\n"
        "Float6E2M3 e = Float6E2M3.of((float6e2m3) 20);\n"
        "if ((int32) e.value() != 20) { return 6; }\n"
        "Float6E3M2 f = Float6E3M2.of((float6e3m2) 15);\n"
        "if ((int32) f.value() != 15) { return 7; }\n"
        "Float4E2M1 g = Float4E2M1.of((float4e2m1) 5);\n"
        "if ((int32) g.value() != 5) { return 8; }\n"
        "return 0;\n"), 0);
}

// W4 hash is over the raw bits: identical storage compares equal, distinct not.
TEST(WrapperTypesTests, w4RawBitsEquality) {
    EXPECT_EQ(runI32(
        "if (Float8E4M3.of((float8e4m3) 12) == Float8E4M3.of((float8e4m3) 13)) { return 1; }\n"
        "if (Float8E4M3.of((float8e4m3) 12) == Float8E4M3.of((float8e4m3) 12)) {\n"
        "    return 0;\n"
        "}\n"
        "return 2;\n"), 0);
}

// W6 toString: decimal text for signed/unsigned/float wrappers, true/false for
// Boolean. Unsigned uses the true magnitude (a uint64 with the high bit set
// must NOT print negative).
TEST(WrapperTypesTests, w6ToString) {
    EXPECT_EQ(runI32(
        "if (!Int32.of(-42).toString().equals(\"-42\")) { return 1; }\n"
        "if (!Int64.of(9000000000L).toString().equals(\"9000000000\")) { return 2; }\n"
        "if (!Int8.of((int8) -5).toString().equals(\"-5\")) { return 3; }\n"
        "if (!UInt8.of((uint8) 200).toString().equals(\"200\")) { return 4; }\n"
        "if (!UInt64.of((uint64) -1L).toString().equals(\"18446744073709551615\")) { return 5; }\n"
        "if (!Float64.of(1.5).toString().equals(\"1.5\")) { return 6; }\n"
        "if (!Boolean.of(true).toString().equals(\"true\")) { return 7; }\n"
        "if (!Boolean.of(false).toString().equals(\"false\")) { return 8; }\n"
        "return 0;\n"), 0);
}
