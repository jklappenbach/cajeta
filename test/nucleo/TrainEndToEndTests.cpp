//
// nucleo-nn-optim Unit 8 — the headline bar (plan 8.1.x; spec §10): a
// 2-layer relu MLP on a seeded toy regression, trained by EACH of
// SGD/Adam/AdamW via the GradAll functional-step bridge — the loss strictly
// decreases. Scheduler-in-the-loop and the freeze story ride along.
//
// The functional step spells the parameters as the LEADING args in
// parameters() order ([fc1.w, fc1.b, fc2.w, fc2.b]) and the data after;
// grads[i] belongs to ps[i], so r.grads feeds opt.step DIRECTLY.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// The shared harness: MLP(2-4-1), toy set y = x0 + 2*x1 (8 points), and a
// train(optName) that runs N steps and returns lossFirst * 1000 + lossLast.
std::string trainSrc(const std::string& runBody) {
    return std::string(
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.nn.Module;\n"
        "import cajeta.nucleo.nn.Parameter;\n"
        "import cajeta.nucleo.nn.Linear;\n"
        "import cajeta.nucleo.nn.Losses;\n"
        "import cajeta.nucleo.optim.Optimizer;\n"
        "import cajeta.nucleo.optim.SGD;\n"
        "import cajeta.nucleo.optim.Adam;\n"
        "import cajeta.nucleo.optim.AdamW;\n"
        "import cajeta.nucleo.optim.LrSchedule;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public class MLP extends Module {\n"
        "    Linear fc1;\n"
        "    Linear fc2;\n"
        "    public MLP() {\n"
        "        this.fc1 = heap Linear(2, 4, 11);\n"
        "        this.fc2 = heap Linear(4, 1, 22);\n"
        "    }\n"
        "}\n"
        "public final class T {\n"
        "    static #Tensor<float32> makeX() {\n"
        "        int64[] xs = {8, 2};\n"
        "        Tensor<float32> x = Tensor.zeros<float32>(xs);\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            x.set2(i, 0, (float32) i * 0.25f);\n"
        "            x.set2(i, 1, 1.0f - (float32) i * 0.125f);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return #x;\n"
        "    }\n"
        "    static #Tensor<float32> makeT(Tensor<float32> x) {\n"
        "        int64[] ts = {8, 1};\n"
        "        Tensor<float32> t = Tensor.zeros<float32>(ts);\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            t.set2(i, 0, x.get2(i, 0) + 2.0f * x.get2(i, 1));\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return #t;\n"
        "    }\n"
        "    // lossFirst*1000 + lossLast after `steps` updates through `opt`.\n"
        "    static float32 trainWith(Optimizer opt, MLP net, int32 steps,\n"
        "                             LrSchedule sched, boolean useSched) {\n"
        "        Tensor<float32> x = T.makeX();\n"
        "        Tensor<float32> t = T.makeT(x);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>,\n"
        "         Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> step =\n"
        "            GradAll<4>((Tensor<float32> w1, Tensor<float32> b1,\n"
        "                        Tensor<float32> w2, Tensor<float32> b2,\n"
        "                        Tensor<float32> xi, Tensor<float32> ti) ->\n"
        "                Losses.mse(\n"
        "                    Tensor.add<float32>(Tensor.matmul<float32>(\n"
        "                        Tensor.relu<float32>(Tensor.add<float32>(\n"
        "                            Tensor.matmul<float32>(xi, w1), b1)),\n"
        "                        w2), b2),\n"
        "                    ti));\n"
        "        Parameter[] ps = net.parameters();\n"
        "        float32 first = -1.0f;\n"
        "        float32 last = -1.0f;\n"
        "        int32 k = 0;\n"
        "        while (k < steps) {\n"
        "            if (useSched) { sched.step(); }\n"
        "            GradResult<float32, Tensor<float32>[]> r = step(\n"
        "                ps[0].get(), ps[1].get(), ps[2].get(), ps[3].get(),\n"
        "                x, t);\n"
        "            if (k == 0) { first = r.value; }\n"
        "            last = r.value;\n"
        "            opt.step(r.grads);\n"
        "            k = k + 1;\n"
        "        }\n"
        "        if (last < first * 0.5f) { return 777.0f; }\n"
        "        return first * 1000.0f + last;\n"
        "    }\n"
        "    public static float32 run() {\n"
        "        ") + runBody + "\n"
        "    }\n"
        "}\n";
}

float runTrain(const std::string& body) {
    auto jit = CajetaJit::compile(trainSrc(body), "test.T");
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1e9f;
    auto fn = jit->lookup<float (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1e9f;
    return fn();
}
} // namespace

// 8.1.1a — SGD: 40 steps at lr 0.05 halves the loss (and then some).
TEST(TrainEndToEndTests, sgdTrainsTheMlp) {
    float r = runTrain(
        "MLP net = heap MLP();\n"
        "        Optimizer opt = heap SGD(net.parameters(), 0.05f, 0.0f);\n"
        "        return T.trainWith(opt, net, 40, null, false);");
    EXPECT_NEAR(r, 777.0f, 1e-3f);
}

// 8.1.1b — Adam.
TEST(TrainEndToEndTests, adamTrainsTheMlp) {
    float r = runTrain(
        "MLP net = heap MLP();\n"
        "        Optimizer opt = heap Adam(net.parameters(), 0.05f);\n"
        "        return T.trainWith(opt, net, 40, null, false);");
    EXPECT_NEAR(r, 777.0f, 1e-3f);
}

// 8.1.1c — AdamW.
TEST(TrainEndToEndTests, adamWTrainsTheMlp) {
    float r = runTrain(
        "MLP net = heap MLP();\n"
        "        Optimizer opt = heap AdamW(net.parameters(), 0.05f, 0.001f);\n"
        "        return T.trainWith(opt, net, 40, null, false);");
    EXPECT_NEAR(r, 777.0f, 1e-3f);
}

// 8.1.2 — a cosine schedule drives opt.lr per step and training converges;
// the LR actually observed changes over the run.
TEST(TrainEndToEndTests, schedulerInTheLoop) {
    float r = runTrain(
        "MLP net = heap MLP();\n"
        "        Optimizer opt = heap SGD(net.parameters(), 9.9f, 0.0f);\n"
        "        LrSchedule sched = LrSchedule.cosine(opt, 0.08f, 200);\n"
        "        float32 res = T.trainWith(opt, net, 40, sched, true);\n"
        "        // after 40 sched.steps the lr is below base and above zero\n"
        "        float32 lr = opt.getLr();\n"
        "        if (res == 777.0f && lr > 0.0f && lr < 0.08f) { return 777.0f; }\n"
        "        return res + lr;");
    EXPECT_NEAR(r, 777.0f, 1e-3f);
}

// 8.1.3 — freeze: the optimizer holds ONLY fc2's params; fc1 stays
// bit-identical through training while the loss still moves.
TEST(TrainEndToEndTests, freezeBackboneTrainsHeadOnly) {
    float r = runTrain(
        "MLP net = heap MLP();\n"
        "        Parameter[] all = net.parameters();\n"
        "        float32 w1before = all[0].get().get2(0, 0);\n"
        "        Optimizer opt = heap SGD(net.fc2.parameters(), 0.05f, 0.0f);\n"
        "        Tensor<float32> x = T.makeX();\n"
        "        Tensor<float32> t = T.makeT(x);\n"
        "        (Tensor<float32>, Tensor<float32>, Tensor<float32>,\n"
        "         Tensor<float32>, Tensor<float32>, Tensor<float32>)\n"
        "                -> GradResult<float32, Tensor<float32>[]> step =\n"
        "            GradAll<4>((Tensor<float32> w1, Tensor<float32> b1,\n"
        "                        Tensor<float32> w2, Tensor<float32> b2,\n"
        "                        Tensor<float32> xi, Tensor<float32> ti) ->\n"
        "                Losses.mse(\n"
        "                    Tensor.add<float32>(Tensor.matmul<float32>(\n"
        "                        Tensor.relu<float32>(Tensor.add<float32>(\n"
        "                            Tensor.matmul<float32>(xi, w1), b1)),\n"
        "                        w2), b2),\n"
        "                    ti));\n"
        "        int32 k = 0;\n"
        "        float32 first = -1.0f;\n"
        "        float32 last = -1.0f;\n"
        "        while (k < 30) {\n"
        "            GradResult<float32, Tensor<float32>[]> r = step(\n"
        "                all[0].get(), all[1].get(), all[2].get(),\n"
        "                all[3].get(), x, t);\n"
        "            if (k == 0) { first = r.value; }\n"
        "            last = r.value;\n"
        "            Tensor<float32>[] headGrads = heap Tensor<float32>[2];\n"
        "            headGrads[0] = r.grads[2];\n"
        "            headGrads[1] = r.grads[3];\n"
        "            opt.step(headGrads);\n"
        "            k = k + 1;\n"
        "        }\n"
        "        float32 ok = 0.0f;\n"
        "        if (all[0].get().get2(0, 0) == w1before) { ok = ok + 1.0f; }\n"
        "        if (last < first) { ok = ok + 10.0f; }\n"
        "        return ok;");
    EXPECT_NEAR(r, 11.0f, 1e-3f);
}
