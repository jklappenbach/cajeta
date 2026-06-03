//
// Numerics for Bounded Templates: `T extends Numeric / Integral / Floating`
// admits primitive numeric type arguments at template instantiation. The
// three bound names are recognized at TemplateInstantiator's constraint
// pass (Templates.h / TemplateInstantiator.cpp) and dispatch to a
// flag-based check (NUMBER_FLAG / INT_FLAG / FLOAT_FLAG) instead of the
// standard class-hierarchy walk.
//
// Hierarchy:
//   - Numeric  — any primitive numeric (int8..int64, uint8..uint64,
//                float32, float64). Boolean is explicitly excluded
//                (no arithmetic semantics).
//   - Integral — Numeric ∩ INT_FLAG (signed/unsigned ints; not boolean).
//   - Floating — Numeric ∩ FLOAT_FLAG (float32, float64).
//
// Monomorphization: each instantiation is a distinct CajetaClass with the
// substituted primitive baked into field layouts and method bodies. The
// template parameter T resolves to a concrete primitive type, so
// expressions like `this.x + other.x` lower to the corresponding primitive
// op (i32 add, double fadd, etc.) without any per-T dispatch overhead.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace

// Numeric bound: accept int32, do primitive addition through T.
TEST(NumericBoundedTemplateTests, numericBoundAcceptsInt32) {
    auto src =
        "package test;\n"
        "public class Adder<T extends Numeric> {\n"
        "    public T x;\n"
        "    public T y;\n"
        "    public Adder(T x, T y) { this.x = x; this.y = y; }\n"
        "    public T sum() { return this.x + this.y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Adder<int32> a = heap Adder<int32>(3, 4);\n"
        "        return a.sum();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Numeric bound: accept float64, do fp addition through T.
TEST(NumericBoundedTemplateTests, numericBoundAcceptsFloat64) {
    auto src =
        "package test;\n"
        "public class Adder<T extends Numeric> {\n"
        "    public T x;\n"
        "    public T y;\n"
        "    public Adder(T x, T y) { this.x = x; this.y = y; }\n"
        "    public T sum() { return this.x + this.y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        Adder<float64> a = heap Adder<float64>(1.5, 2.25);\n"
        "        return a.sum();\n"
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(runF64(src), 3.75);
}

// Numeric bound: reject String (not a primitive numeric).
TEST(NumericBoundedTemplateTests, numericBoundRejectsString) {
    auto src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public class Adder<T extends Numeric> {\n"
        "    public T x;\n"
        "    public Adder(T x) { this.x = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Adder<String> a = heap Adder<String>(\"x\");\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Numeric bound: reject boolean (no arithmetic semantics — INT_FLAG is set
// for legacy zero/one storage but boolean is not a Numeric category).
TEST(NumericBoundedTemplateTests, numericBoundRejectsBoolean) {
    auto src =
        "package test;\n"
        "public class Holder<T extends Numeric> {\n"
        "    public T x;\n"
        "    public Holder(T x) { this.x = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder<boolean> h = heap Holder<boolean>(true);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Integral bound: accept int64, use a bitwise shift (only valid for ints).
TEST(NumericBoundedTemplateTests, integralBoundAllowsShift) {
    auto src =
        "package test;\n"
        "public class Bits<T extends Integral> {\n"
        "    public T v;\n"
        "    public Bits(T v) { this.v = v; }\n"
        "    public T doubled() { return this.v << 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        Bits<int64> b = heap Bits<int64>(21L);\n"
        "        return b.doubled();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI64(src), 42L);
}

// Integral bound: reject float32 (FLOAT_FLAG, not INT_FLAG).
TEST(NumericBoundedTemplateTests, integralBoundRejectsFloat32) {
    auto src =
        "package test;\n"
        "public class Bits<T extends Integral> {\n"
        "    public T v;\n"
        "    public Bits(T v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Bits<float32> b = heap Bits<float32>(1.5f);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Floating bound: accept float64.
TEST(NumericBoundedTemplateTests, floatingBoundAcceptsFloat64) {
    auto src =
        "package test;\n"
        "public class Scaled<T extends Floating> {\n"
        "    public T v;\n"
        "    public Scaled(T v) { this.v = v; }\n"
        "    public T halve() { return this.v / 2.0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        Scaled<float64> s = heap Scaled<float64>(10.0);\n"
        "        return s.halve();\n"
        "    }\n"
        "}\n";
    EXPECT_DOUBLE_EQ(runF64(src), 5.0);
}

// Floating bound: reject int32.
TEST(NumericBoundedTemplateTests, floatingBoundRejectsInt32) {
    auto src =
        "package test;\n"
        "public class Scaled<T extends Floating> {\n"
        "    public T v;\n"
        "    public Scaled(T v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Scaled<int32> s = heap Scaled<int32>(5);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// Distinct monomorphizations from one template — two different
// instantiations coexist in the same compilation unit.
TEST(NumericBoundedTemplateTests, distinctMonomorphizationsCoexist) {
    auto src =
        "package test;\n"
        "public class Adder<T extends Numeric> {\n"
        "    public T x;\n"
        "    public T y;\n"
        "    public Adder(T x, T y) { this.x = x; this.y = y; }\n"
        "    public T sum() { return this.x + this.y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Adder<int32> a = heap Adder<int32>(10, 5);\n"
        "        Adder<int64> b = heap Adder<int64>(100L, 50L);\n"
        "        int32 r = a.sum() + (int32) b.sum();\n"
        "        return r;\n"  // 15 + 150 = 165
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 165);
}
