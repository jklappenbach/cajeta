//
// Cajeta XPU — proof-of-support compute probes, ON A REAL DEVICE (Vulkan/RADV).
//
// The on-device twin of XpuComputeProbeTests: the SAME sources (ComputeProbeSources.h)
// the CPU oracle bit-checks, here compiled with xpuBackends={Spirv} and run end to
// end on a real Vulkan compute device — buffers bind as storage buffers, scalars
// wrap in transient SSBOs, atomics lower to OpAtomicFAdd, and each run() self-checks
// and returns 777. This is the executable payoff of variance discipline: one Cajeta
// program proves the numerics / PyTorch / SPELA compute shapes work on the substrate
// AND on the metal. Skips cleanly when no Vulkan compute device is present.
//
// (AMD HIP in-process device JIT is currently blocked by a libamd_comgr symbol
// collision — see the project memory — so the on-device rung is Vulkan here.)
//

#include "gtest/gtest.h"

#include "ComputeProbeSources.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

CajetaJit::Options vulkanOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    return o;
}

int runProbeOnDevice(const char* source, const char* fqClass) {
    auto jit = CajetaJit::compile(source, fqClass, vulkanOptions());
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -1;
    return fn();
}

} // namespace

// Target 1: numerics — broadcast element-wise + atomic-sum reduction, on device.
TEST(XpuComputeProbeDeviceTests, numericsBroadcastReductionOnVulkan) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    int r = runProbeOnDevice(cajeta_test_probes::kNumericsBroadcastReduce(),
                             "test.NumProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: element-wise mismatch; 2: reduction sum off)";
}

// Target 2: PyTorch — matmul + atomic scatter-add (colliding indices), on device.
TEST(XpuComputeProbeDeviceTests, torchMatmulAtomicScatterOnVulkan) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    int r = runProbeOnDevice(cajeta_test_probes::kTorchMatmulScatter(),
                             "test.TorchProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: matmul mismatch; 200+bin: scatter-add mismatch)";
}

// Target 3: Toffee/SPELA — fused forward + local-loss + weight update, on device.
TEST(XpuComputeProbeDeviceTests, spelaFusedLayerOnVulkan) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    int r = runProbeOnDevice(cajeta_test_probes::kSpelaFusedLayer(),
                             "test.SpelaProbe");
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+j: loss mismatch; 200+idx: updated-weight mismatch)";
}
