//
// nucleo-nn-optim Unit 5 — the optimizer protocol: SGD, Adam, AdamW
// (plan 5.1.x; spec §5; resolution N6).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runOpt(const std::string& body, const std::string& extraClasses = "") {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.nn.Module;\n"
        "import cajeta.nucleo.nn.Parameter;\n"
        "import cajeta.nucleo.optim.Optimizer;\n"
        "import cajeta.nucleo.optim.OptimizerException;\n"
        "import cajeta.nucleo.optim.SGD;\n"
        "import cajeta.nucleo.optim.Adam;\n"
        "import cajeta.nucleo.optim.AdamW;\n"
        + extraClasses +
        "public class Net extends Module {\n"
        "    Parameter w;\n"
        "    public Net(float32 init) {\n"
        "        int64[] s = {2};\n"
        "        this.w = heap Parameter(Tensor.full<float32>(s, init));\n"
        "    }\n"
        "}\n"
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

// One grads bag: a single [2] tensor of `g`.
const char* kGrads =
    "int64[] gs = {2};\n"
    "        Tensor<float32>[] grads = heap Tensor<float32>[1];\n"
    "        grads[0] = Tensor.full<float32>(gs, 1.0f);\n";
} // namespace

// 5.1.2a — SGD: p = 1, g = 1, lr = 0.1 -> 0.9; second step -> 0.8.
TEST(OptimizerTests, sgdPlainMatchesHand) {
    float r = runOpt(std::string(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap SGD(net.parameters(), 0.1f, 0.0f);\n"
        "        ") + kGrads +
        "        opt.step(grads);\n"
        "        float32 p1 = net.w.get().get1(0);\n"
        "        opt.step(grads);\n"
        "        return p1 * 100.0f + net.w.get().get1(1) * 10.0f;");
    // 90 + 8 = 98
    EXPECT_NEAR(r, 98.0f, 1e-3f);
}

// 5.1.2b — SGD momentum 0.9, constant g=1: v1=1 (p -= .1), v2=1.9 (p -= .19).
TEST(OptimizerTests, sgdMomentumMatchesHand) {
    float r = runOpt(std::string(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap SGD(net.parameters(), 0.1f, 0.9f);\n"
        "        ") + kGrads +
        "        opt.step(grads);\n"
        "        opt.step(grads);\n"
        "        return net.w.get().get1(0);");
    EXPECT_NEAR(r, 0.71f, 1e-4f);
}

// 5.1.2c — Adam step 1 with g=1, lr=0.1: bias correction makes the update
// exactly lr (up to eps): p = 0.9. Step 2 ≈ 0.8 (state persisted, 5.1.3).
TEST(OptimizerTests, adamBiasCorrectionAndState) {
    float r = runOpt(std::string(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap Adam(net.parameters(), 0.1f);\n"
        "        ") + kGrads +
        "        opt.step(grads);\n"
        "        float32 p1 = net.w.get().get1(0);\n"
        "        opt.step(grads);\n"
        "        return p1 * 10.0f + net.w.get().get1(0);");
    // 9 + 0.8 = 9.8
    EXPECT_NEAR(r, 9.8f, 1e-3f);
}

// 5.1.2d — AdamW decouples decay: step 1 with wd=0.1 gives p = 1 - 0.1*(1 +
// 0.1*1) = 0.89 — NOT Adam-plus-L2's 0.9 (the moments never see the decay).
TEST(OptimizerTests, adamWDecouplesWeightDecay) {
    float r = runOpt(std::string(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap AdamW(net.parameters(), 0.1f, 0.1f);\n"
        "        ") + kGrads +
        "        opt.step(grads);\n"
        "        return net.w.get().get1(0);");
    EXPECT_NEAR(r, 0.89f, 1e-3f);
}

// 5.1.4 — fail-loud: wrong grad count and wrong shape both throw
// OptimizerException BEFORE any write (param stays bit-identical).
TEST(OptimizerTests, mismatchThrowsNoPartialUpdate) {
    float r = runOpt(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap SGD(net.parameters(), 0.1f, 0.0f);\n"
        "        Tensor<float32>[] wrongCount = heap Tensor<float32>[2];\n"
        "        int64[] gs = {2};\n"
        "        wrongCount[0] = Tensor.full<float32>(gs, 1.0f);\n"
        "        wrongCount[1] = Tensor.full<float32>(gs, 1.0f);\n"
        "        float32 acc = 0.0f;\n"
        "        try {\n"
        "            opt.step(wrongCount);\n"
        "        } catch (OptimizerException e) {\n"
        "            acc = acc + 1.0f;\n"
        "        }\n"
        "        int64[] bad = {3};\n"
        "        Tensor<float32>[] wrongShape = heap Tensor<float32>[1];\n"
        "        wrongShape[0] = Tensor.full<float32>(bad, 1.0f);\n"
        "        try {\n"
        "            opt.step(wrongShape);\n"
        "        } catch (OptimizerException e) {\n"
        "            acc = acc + 10.0f;\n"
        "        }\n"
        "        return acc + net.w.get().get1(0) * 100.0f;");
    // both throws + param untouched (1.0) -> 11 + 100
    EXPECT_NEAR(r, 111.0f, 1e-4f);
}

// 5.1.1 + 5.1.5 — the protocol is open (a custom rule implements it) and
// driver-agnostic (this hand-built grads bag came from no autograd at all).
TEST(OptimizerTests, customOptimizerImplementsProtocol) {
    std::string custom =
        "public class HalfStep implements Optimizer {\n"
        "    Parameter[] params;\n"
        "    float32 lr;\n"
        "    public HalfStep(#Parameter[] params, float32 lr) {\n"
        "        this.params #= params;\n"
        "        this.lr = lr;\n"
        "    }\n"
        "    public float32 getLr() { return this.lr; }\n"
        "    public void setLr(float32 v) { this.lr = v; }\n"
        "    public void step(Tensor<float32>[] grads) {\n"
        "        int32 i = 0;\n"
        "        while (i < (int32) this.params.count()) {\n"
        "            Tensor<float32> p = this.params[i].get();\n"
        "            p.set1(0, p.get1(0) - 0.5f * this.lr * grads[i].get1(0));\n"
        "            p.set1(1, p.get1(1) - 0.5f * this.lr * grads[i].get1(1));\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    float r = runOpt(std::string(
        "Net net = heap Net(1.0f);\n"
        "        Optimizer opt = heap HalfStep(net.parameters(), 0.1f);\n"
        "        ") + kGrads +
        "        opt.step(grads);\n"
        "        return net.w.get().get1(0) + opt.getLr();",
        custom);
    // 1 - 0.05 + 0.1 = 1.05
    EXPECT_NEAR(r, 1.05f, 1e-4f);
}
