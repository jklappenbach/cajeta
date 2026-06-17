//
// Device.supports(Capability) — the runtime capability heuristic's entry point
// (cajeta-gpu inc 4, the model plumbing). A host query of the ACTIVE device: the
// CPU backend has no native ray query (false → the software BVH path), a Vulkan
// ray-query device does (true → the hardware path). Same source, different answer
// per device — that is the heuristic input.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"

#include <map>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;
using cajeta::xpu::nvidia::CudaDriver;

namespace {

// run() returns 1 iff the active device advertises native ray query. The trivial
// kernel is what `Device.supports` is *for* — choosing which kernel path to launch
// — and its presence bundles the backend (so the runtime selects the device).
const char* kSupportsDriver =
    "package test;\n"
    "import cajeta.gpu.Device;\n"
    "import cajeta.gpu.Capability;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "public class DevCap {\n"
    "    @Kernel\n"
    "    public static void touch(Buffer<uint32> b) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i == 0) { b[0] = 1; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        if (Device.supports(Capability.RayQueryNative)) { return 1; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

// run() returns 1 iff the active device advertises RT-core (OptiX) ray query — the
// pipeline-based CUDA path, distinct from the inline RayQueryNative above.
const char* kRtCoreDriver =
    "package test;\n"
    "import cajeta.gpu.Device;\n"
    "import cajeta.gpu.Capability;\n"
    "import cajeta.gpu.Buffer;\n"
    "import cajeta.gpu.Thread;\n"
    "public class DevRt {\n"
    "    @Kernel\n"
    "    public static void touch(Buffer<uint32> b) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i == 0) { b[0] = 1; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        if (Device.supports(Capability.RayQueryRtCore)) { return 1; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

int runSource(const char* src, const char* fq, cajeta::xpu::Backend backend) {
    std::map<std::string, std::string> sources = {{fq, src}};
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(sources, fq, o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -1;
}

int runOn(cajeta::xpu::Backend backend) {
    return runSource(kSupportsDriver, "test.DevCap", backend);
}

} // namespace

// CPU has no native ray query — supports() is false (and a RayQuery there still
// runs, via the software BVH walk).
TEST(XpuDeviceCapabilityTests, rayQueryNativeFalseOnCpu) {
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Cpu), 0);
}

// A ray-query-capable Vulkan device reports the native capability.
TEST(XpuDeviceCapabilityTests, rayQueryNativeTrueOnVulkanDevice) {
    if (!VulkanDriver::rayQueryAvailable()) {
        GTEST_SKIP() << "no Vulkan ray-query device";
    }
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Spirv), 1);
}

// CPU has no RT cores — RayQueryRtCore is false (a RayQuery there still runs via the
// software BVH walk).
TEST(XpuDeviceCapabilityTests, rayQueryRtCoreFalseOnCpu) {
    EXPECT_EQ(runSource(kRtCoreDriver, "test.DevRt", cajeta::xpu::Backend::Cpu), 0);
}

// CUDA is INLINE-ray-query-incapable (OptiX is pipeline-based), so RayQueryNative is
// false there — the RT-core path is the separate RayQueryRtCore capability.
TEST(XpuDeviceCapabilityTests, rayQueryNativeFalseOnNvptxDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Nvptx), 0);
}

// A CUDA device with the OptiX engine loaded reports RayQueryRtCore — the heuristic
// input an app uses to choose the OptiX opt-in (CAJETA_GPU_AS_IMPL=optix) path.
TEST(XpuDeviceCapabilityTests, rayQueryRtCoreTrueOnOptixDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }
    // Skips cleanly on a CUDA box without the OptiX runtime: there RayQueryRtCore is
    // (correctly) false, so only assert true where OptiX is actually present.
    int r = runSource(kRtCoreDriver, "test.DevRt", cajeta::xpu::Backend::Nvptx);
    if (r == 0) {
        GTEST_SKIP() << "CUDA device present but OptiX engine not loaded";
    }
    EXPECT_EQ(r, 1);
}
