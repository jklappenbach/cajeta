//
// transform-intrinsics Unit 3, Step 1a — validate the GradResult<V,G> return bag
// (cajeta.nucleo.transform.GradResult) in HAND-WRITTEN code before the autodiff
// synthesis depends on it. Isolates two flagged risks: a generic record first
// used in a fresh nucleo package, and returning a value-type record BY VALUE
// (the value-type-return path). JIT path only (the AOT -O3 value-return hazard
// is out of scope here — U3 acceptance is JIT-tested).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}
} // namespace

// Stack (value-type) construction + return BY VALUE across a call — the shape the
// synthesized backward uses. r.value = 9, r.grads = 6 -> 15.
TEST(GradResultRecord, stackCtorReturnByValue) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    static GradResult<float32,float32> make(float32 x) {\n"
        "        return stack GradResult<float32,float32>(x * x, 2.0f * x);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        GradResult<float32,float32> r = make(3.0f);\n"
        "        return r.value + r.grads;\n"
        "    }\n"
        "}\n"), 15.0f);
}

// The make()-returns-lambda shape the U3 synthesizer emits: a static returns the
// backward as a lambda constructing GradResult by value; the caller stores the
// function value and invokes it. This is the hand-written mirror of the generated
// source — validating the return strategy before the parse-extract plumbing.
TEST(GradResultRecord, makeReturnsLambdaConstructingGradResult) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    static (float32) -> GradResult<float32,float32> make() {\n"
        "        return (float32 x) -> stack GradResult<float32,float32>(x * x, 2.0f * x);\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        (float32) -> GradResult<float32,float32> g = make();\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value + r.grads;\n"
        "    }\n"
        "}\n"), 15.0f);
}

// Heap construction of the same generic record (the RecPair-tested path).
TEST(GradResultRecord, heapCtorReadFields) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static float32 run() {\n"
        "        GradResult<float32,float32> r = heap GradResult<float32,float32>(9.0f, 6.0f);\n"
        "        return r.value + r.grads;\n"
        "    }\n"
        "}\n"), 15.0f);
}
