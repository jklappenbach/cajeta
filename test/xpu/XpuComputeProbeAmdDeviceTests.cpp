//
// Cajeta XPU — proof-of-support compute probes, ON A REAL AMD DEVICE (gfx1151).
//
// The AMD on-device twin of XpuComputeProbe{,Device}Tests: the SAME sources
// (ComputeProbeSources.h) the CPU oracle bit-checks and Vulkan runs, here
// compiled with xpuBackends={Amdgpu} and run end to end on the real HIP device —
// buffers as device allocations, atomics as native gfx FP atomics, each run()
// self-checks and returns 777.
//
// These supersede the emit-only XpuComputeProbeAmdEmitTests (kept as a no-HW
// gate): in-process AMD device JIT was previously comgr-blocked; the
// --exclude-libs symbol-isolation fix unblocked it (see project memory
// reference_amd_device_jit_comgr_collision). Skips cleanly when no HIP device.
//

#include "gtest/gtest.h"

#include "ComputeProbeSources.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/HipDriver.h"

using cajeta_test::CajetaJit;
using cajeta::xpu::amd::HipDriver;

namespace {

CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

int runProbeOnAmd(const char* source, const char* fqClass) {
    auto jit = CajetaJit::compile(source, fqClass, amdOptions());
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1;
    return fn();
}

} // namespace

// Target 1: numerics — broadcast element-wise + atomic-sum reduction, on gfx1151.
TEST(XpuComputeProbeAmdDeviceTests, numericsBroadcastReductionOnAmd) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    int r = runProbeOnAmd(cajeta_test_probes::kNumericsBroadcastReduce(),
                          "test.NumProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: element-wise mismatch; 2: reduction sum off)";
}

// Target 2: PyTorch — matmul + atomic scatter-add (colliding indices), on gfx1151.
TEST(XpuComputeProbeAmdDeviceTests, torchMatmulAtomicScatterOnAmd) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    int r = runProbeOnAmd(cajeta_test_probes::kTorchMatmulScatter(),
                          "test.TorchProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: matmul mismatch; 200+bin: scatter-add mismatch)";
}

// Target 3: Caramelo/SPELA — fused forward + local-loss + weight update, on gfx1151.
TEST(XpuComputeProbeAmdDeviceTests, spelaFusedLayerOnAmd) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    int r = runProbeOnAmd(cajeta_test_probes::kSpelaFusedLayer(),
                          "test.SpelaProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+j: loss mismatch; 200+idx: updated-weight mismatch)";
}
