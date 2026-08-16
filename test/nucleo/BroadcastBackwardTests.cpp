//
// nucleo-nn-optim Unit 4 — broadcast-add backward (plan 4.1.x).
//
// Bias add is `[B,O] + [O]`: the forward broadcasts the bias over the batch
// axis, so the bias COTANGENT must be the batch-axis SUM of the upstream
// grad, restored to the bias's own shape (numpy right-aligned rules). The
// shipped `add` rule passed the upstream through untouched — correct only
// for same-shape operands. `Tensor.sumTo(g, like)` is the shape-restoring
// reduction the widened add/sub rules emit.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runT(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1e9f;
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1e9f;
    return fn();
}
} // namespace

// 4.0 — Tensor.sumTo itself: [3,2] summed to [2] is the column sums; a
// same-shape sumTo is the identity (as a fresh tensor).
TEST(BroadcastBackwardTests, sumToReducesBroadcastAxes) {
    float r = runT(
        "int64[] gs = [3, 2];\n"
        "        Tensor<float32> g #= Tensor.zeros<float32>(gs);\n"
        "        g.set2(0, 0, 1.0f); g.set2(0, 1, 2.0f);\n"
        "        g.set2(1, 0, 10.0f); g.set2(1, 1, 20.0f);\n"
        "        g.set2(2, 0, 100.0f); g.set2(2, 1, 200.0f);\n"
        "        int64[] ls = [2];\n"
        "        Tensor<float32> like #= Tensor.zeros<float32>(ls);\n"
        "        Tensor<float32> red #= Tensor.sumTo<float32>(g, like);\n"
        "        Tensor<float32> same #= Tensor.sumTo<float32>(g, g);\n"
        "        return red.get1(0) + red.get1(1) * 0.001f\n"
        "            + same.get2(1, 1) * 0.00001f;");
    // red = [111, 222] -> 111 + 0.222; same[1,1] = 20 -> 0.0002
    EXPECT_NEAR(r, 111.2222f, 1e-3f);
}

// 4.1.1 — Grad<1> through add([B,O], [O]): the bias grad is the batch-axis
// sum (here B=3, upstream = ones from sum -> bias grad = [3, 3]).
TEST(BroadcastBackwardTests, biasGradIsBatchSum) {
    float r = runT(
        "int64[] xs = [3, 2];\n"
        "        Tensor<float32> x #= Tensor.full<float32>(xs, 1.0f);\n"
        "        int64[] bs = [2];\n"
        "        Tensor<float32> b #= Tensor.full<float32>(bs, 0.5f);\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g1 =\n"
        "            Grad<1>((Tensor<float32> xi, Tensor<float32> bi) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.add<float32>(xi, bi)));\n"
        "        GradResult<float32, Tensor<float32>> r1 = g1(x, b);\n"
        "        // value = 6*1 + 3*(0.5+0.5) ... = 6 + 3 = 9; db = [3, 3]\n"
        "        return r1.value * 100.0f + r1.grads.get1(0) * 10.0f\n"
        "            + r1.grads.get1(1);");
    // 900 + 30 + 3 = 933
    EXPECT_NEAR(r, 933.0f, 1e-2f);
}

// 4.1.2 — the same through GradAll in the Linear shape, batch 3: the weight
// grad stays [in,out]-shaped, the bias grad is [out] batch-summed.
TEST(BroadcastBackwardTests, gradAllLinearShapeBatchThree) {
    float r = runT(
        "int64[] ws = [2, 2];\n"
        "        Tensor<float32> w #= Tensor.full<float32>(ws, 0.5f);\n"
        "        int64[] bs = [2];\n"
        "        Tensor<float32> b #= Tensor.full<float32>(bs, 0.1f);\n"
        "        int64[] xs = [3, 2];\n"
        "        Tensor<float32> x #= Tensor.full<float32>(xs, 2.0f);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> step =\n"
        "            GradAll<2>((Tensor<float32> wp, Tensor<float32> bp,\n"
        "                        Tensor<float32> xp) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.add<float32>(\n"
        "                    Tensor.matmul<float32>(xp, wp), bp)));\n"
        "        GradResult<float32, Tensor<float32>[]> g = step(w, b, x);\n"
        "        // forward cell = 2*0.5*2 + 0.1 = 2.1; 6 cells -> 12.6\n"
        "        // dW = X^T @ ones = each cell 3*2 = 6; db = [3, 3]\n"
        "        return g.value * 10.0f + g.grads[0].get2(0, 0)\n"
        "            + g.grads[1].get1(0) * 0.1f + g.grads[1].get1(1) * 0.01f;");
    // 126 + 6 + 0.3 + 0.03 = 132.33
    EXPECT_NEAR(r, 132.33f, 1e-2f);
}
