//
// nucleo-nn-optim Unit 7 — MSE loss, train/eval mode, Dropout
// (plan 7.1.x; spec §7, §8; resolutions N8, N9).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runL(const std::string& body, const std::string& helpers = "") {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.nn.Losses;\n"
        "import cajeta.nucleo.nn.Modes;\n"
        "import cajeta.nucleo.nn.Dropout;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.concurrent.Tasks;\n"
        "public final class T {\n"
        + helpers +
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

// 7.1.1 — the three reductions match hand math: pred=[1,3], target=[0,1]:
// errors² = [1,4]; mean 2.5, sum 5, none elementwise.
TEST(LossModeTests, mseReductionsMatchHand) {
    float r = runL(
        "int64[] s = {2};\n"
        "        Tensor<float32> p = Tensor.zeros<float32>(s);\n"
        "        p.set1(0, 1.0f); p.set1(1, 3.0f);\n"
        "        Tensor<float32> t = Tensor.zeros<float32>(s);\n"
        "        t.set1(0, 0.0f); t.set1(1, 1.0f);\n"
        "        float32 m = Losses.mse(p, t);\n"
        "        float32 su = Losses.mseSum(p, t);\n"
        "        Tensor<float32> e = Losses.mseNone(p, t);\n"
        "        return m * 100.0f + su * 10.0f + e.get1(0) + e.get1(1) * 0.1f;");
    // 250 + 50 + 1 + 0.4 = 301.4
    EXPECT_NEAR(r, 301.4f, 1e-2f);
}

// 7.1.2 — Grad THROUGH the qualified loss call: d mse/d pred = 2(p-t)/n;
// and a custom free-function loss differentiates with no registration.
TEST(LossModeTests, gradThroughLossFunctions) {
    std::string customLoss =
        "    static float32 myLoss(Tensor<float32> p, Tensor<float32> t) {\n"
        "        return Tensor.sum<float32,float32>(\n"
        "            Tensor.mul<float32>(Tensor.sub<float32>(p, t),\n"
        "                                Tensor.sub<float32>(p, t)));\n"
        "    }\n";
    float r = runL(
        "int64[] s = {2};\n"
        "        Tensor<float32> p = Tensor.zeros<float32>(s);\n"
        "        p.set1(0, 1.0f); p.set1(1, 3.0f);\n"
        "        Tensor<float32> t = Tensor.zeros<float32>(s);\n"
        "        t.set1(0, 0.0f); t.set1(1, 1.0f);\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g =\n"
        "            Grad((Tensor<float32> pp, Tensor<float32> tt) ->\n"
        "                Losses.mse(pp, tt));\n"
        "        GradResult<float32, Tensor<float32>> r1 = g(p, t);\n"
        "        // d/dp = 2(p-t)/2 = (p-t) = [1, 2]\n"
        "        (Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>> g2 =\n"
        "            Grad((Tensor<float32> pp, Tensor<float32> tt) ->\n"
        "                myLoss(pp, tt));\n"
        "        GradResult<float32, Tensor<float32>> r2 = g2(p, t);\n"
        "        // d/dp = 2(p-t) = [2, 4]\n"
        "        return r1.value * 100.0f + r1.grads.get1(0) * 10.0f\n"
        "            + r1.grads.get1(1) + r2.grads.get1(1) * 0.1f;",
        customLoss);
    // 250 + 10 + 2 + 0.4 = 262.4
    EXPECT_NEAR(r, 262.4f, 1e-2f);
}

// 7.1.3 — mode scoping: default is EVAL; train() binds; eval nests inside
// train and restores on exit.
TEST(LossModeTests, modeScopesAndNests) {
    std::string probe =
        "    static float32 flag() {\n"
        "        if (Modes.isTraining()) { return 1.0f; }\n"
        "        return 0.0f;\n"
        "    }\n"
        "    static float32 acc;\n"
        "    static void inTrain() {\n"
        "        T.acc = T.acc + T.flag() * 10.0f;\n"       // train -> +10
        "        Modes.eval(() -> T.evalInner());\n"
        "        T.acc = T.acc + T.flag() * 100.0f;\n"      // restored -> +100
        "    }\n"
        "    static void evalInner() {\n"
        "        T.acc = T.acc + T.flag();\n"                // eval -> +0
        "    }\n";
    float r = runL(
        "T.acc = 0.0f;\n"
        "        float32 pre = T.flag();\n"                  // default eval -> 0
        "        Modes.train(() -> T.inTrain());\n"
        "        float32 post = T.flag();\n"                 // back to eval -> 0
        "        return T.acc + pre * 1000.0f + post * 1000.0f;",
        probe);
    EXPECT_NEAR(r, 110.0f, 1e-3f);
}

// 7.1.4 — reentrancy: two fibers in opposite modes observe their own mode
// (spawned work runs under its own fiber's bindings).
TEST(LossModeTests, modesAreFiberLocal) {
    std::string helpers =
        "    static float32 a;\n"
        "    static float32 b;\n"
        "    static async int32 trainSide() {\n"
        "        Modes.train(() -> T.markA());\n"
        "        return 0;\n"
        "    }\n"
        "    static void markA() {\n"
        "        if (Modes.isTraining()) { T.a = 1.0f; }\n"
        "    }\n"
        "    static async int32 evalSide() {\n"
        "        if (Modes.isTraining()) { T.b = 1.0f; }\n"  // must NOT see train
        "        return 0;\n"
        "    }\n";
    float r = runL(
        "T.a = 0.0f;\n"
        "        T.b = 0.0f;\n"
        "        scope {\n"
        "            int32 x = await spawn trainSide();\n"
        "            int32 y = await spawn evalSide();\n"
        "        }\n"
        "        return T.a * 10.0f + T.b;",
        helpers);
    // train side saw train (+10); eval side untouched (0)
    EXPECT_NEAR(r, 10.0f, 1e-3f);
}

// 7.1.5 — Dropout: eval = identity; train = ~p zeroed, survivors scaled by
// 1/(1-p); deterministic per seed; a fresh tensor either way.
TEST(LossModeTests, dropoutMaskAndIdentity) {
    std::string helpers =
        "    static float32 trainSum;\n"
        "    static float32 zeroCount;\n"
        "    static void trainPass(Dropout d, Tensor<float32> x) {\n"
        "        Tensor<float32> y = d.forward(x);\n"
        "        int64 i = 0;\n"
        "        while (i < 100) {\n"
        "            float32 v = y.get1(i);\n"
        "            T.trainSum = T.trainSum + v;\n"
        "            if (v == 0.0f) { T.zeroCount = T.zeroCount + 1.0f; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n";
    float r = runL(
        "int64[] s = {100};\n"
        "        Tensor<float32> x = Tensor.full<float32>(s, 1.0f);\n"
        "        Dropout d = heap Dropout(0.5f, 42);\n"
        "        Tensor<float32> ye = d.forward(x);\n"          // eval: identity
        "        float32 evalOk = 0.0f;\n"
        "        if (ye.get1(0) == 1.0f && ye.get1(99) == 1.0f) { evalOk = 1.0f; }\n"
        "        T.trainSum = 0.0f;\n"
        "        T.zeroCount = 0.0f;\n"
        "        Modes.train(() -> T.trainPass(d, x));\n"
        "        // ~50 zeros (Philox, seed 42 — tolerance wide); survivors are 2.0\n"
        "        float32 zc = T.zeroCount;\n"
        "        float32 ok = 0.0f;\n"
        "        if (zc > 30.0f && zc < 70.0f) { ok = 1.0f; }\n"
        "        float32 scaleOk = 0.0f;\n"
        "        if (T.trainSum == (100.0f - zc) * 2.0f) { scaleOk = 1.0f; }\n"
        "        return evalOk + ok * 10.0f + scaleOk * 100.0f;",
        helpers);
    EXPECT_NEAR(r, 111.0f, 1e-3f);
}
