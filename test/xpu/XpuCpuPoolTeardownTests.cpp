//
// The math-suite futex SIGABRT, root-caused: each JIT'd test module carries
// its OWN copy of the runtime statics — including the persistent CPU-kernel
// worker pool (g_caj_kpool, cajeta_xpu_dispatch.c). Its workers park between
// launches on a condvar living in MODULE memory. `__cajeta_task_shutdown`
// (the teardown hook CajetaJit's dtor calls through the dying module's own
// symbol) joined carriers, the timer, and the reactor (R9.1/R9.4) — but NOT
// the kernel pool. So the first test that fanned a kernel across cores left
// its workers parked on a futex the LLJIT then freed; once a later test's
// allocations recycled that address, glibc died with "The futex facility
// returned an unexpected error code" (SIGABRT, "heap corruption"-shaped) at
// a load-dependent point — ~55-90 tests into a math sweep, a different test
// each run, every test green in isolation.
//
// The pin: pool workers must JOIN when their module tears down. Thread count
// is read from /proc/self/status — before the probe JIT, during (pool live,
// proving the probe actually forked), and after teardown (must equal before).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <fstream>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int threadCount() {
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Threads:", 0) == 0) {
            return std::atoi(line.c_str() + 8);
        }
    }
#endif
    return -1;
}

// A gather big enough to clear the parallel threshold (256 work-items) and
// fan across cores: 8192 elements -> the launch ensures a multi-worker pool.
// Mirrors NumpyOpsTests.gatherCpuGpuAgree's proven device-routing shape.
const char* kPoolProgram =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        Tensor<float32> d = Tensor.linspace<float32>(0.0f, 8191.0f, 8192);\n"
    "        int64[] s = heap int64[1]; s[0] = 8192;\n"
    "        Tensor<int64> ix = Tensor.zeros<int64>(s);\n"
    "        int64 i = 0;\n"
    "        while (i < 8192) {\n"
    "            ix.set1(i, 8191 - i);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        d.gpu();\n"
    "        ix.gpu();\n"
    "        Tensor<float32> g = Ewise.takeF32(d, ix);\n"
    "        g.cpu();\n"
    "        if (g.get1(0) != 8191.0f) { return -1; }\n"
    "        if (g.get1(8191) != 0.0f) { return -2; }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuCpuPoolTeardown, poolWorkersJoinWithTheirModule) {
#ifndef __linux__
    GTEST_SKIP() << "thread-count probe reads /proc";
#endif
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};

    // Warmup: absorb any one-time host-side threads (JIT infrastructure)
    // so the before/after baseline isolates the POOL's threads.
    {
        auto warm = CajetaJit::compile(kPoolProgram, "test.D", o);
        ASSERT_EQ(warm->lookup<int32_t (*)()>("run")(), 1);
    }

    int before = threadCount();
    ASSERT_GT(before, 0);

    int during = -1;
    {
        auto jit = CajetaJit::compile(kPoolProgram, "test.D", o);
        ASSERT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
        during = threadCount();
    }
    int after = threadCount();

    EXPECT_GT(during, before)
        << "the kernel launch started no pool workers — the probe is vacuous "
           "(threshold or worker-count logic changed?)";
    EXPECT_EQ(after, before)
        << "CPU kernel-pool workers survived their module's teardown — parked "
           "on a condvar in freed JIT memory (the futex-fatal bug)";
}
