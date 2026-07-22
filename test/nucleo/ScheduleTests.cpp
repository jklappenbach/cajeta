//
// nucleo-nn-optim Unit 6 — LR schedulers (plan 6.1.x; spec §6; N7):
// pure lr(step) functions + the thin mutating wrapper with explicit count.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runSched(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.nucleo.nn.Module;\n"
        "import cajeta.nucleo.nn.Parameter;\n"
        "import cajeta.nucleo.optim.Optimizer;\n"
        "import cajeta.nucleo.optim.SGD;\n"
        "import cajeta.nucleo.optim.Schedules;\n"
        "import cajeta.nucleo.optim.LrSchedule;\n"
        "public class Net extends Module {\n"
        "    Parameter w;\n"
        "    public Net() {\n"
        "        int64[] s = {1};\n"
        "        this.w = heap Parameter(Tensor.full<float32>(s, 1.0f));\n"
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
} // namespace

// 6.1.1 — the pure functions match reference values at sampled steps.
TEST(ScheduleTests, pureFunctionsMatchReference) {
    float r = runSched(
        "float32 a = Schedules.stepLr(1.0f, 10, 0.5f, 0);\n"     // 1.0
        "        float32 b = Schedules.stepLr(1.0f, 10, 0.5f, 25);\n"    // 0.25
        "        float32 c = Schedules.exponentialLr(1.0f, 0.9f, 2);\n"  // 0.81
        "        float32 d = Schedules.cosineLr(1.0f, 100, 0);\n"        // 1.0
        "        float32 e = Schedules.cosineLr(1.0f, 100, 50);\n"       // 0.5
        "        float32 f = Schedules.cosineLr(1.0f, 100, 100);\n"      // 0.0
        "        return a + b * 10.0f + c * 100.0f + d * 1000.0f\n"
        "            + e * 10000.0f + f;");
    // 1 + 2.5 + 81 + 1000 + 5000 + 0 = 6084.5
    EXPECT_NEAR(r, 6084.5f, 1e-1f);
}

// 6.1.2 — the wrapper advances ITS OWN count and writes the optimizer's lr;
// getLr reads the value in effect.
TEST(ScheduleTests, wrapperDrivesOptimizerLr) {
    float r = runSched(
        "Net net = heap Net();\n"
        "        Optimizer opt = heap SGD(net.parameters(), 9.0f, 0.0f);\n"
        "        LrSchedule sched = LrSchedule.exponential(opt, 1.0f, 0.5f);\n"
        "        sched.step();\n"                       // count 0 -> lr 1.0
        "        float32 l1 = opt.getLr();\n"
        "        sched.step();\n"                       // count 1 -> lr 0.5
        "        sched.step();\n"                       // count 2 -> lr 0.25
        "        return l1 * 100.0f + sched.getLr() * 10.0f;");
    // 100 + 2.5
    EXPECT_NEAR(r, 102.5f, 1e-3f);
}

// 6.1.3 — warmup-then-cosine composes: ramps up over warmup, then anneals.
TEST(ScheduleTests, warmupCosineComposes) {
    float r = runSched(
        "float32 w0 = Schedules.warmupCosineLr(1.0f, 4, 104, 0);\n"   // 0.25
        "        float32 w3 = Schedules.warmupCosineLr(1.0f, 4, 104, 3);\n"   // 1.0
        "        float32 mid = Schedules.warmupCosineLr(1.0f, 4, 104, 54);\n" // cos mid = 0.5
        "        float32 end = Schedules.warmupCosineLr(1.0f, 4, 104, 104);\n"// 0.0
        "        return w0 + w3 * 10.0f + mid * 100.0f + end;");
    // 0.25 + 10 + 50 + 0 = 60.25
    EXPECT_NEAR(r, 60.25f, 1e-2f);
}
