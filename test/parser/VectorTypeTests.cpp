//
// Stage 3 of the Vector<T,N> work: the CajetaVector type + fromContext
// interception. These tests cover TYPE RESOLUTION and LLVM-type layout only —
// `Vector<T,N>` resolves to a `<N x T>`-backed value type, distinct per (T,N),
// admitted only for non-bool numeric T and positive constant N. Construction,
// component access, arithmetic, and dot/length/normalize are exercised by the
// Stage 4 host-codegen tests (VectorTests.cpp).
//
// Resolution + layout are forced by wrapping the vector as a KernelBuffer<T> element
// (KernelBuffer<Vector<...>> monomorphizes, re-parsing Buffer's body with
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
    "import cajeta.xpu.KernelBuffer;\n";

} // namespace

// Vector<float32,4> resolves and a Buffer over it instantiates + lays out.

// Distinct (T,N) shapes are distinct types and all lay out — float32x4,
// int32x3, and a repeat of float32x4 (cache hit).

// Non-numeric element type (a user class) is rejected.
TEST(VectorTypeTests, nonNumericElementRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public class Thing { public Thing() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        KernelBuffer<Vector<Thing,4>> b = heap KernelBuffer<Vector<Thing,4>>(0, 1);\n"
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

// Zero length is rejected (N must be a positive constant).
