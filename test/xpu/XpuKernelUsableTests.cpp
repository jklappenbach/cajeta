//
// cajeta-ml-gpu Unit 6 (6.1.1; spec §7.1): the availability registry in cajeta.xpu.
// Device.kernelUsable(name) answers per backend with the reason: not a target, no
// device code, held by a recorded defect, or pinned off by a test override.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelCap;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class U {\n"
    "    @Kernel\n"
    "    public static void realK(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = 1.0f; }\n"
    "    }\n"
    "    @Kernel(targets = {\"gpu\"})\n"
    "    public static void gpuOnlyK(KernelBuffer<float32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = 2.0f; }\n"
    "    }\n"
    "    static boolean same(String a, String b) { return a.equals(b); }\n"
    "    /** 0, or the number of the first check that failed. */\n"
    "    public static int32 run() {\n"
    "        if (!Device.kernelUsable(\"realK\")) { return 1; }\n"
    "        if (!U.same(Device.kernelUnusableReason(\"realK\"), \"\")) { return 2; }\n"
    "        if (Device.kernelUsable(\"gpuOnlyK\")) { return 3; }\n"
    "        if (!U.same(Device.kernelUnusableReason(\"gpuOnlyK\"), \"not a target\")) { return 4; }\n"
    "        if (Device.kernelUsable(\"ghostK\")) { return 5; }\n"
    "        if (!U.same(Device.kernelUnusableReason(\"ghostK\"), \"no usable device code\")) { return 6; }\n"
    "        KernelCap.hold(Device.activeBackend(), \"realK\", \"plan 1.2.3\");\n"
    "        if (Device.kernelUsable(\"realK\")) { return 7; }\n"
    "        if (!U.same(Device.kernelUnusableReason(\"realK\"), \"held: plan 1.2.3\")) { return 8; }\n"
    "        KernelCap.hold(\"nowhere\", \"gpuOnlyK\", \"other backend\");\n"
    "        if (!U.same(Device.kernelUnusableReason(\"gpuOnlyK\"), \"not a target\")) { return 9; }\n"
    "        KernelCap.setOverride(\"realK\", true);\n"
    "        if (!Device.kernelUsable(\"realK\")) { return 10; }\n"
    "        KernelCap.setOverride(\"realK\", false);\n"
    "        if (!U.same(Device.kernelUnusableReason(\"realK\"), \"overridden off\")) { return 11; }\n"
    "        KernelCap.clearOverrides();\n"
    "        if (Device.kernelUsable(\"realK\")) { return 12; }\n"
    "        KernelCap.release(Device.activeBackend(), \"realK\");\n"
    "        if (!Device.kernelUsable(\"realK\")) { return 13; }\n"
    "        KernelCap.setOverride(\"ghostK\", true);\n"
    "        if (!Device.kernelUsable(\"ghostK\")) { return 14; }\n"
    "        KernelCap.clearOverrides();\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

} // namespace

// 6.1.1: registered, off-target, unknown, held, overridden on and off, released.
TEST(XpuKernelUsable, answersWithTheReasonAndHonoursHoldsAndOverrides) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.U", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0);
}
