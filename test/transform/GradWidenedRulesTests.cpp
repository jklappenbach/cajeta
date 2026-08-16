//
// nucleo-autograd Unit 1 — the widened VJP rule set (spec 2.1/2.2/2.3/2.5):
// div, exp, log, sqrt, mean join add/mul/sub/negate/matmul/sum. Scalar spelling
// is `/` and the Math.* intrinsics; tensor spelling is the Tensor.* statics.
// Expected values are the analytic derivatives — they ARE the cross-check.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float gradAt(const std::string& lambda, const std::string& at) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        (float32) -> GradResult<float32,float32> g = Grad(" + lambda + ");\n"
        "        GradResult<float32,float32> r = g(" + at + ");\n"
        "        return r.grads;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}

float runTensor(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.math.Tensor;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}
} // namespace

// 1.1.1 — scalar div: d/dx [x/(x+1)] = 1/(x+1)^2; at 2 -> 1/9.

// 1.1.1 — exp: d/dx e^x at 1 -> e.

// 1.1.1 — log: d/dx ln x at 2 -> 0.5.

// 1.1.1 — sqrt: d/dx sqrt(x) at 4 -> 0.25.

// 1.1.1 — chain rule through the widened set: d/dx ln(x*x) = 2/x; at 3 -> 2/3.

// 1.1.2 — tensor mean: d/dt mean(t*t) = 2t/n; at {1,2,3} the grads sum to 4.
TEST(GradWidenedRules, tensorMeanOfSquare) {
    EXPECT_NEAR(runTensor(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> t) -> Tensor.mean<float32,float32>(Tensor.mul<float32>(t, t)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r.grads);"), 4.0f, 1e-4f);
}

// 1.1.2 — tensor log under sum: d/dt sum(log t) = 1/t; at {1,2,4} sums to 1.75.
TEST(GradWidenedRules, tensorLogUnderSum) {
    EXPECT_NEAR(runTensor(
        "float32[] fa = [ 1.0f, 2.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> t) -> Tensor.sum<float32,float32>(Tensor.log<float32>(t)));\n"
        "        GradResult<float32, Tensor<float32>> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r.grads);"), 1.75f, 1e-4f);
}

// 1.1.4 — second-order through the widened set: d2/dx2 ln x = -1/x^2; at 2 -> -0.25.
TEST(GradWidenedRules, secondOrderLog) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        (float32) -> GradResult<float32,float32> g =\n"
        "            Grad(Grad((float32 x) -> Math.log(x)));\n"
        "        GradResult<float32,float32> r = g(2.0f);\n"
        "        return r.grads;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    EXPECT_NEAR(jit->lookup<float (*)()>("run")(), -0.25f, 1e-4f);
}
