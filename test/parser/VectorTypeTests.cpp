//
// Stage 3 of the Vector<T,N> work: the CajetaVector type + fromContext
// interception. These tests cover TYPE RESOLUTION and LLVM-type layout only —
// `Vector<T,N>` resolves to a `<N x T>`-backed value type, distinct per (T,N),
// admitted only for non-bool numeric T and positive constant N. Construction,
// component access, arithmetic, and dot/length/normalize are exercised by the
// Stage 4 host-codegen tests (VectorTests.cpp).
//
// Resolution + layout are forced by wrapping the vector as a Buffer<T> element
// (Buffer<Vector<...>> monomorphizes, re-parsing Buffer's body with
// T=Vector<...> and laying out the vector's LLVM type); the 2-arg Buffer ctor
// stores a handle/count without touching a device, so this runs under the host
// JIT.
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

const char* IMPORTS =
    "import cajeta.xpu.core.Buffer;\n";

} // namespace

// Vector<float32,4> resolves and a Buffer over it instantiates + lays out.
TEST(VectorTypeTests, float4ResolvesAsBufferElement) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Vector<float32,4>> b = heap Buffer<Vector<float32,4>>(0, 5);\n"
        "        return (int32) b.length();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Distinct (T,N) shapes are distinct types and all lay out — float32x4,
// int32x3, and a repeat of float32x4 (cache hit).
TEST(VectorTypeTests, multipleShapesResolveDistinctly) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Vector<float32,4>> a = heap Buffer<Vector<float32,4>>(0, 2);\n"
        "        Buffer<Vector<int32,3>>   b = heap Buffer<Vector<int32,3>>(0, 3);\n"
        "        Buffer<Vector<float32,4>> c = heap Buffer<Vector<float32,4>>(0, 5);\n"
        "        return (int32)(a.length() + b.length() + c.length());\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// Non-numeric element type (a user class) is rejected.
TEST(VectorTypeTests, nonNumericElementRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public class Thing { public Thing() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Vector<Thing,4>> b = heap Buffer<Vector<Thing,4>>(0, 1);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VECTOR_ELEMENT_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
    }
}

// bool is numeric-flagged but excluded (ABI-ambiguous <N x i1>).
TEST(VectorTypeTests, boolElementRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Vector<boolean,4>> b = heap Buffer<Vector<boolean,4>>(0, 1);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VECTOR_ELEMENT_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_ELEMENT_TYPE");
    }
}

// Zero length is rejected (N must be a positive constant).
TEST(VectorTypeTests, zeroLengthRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Vector<float32,0>> b = heap Buffer<Vector<float32,0>>(0, 1);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VECTOR_LENGTH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VECTOR_LENGTH");
    }
}
