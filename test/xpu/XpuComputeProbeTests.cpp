//
// Cajeta XPU — proof-of-support compute probes (Stage-12 definition-of-done).
//
// These are *thin* probes — one per acceptance target — that prove the compute
// substrate can support building each downstream library, not the library
// itself. Each is a complete Cajeta program (allocate / upload / kernel.launch /
// sync / download / verify) run end to end through the frozen launch + kernel-arg
// FFI (the `__cajeta_xpu_launch` → `_v2` path, `__cajeta_xpu_register_module` +
// `__cajeta_xpu_register_kernel_params`); see docs/gpu/xpu/CajetaXPU-FFI.md. Each
// `run()` self-checks and returns 777 on success (else a fail code), so a green
// test is an executable proof the pattern works on the substrate.
//
// CPU is the bit-checked reference oracle. The *same* sources (ComputeProbeSources.h)
// ride the on-device Vulkan gate in XpuComputeProbeDeviceTests.cpp — "on-device
// gates ride the same sources". (AMD in-process device JIT is currently blocked
// by a libamd_comgr symbol collision — see the project memory — so the on-device
// rung here is Vulkan.)
//
// Targets covered:
//   1. Numerics (NumPy/SciPy) — broadcast-shaped element-wise + atomic reduction.
//   2. PyTorch — matmul + atomic grad-scatter (scatter-add with collisions).
//   3. Caramelo/SPELA — fused forward + local-loss + weight update in one pass.
//

#include "gtest/gtest.h"

#include "ComputeProbeSources.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

int runProbe(const char* source, const char* fqClass) {
    auto jit = CajetaJit::compile(source, fqClass, cpuOptions());
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1;
    return fn();
}

} // namespace

// Target 1: numerics — broadcast-shaped element-wise (per-row scale/bias) +
// atomic-sum reduction. The two shapes every NumPy/SciPy kernel leans on.
TEST(XpuComputeProbeTests, numericsBroadcastReductionOnCpu) {
    int r = runProbe(cajeta_test_probes::kNumericsBroadcastReduce(), "test.NumProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: element-wise mismatch; 2: reduction sum off)";
}

// Target 2: PyTorch — matmul (accumulate over K) + atomic scatter-add with
// colliding indices (the embedding-grad pattern that *requires* atomics).
TEST(XpuComputeProbeTests, torchMatmulAtomicScatterOnCpu) {
    int r = runProbe(cajeta_test_probes::kTorchMatmulScatter(), "test.TorchProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: matmul mismatch; 200+bin: scatter-add mismatch)";
}

// Target 3: Caramelo/SPELA — fuse a layer's forward + local loss + weight update
// into ONE device pass (one thread per output neuron owns its W row).
TEST(XpuComputeProbeTests, spelaFusedLayerOnCpu) {
    int r = runProbe(cajeta_test_probes::kSpelaFusedLayer(), "test.SpelaProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+j: loss mismatch; 200+idx: updated-weight mismatch)";
}
