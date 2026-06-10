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

#include <map>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// run() returns 1 iff the active device advertises native ray query. The trivial
// kernel is what `Device.supports` is *for* — choosing which kernel path to launch
// — and its presence bundles the backend (so the runtime selects the device).
const char* kSupportsDriver =
    "package test;\n"
    "import cajeta.xpu.core.Device;\n"
    "import cajeta.xpu.core.Capability;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
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

int runOn(cajeta::xpu::Backend backend) {
    std::map<std::string, std::string> sources = {{"test.DevCap", kSupportsDriver}};
    CajetaJit::Options o;
    o.xpuBackends = {backend};
    auto jit = CajetaJit::compile(sources, "test.DevCap", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -1;
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
