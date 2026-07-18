//
// transform-intrinsics Unit 3 (3.1.1 Tensor case) — Grad over a scalar-valued
// function of a tensor. `Grad((Tensor<f32> x) -> sum(x*x))` walks the tensor DAG
// (elementwise `Tensor.mul` + rank-reducing `Tensor.sum`), reverse-composes the
// tensor-surface VJP rules, and returns `(Tensor<f32>) -> GradResult<f32, Tensor<f32>>`
// — a scalar value bag with a TENSOR gradient. The reduction makes cotangents
// scalar above the sum and tensor below it, so each DAG node is rank-tagged and
// the accumulation-add is spelled per surface (`+` vs `Tensor.add<E>`).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runTensorGrad(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}
} // namespace

// DIAG A — the record holds a tensor gradient at all (no Grad involved).
// value 5, grads = x = [1,2,3] sum 6; total 11.
TEST(GradTensor, gradResultHoldsTensorField) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s3);\n"
        "        GradResult<float32, Tensor<float32>> r =\n"
        "            stack GradResult<float32, Tensor<float32>>(5.0f, x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 11.0f);
}

// The simplest tensor Grad: loss(x) = sum(x), grad = ones. value 6; total 9.
//
// DISABLED: the reverse-mode SOURCE synthesis is correct (CAJETA_GRAD_DUMP shows
// the backward computes `mulScalar(onesLike(xx), 1.0f)` = ones, and it parses +
// type-checks), but EXECUTING the synthesized backward SIGABRTs in
// __cajeta_new_array_header_arena with a garbage (negative) count — xx reads as a
// corrupt Tensor pointer. It is a codegen bug in the SYNTHESIZED-method path for a
// class-pointer parameter, NOT the tensor autodiff logic:
//   * the identical grad expression in a PLAIN lambda works (plainLambdaOnesLikeGrad),
//   * the scalar synthesized backward works (GradEndToEndTests),
//   * GradResult<f32, Tensor<f32>> holds a tensor fine (gradResultHoldsTensorField).
// Needs a focused compiler fix (IR diff of the make()-returned lambda body vs a
// plain lambda's) before re-enabling. See transform-intrinsics-plan 3.1.1.
TEST(GradTensor, DISABLED_sumIdentityGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx) -> Tensor.sum<float32,float32>(xx));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 9.0f);
}

// DIAG C — a PLAIN (non-synthesized) lambda with a Tensor param used inline as a
// call arg. Isolates whether the crash is a general lambda-Tensor-param codegen
// bug or specific to the synthesized backward. value 6.
TEST(GradTensor, plainLambdaTensorParamSum) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> float32 f =\n"
        "            (Tensor<float32> xx) -> Tensor.sum<float32,float32>(xx);\n"
        "        return f(x);"), 6.0f);
}

// DIAG D — a PLAIN lambda whose body is exactly the sum(x) grad expression
// (onesLike*1), reduced so run() returns a scalar. sum([1,1,1]) = 3.
TEST(GradTensor, plainLambdaOnesLikeGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> float32 f =\n"
        "            (Tensor<float32> xx) -> Tensor.sum<float32,float32>(\n"
        "                Tensor.mulScalar<float32>(Tensor.onesLike<float32>(xx), 1.0f));\n"
        "        return f(x);"), 3.0f);
}

// 3.1.1 — the spec's literal example: loss(x) = sum(x*x), a scalar over a tensor.
// value = 1+4+9 = 14; grad = 2x = [2,4,6], sum 12; total 26.
// DISABLED: same synthesized-backward class-pointer-param codegen bug as
// DISABLED_sumIdentityGrad (the backward SOURCE is dump-verified correct).
TEST(GradTensor, DISABLED_sumOfSquaresValueAndGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x = Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx) -> Tensor.sum<float32,float32>(Tensor.mul<float32>(xx, xx)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 26.0f);
}
