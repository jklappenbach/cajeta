// cajeta-profiler 3.2.d — the CPU kernel-pool workers are host threads, and
// §2.1 says every host thread is sampled.
//
// They were the last gap: carrier, timer and reactor threads each register via
// a wrapper around their loop body, but the pool workers in
// cajeta_xpu_dispatch.c did not. A kernel fanned across cores therefore ran on
// threads the sampler could not see — and the resulting profile is not empty,
// it is WRONG: the work happened, it just appears nowhere, so a kernel that
// dominates a run reads as a gap.
//
// The pool is lazily created on the first launch that clears the parallel
// threshold, so the observation is a before/after around exactly that launch.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

// 8192-element gather — clears the 256-item parallel threshold and fans across
// cores. Lifted from XpuCpuPoolTeardownTests, which relies on the same
// property and would notice if it stopped holding.
const char* kPoolProgram =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        Tensor<float32> d #= Tensor.linspace<float32>(0.0f, 8191.0f, 8192);\n"
    "        int64[] s = heap int64[1]; s[0] = 8192;\n"
    "        Tensor<int64> ix #= Tensor.zeros<int64>(s);\n"
    "        int64 i = 0;\n"
    "        while (i < 8192) {\n"
    "            ix.set1(i, 8191 - i);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        d.gpu();\n"
    "        ix.gpu();\n"
    "        Tensor<float32> g #= Ewise.takeF32(d, ix);\n"
    "        g.cpu();\n"
    "        if (g.get1(0) != 8191.0f) { return -1; }\n"
    "        if (g.get1(8191) != 0.0f) { return -2; }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(ProfilerKernelPoolRegistry, poolWorkersRegisterWithTheThreadRegistry) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};

    auto jit = CajetaJit::compile(kPoolProgram, "test.D", o);
    ASSERT_NE(jit, nullptr);

    // Each JIT module carries its OWN runtime statics — the registry and the
    // pool both live in this module's memory — so both symbols must come from
    // the module that runs the kernel.
    auto count = reinterpret_cast<int (*)(void)>(
        jit->lookupRawSymbol("__cajeta_prof_thread_count"));
    auto poolThreads = reinterpret_cast<int32_t (*)(void)>(
        jit->lookupRawSymbol("__cajeta_xpu_cpu_pool_threads"));
    ASSERT_NE(count, nullptr) << "__cajeta_prof_thread_count unresolved";
    ASSERT_NE(poolThreads, nullptr) << "__cajeta_xpu_cpu_pool_threads unresolved";

    const int before = count();
    ASSERT_EQ(poolThreads(), 0) << "the pool is supposed to be lazy";

    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    ASSERT_EQ(run(), 1);

    const int workers = poolThreads();
    const int after = count();

    ASSERT_GT(workers, 0)
        << "the launch forked no pool workers — the probe is vacuous "
           "(threshold or worker-count logic changed?)";
    EXPECT_GE(after - before, workers)
        << "kernel-pool workers are missing from the thread registry: "
        << workers << " forked, registry grew by " << (after - before)
        << " (spec 2.1)";
}
