//
// nucleo-nn-optim Unit 3 — Linear, forward composition, the params↔args
// bridge (plan 3.1.x; spec §2).
//
// The bridge contract (resolution N10): a GradAll functional step spells the
// parameters as its LEADING args in `parameters()` order; grads come back in
// that same order, so grads[i] belongs to ps[i]. The lambda's arity is fixed
// at compile time — the "flatten" is the ORDER contract, not runtime codegen.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runNn(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.nn.Module;\n"
        "import cajeta.nucleo.nn.Parameter;\n"
        "import cajeta.nucleo.nn.Linear;\n"
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

// 3.1.1 — forward matches hand math once weights are pinned: W = [[1,2],[3,4]]
// (row-major [in,out]), b = [10, 20], x = [[1, 1]] -> y = [1+3+10, 2+4+20].
TEST(LinearTests, forwardMatchesReference) {
    float r = runNn(
        "Linear fc = heap Linear(2, 2, 7);\n"
        "        Parameter[] ps = fc.parameters();\n"
        "        Tensor<float32> w = ps[0].get();\n"
        "        w.set2(0, 0, 1.0f); w.set2(0, 1, 2.0f);\n"
        "        w.set2(1, 0, 3.0f); w.set2(1, 1, 4.0f);\n"
        "        Tensor<float32> b = ps[1].get();\n"
        "        b.set1(0, 10.0f); b.set1(1, 20.0f);\n"
        "        int64[] xs = {1, 2};\n"
        "        Tensor<float32> x = Tensor.full<float32>(xs, 1.0f);\n"
        "        Tensor<float32> y = fc.forward(x);\n"
        "        return y.get2(0, 0) * 100.0f + y.get2(0, 1);");
    // y = [14, 26] -> 1426
    EXPECT_NEAR(r, 1426.0f, 1e-2f);
}

// 3.1.1b — seeded init: same seed twice gives identical weights; weights lie
// in (-1/sqrt(in), +1/sqrt(in)); bias starts at zero.
TEST(LinearTests, seededInitDeterministicAndBounded) {
    float r = runNn(
        "Linear a = heap Linear(4, 3, 99);\n"
        "        Linear b = heap Linear(4, 3, 99);\n"
        "        Parameter[] pa = a.parameters();\n"
        "        Parameter[] pb = b.parameters();\n"
        "        float32 acc = 0.0f;\n"
        "        int64 r = 0;\n"
        "        while (r < 4) {\n"
        "            int64 c = 0;\n"
        "            while (c < 3) {\n"
        "                float32 va = pa[0].get().get2(r, c);\n"
        "                float32 vb = pb[0].get().get2(r, c);\n"
        "                if (va != vb) { acc = acc + 1.0f; }\n"
        "                if (va <= -0.5f || va >= 0.5f) { acc = acc + 100.0f; }\n"
        "                c = c + 1;\n"
        "            }\n"
        "            r = r + 1;\n"
        "        }\n"
        "        if (pa[1].get().get1(0) != 0.0f) { acc = acc + 1000.0f; }\n"
        "        return acc;");
    EXPECT_NEAR(r, 0.0f, 1e-6f);   // no mismatches, all in bound, zero bias
}

// 3.1.2 — two-layer MLP forward with relu between matches composed hand math.
TEST(LinearTests, mlpForwardComposes) {
    float r = runNn(
        "Linear fc1 = heap Linear(2, 2, 1);\n"
        "        Linear fc2 = heap Linear(2, 1, 2);\n"
        "        Parameter[] p1 = fc1.parameters();\n"
        "        p1[0].get().set2(0, 0, 1.0f); p1[0].get().set2(0, 1, -1.0f);\n"
        "        p1[0].get().set2(1, 0, 1.0f); p1[0].get().set2(1, 1, 1.0f);\n"
        "        p1[1].get().set1(0, 0.0f); p1[1].get().set1(1, 0.0f);\n"
        "        Parameter[] p2 = fc2.parameters();\n"
        "        p2[0].get().set2(0, 0, 2.0f); p2[0].get().set2(1, 0, 3.0f);\n"
        "        p2[1].get().set1(0, 5.0f);\n"
        "        int64[] xs = {1, 2};\n"
        "        Tensor<float32> x = Tensor.zeros<float32>(xs);\n"
        "        x.set2(0, 0, 1.0f); x.set2(0, 1, 2.0f);\n"
        "        Tensor<float32> h = Tensor.relu<float32>(fc1.forward(x));\n"
        "        Tensor<float32> y = fc2.forward(h);\n"
        "        return y.get2(0, 0);");
    // h_pre = [1*1+2*1, 1*(-1)+2*1] = [3, 1]; relu -> [3, 1]
    // y = 3*2 + 1*3 + 5 = 14
    EXPECT_NEAR(r, 14.0f, 1e-3f);
}

// 3.1.3 — the bridge: a GradAll<2> functional step over (w, b, x, t) in
// parameters() order; grads[i] belongs to ps[i] — writing -g into ps[i]
// changes the module's OWN forward exactly as the math says.
TEST(LinearTests, gradAllBridgeOrderContract) {
    float r = runNn(
        "Linear fc = heap Linear(2, 1, 3);\n"
        "        Parameter[] ps = fc.parameters();\n"
        "        ps[0].get().set2(0, 0, 1.0f); ps[0].get().set2(1, 0, 2.0f);\n"
        "        ps[1].get().set1(0, 0.5f);\n"
        "        int64[] xs = {1, 2};\n"
        "        Tensor<float32> x = Tensor.zeros<float32>(xs);\n"
        "        x.set2(0, 0, 1.0f); x.set2(0, 1, 1.0f);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> step =\n"
        "            GradAll<2>((Tensor<float32> w, Tensor<float32> b,\n"
        "                        Tensor<float32> xi) ->\n"
        "                Tensor.sum<float32,float32>(Tensor.add<float32>(\n"
        "                    Tensor.matmul<float32>(xi, w), b)));\n"
        "        GradResult<float32, Tensor<float32>[]> g =\n"
        "            step(ps[0].get(), ps[1].get(), x);\n"
        "        // loss = x·w + b summed = 1+2+0.5 = 3.5; dw = [1,1]; db = [1]\n"
        "        float32 acc = g.value * 100.0f;\n"
        "        acc = acc + g.grads[0].get2(0, 0) + g.grads[0].get2(1, 0) * 10.0f;\n"
        "        acc = acc + g.grads[1].get1(0) * 0.5f;\n"
        "        // write-back by position: p_i -= g_i elementwise (lr = 1)\n"
        "        ps[0].get().set2(0, 0, ps[0].get().get2(0, 0) - g.grads[0].get2(0, 0));\n"
        "        ps[0].get().set2(1, 0, ps[0].get().get2(1, 0) - g.grads[0].get2(1, 0));\n"
        "        ps[1].get().set1(0, ps[1].get().get1(0) - g.grads[1].get1(0));\n"
        "        Tensor<float32> y2 = fc.forward(x);\n"
        "        // new: w=[0,1], b=[-0.5] -> y = 0+1-0.5 = 0.5\n"
        "        return acc + y2.get2(0, 0) * 10000.0f;");
    // 350 + (1 + 10) + 0.5 + 5000 = 5361.5
    EXPECT_NEAR(r, 5361.5f, 1e-1f);
}
