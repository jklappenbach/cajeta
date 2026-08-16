//
// nucleo-expr Unit 1 — the mechanism spike (plan 1.1.1). Before building the
// engine, prove the ONE assumption the plan rests on: that `Fuse(lambda)` can
// ride the transform-intrinsics recognizer + buildDag + Tier-A synthesis, the
// same path Grad/Vmap/Jit already use, and evaluate correctly.
//
// If this cannot be made to work, the plan STOPS here and re-plans (the
// recorded fallback), rather than building U2-U4 on a bad assumption.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runTensor(const std::string& body) {
    std::string src =
        "package test;\n"
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

// 1.1.1 — the smallest case: Fuse over one elementwise op, forced with eval.
// t = {1,2,3}; add(t,t) = {2,4,6}; sum = 12.
TEST(FuseSpike, singleElementwiseOpEvaluates) {
    EXPECT_FLOAT_EQ(runTensor(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> #Tensor<float32> g =\n"
        "            Fuse((Tensor<float32> t) -> Tensor.add<float32>(t, t));\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r);"), 12.0f);
}

// 1.1.3 — a 3-op elementwise chain: (t*t + t) - t == t*t.
// t = {1,2,3} -> {1,4,9}; sum = 14.
TEST(FuseSpike, threeOpChainEvaluates) {
    EXPECT_FLOAT_EQ(runTensor(
        "float32[] fa = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(fa, s);\n"
        "        (Tensor<float32>) -> #Tensor<float32> g = Fuse((Tensor<float32> t) ->\n"
        "            Tensor.sub<float32>(Tensor.add<float32>(Tensor.mul<float32>(t, t), t), t));\n"
        "        Tensor<float32> r = g(x);\n"
        "        return Tensor.sum<float32,float32>(r);"), 14.0f);
}
