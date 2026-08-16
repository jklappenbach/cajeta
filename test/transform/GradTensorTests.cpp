//
// transform-intrinsics Unit 3 (3.1.1 Tensor case) — Grad over a scalar-valued
// function of a tensor. `Grad((Tensor<f32> x) -> sum(x*x))` walks the tensor DAG
// (elementwise `Tensor.mul` + rank-reducing `Tensor.sum`), reverse-composes the
// tensor-surface VJP rules, and returns `(Tensor<f32>) -> GradResult<f32, Tensor<f32>>`
// — a scalar value bag with a TENSOR gradient. The reduction makes cotangents
// scalar above the sum and tensor below it, so each DAG node is rank-tagged and
// the accumulation-add is spelled per surface (`+` vs `Tensor.add<E>`).
//
// `GradResult<f32, Tensor<f32>>` is a generic value-type record that OWNS the heap
// grad in its field — sound because the ctor `#=`-transfers ownership into the field
// (the same idiom `MapEntry<K,V>` uses for `HashMap<_, class>`). A plain `=` borrow-
// stored it and the returned copy dangled (fixed 2026-07-18).
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
float runWithSrc(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}
} // namespace

// A value-type record that OWNS a heap tensor field, returned across a frame — the
// pattern the tensor backward uses. Sound because `GradResult`'s ctor `#=`-transfers
// the owned grad into the field (the fix; a plain `=` borrow-stored it and the
// returned copy dangled). Same idiom as `MapEntry`'s `#= value`. value 6 + 3 → 9.
TEST(GradTensor, valueTypeRecordOwnedTensorFieldReturn) {
    EXPECT_FLOAT_EQ(runWithSrc(
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static GradResult<float32, Tensor<float32>> mk(Tensor<float32> xx) {\n"
        "        return stack GradResult<float32, Tensor<float32>>(\n"
        "            Tensor.sum<float32,float32>(xx),\n"
        "            Tensor.mulScalar<float32>(Tensor.onesLike<float32>(xx), 1.0f));\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        GradResult<float32, Tensor<float32>> r = mk(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);\n"
        "    }\n"
        "}\n"), 9.0f);
}

// The record holds a tensor gradient when built + read in the SAME frame (no
// cross-frame return, so no arena-escape). value 5 + sum(x [1,2,3])=6 → 11.
TEST(GradTensor, gradResultHoldsTensorField) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        GradResult<float32, Tensor<float32>> r =\n"
        "            stack GradResult<float32, Tensor<float32>>(5.0f, x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 11.0f);
}

// A tensor arena allocation AFTER a Tensor.sum, in-frame — the order the tensor
// backward uses (value=sum first, grad alloc second). In-frame it is fine; only
// the cross-frame RETURN of the allocated tensor dangles. value 6 + 3 → 9.
TEST(GradTensor, arenaAllocAfterSum) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        float32 s = Tensor.sum<float32,float32>(x);\n"
        "        Tensor<float32> o #= Tensor.onesLike<float32>(x);\n"
        "        return s + Tensor.sum<float32,float32>(o);"), 9.0f);
}

// A plain lambda with a Tensor param used inline as a call arg works — the tensor
// autodiff logic and closure/param handling are sound. value 6.
TEST(GradTensor, plainLambdaTensorParamSum) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> float32 f =\n"
        "            (Tensor<float32> xx) -> Tensor.sum<float32,float32>(xx);\n"
        "        return f(x);"), 6.0f);
}

// The exact sum(x) grad expression (onesLike*1) in a plain lambda, reduced so it
// does NOT escape (consumed by sum) — works. sum([1,1,1]) = 3.
TEST(GradTensor, plainLambdaOnesLikeGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> float32 f =\n"
        "            (Tensor<float32> xx) -> Tensor.sum<float32,float32>(\n"
        "                Tensor.mulScalar<float32>(Tensor.onesLike<float32>(xx), 1.0f));\n"
        "        return f(x);"), 3.0f);
}

// 3.1.1 (simplest tensor Grad) — loss(x) = sum(x), grad = ones. value 6 + 3 → 9.
TEST(GradTensor, sumIdentityGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx) -> Tensor.sum<float32,float32>(xx));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 9.0f);
}

// 3.1.1 — the spec's literal example: loss(x) = sum(x*x), grad = 2x.
// value 14 + sum([2,4,6])=12 → 26.
TEST(GradTensor, sumOfSquaresValueAndGrad) {
    EXPECT_FLOAT_EQ(runTensorGrad(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s3);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> xx) -> Tensor.sum<float32,float32>(Tensor.mul<float32>(xx, xx)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return r.value + Tensor.sum<float32,float32>(r.grads);"), 26.0f);
}
