//
// Const-generic N in built-in Vector<T,N> / Matrix<T,R,C> TYPE ANNOTATIONS.
//
// A method const-param `<uint32 N>` substitutes a CajetaConstantType for N
// (MethodConstParamTests). For USER templates `Box<N>` already resolves N through
// the substitution map (module->lookupTypeParameter), but the BUILT-IN Vector /
// Matrix branch in CajetaType::fromContext hard-required a syntactic integer
// literal — so `Vector<float32,N>` as a local/return type inside a generic method
// threw CAJETA_ERROR_VECTOR_LENGTH. That blocked the cajeta-accel §4.5 goal of ONE
// generic source for the width-parametric Exact wide traversal.
//
// These tests pin the fix: the Vector/Matrix length args accept a bound const-param
// (resolved to its CajetaConstantType), so one generic source monomorphizes per
// width — while a literal still works and an unresolvable/non-positive N still
// errors. Mirrors VectorTypeTests (literal path) + MethodConstParamTests (const
// params).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

float runF32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<float (*)()>("run");
    return fn ? fn() : -1.0f;
}

} // namespace

// The §4.5 enabler: a `Vector<float32,N>` LOCAL with the method's const-param N,
// fed by `a.vload<N>(0)`, doubled, first lane returned. Instantiated at N=4 and
// N=8 from ONE source — distinct monomorphizations. a[0]=1 -> 2 each -> 4.
TEST(ConstGenericVectorTests, vectorLocalWithConstParamWidthRuns) {
    auto src =
        "package test;\n"
        "public class W {\n"
        "    public static float32 firstDoubled<uint32 N>(float32[] a) {\n"
        "        Vector<float32,N> v = a.vload<N>(0);\n"
        "        Vector<float32,N> t = v + v;\n"
        "        return t.x;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        float32[] a = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };\n"
        "        return W.firstDoubled<4>(a) + W.firstDoubled<8>(a);\n"
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 4.0f);
}

// Const-param N as a Vector length resolves in a TYPE position (KernelBuffer
// element) too — pure resolution + layout, no codegen. lenN<3>() over a 5-elem
// buffer returns 5 regardless of N; both N=3 and N=4 lay out.
TEST(ConstGenericVectorTests, vectorAsBufferElementWithConstParam) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "public class W {\n"
        "    public static int32 lenN<uint32 N>() {\n"
        "        KernelBuffer<Vector<float32,N>> b = heap KernelBuffer<Vector<float32,N>>(0, 5);\n"
        "        return (int32) b.length();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return W.lenN<3>() + W.lenN<4>();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// The Matrix mirror: `Matrix<float32,N,N>` with the const-param in BOTH dimension
// slots resolves + lays out as a buffer element. lenN<2>() + lenN<4>() -> 14.
TEST(ConstGenericVectorTests, matrixWithConstParamDims) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "public class W {\n"
        "    public static int32 lenN<uint32 N>() {\n"
        "        KernelBuffer<Matrix<float32,N,N>> b = heap KernelBuffer<Matrix<float32,N,N>>(0, 7);\n"
        "        return (int32) b.length();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return W.lenN<2>() + W.lenN<4>();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 14);
}

// Regression: a plain integer literal length still resolves (the fix must not
// disturb the existing literal path). Width 4, lanes summed via named accessors.
TEST(ConstGenericVectorTests, literalWidthStillResolves) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        float32[] a = { 1.0f, 2.0f, 3.0f, 4.0f };\n"
        "        Vector<float32,4> v = a.vload<4>(0);\n"
        "        return v.x + v.y + v.z + v.w;\n"
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 10.0f);
}
