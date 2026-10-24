// Vulkan's eager submit path reuses the scalar-argument arena, so a long run of launches
// with scalar arguments keeps launching (cajeta-llm 15.2.12.1).

#include <gtest/gtest.h>

#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdlib>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class A {\n"
    "    @Kernel\n"
    "    public static void bump(KernelBuffer<int32> out, int32 a, int32 b, int32 c, int32 d) {\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + a + b + c + d; }\n"
    "    }\n"
    "    /** The failures counted across 20000 launches of four scalars each, times\n"
    "     *  1000000, plus how far the sum is from 80000; +1e9 when a launch throws. */\n"
    "    public static int64 run() {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        out.allocate();\n"
    "        int32[] zero #= heap int32[1];\n"
    "        out.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Device.launchFailures();\n"
    "        int64 thrown = 0L;\n"
    "        int32 i = 0;\n"
    "        try {\n"
    "            while (i < 20000) {\n"
    "                bump.launch(s, grid: [1], block: [1])(out, 1, 1, 1, 1);\n"
    "                i = i + 1;\n"
    "            }\n"
    "        } catch (XpuLaunchException e) {\n"
    "            thrown = 1000000000L;\n"
    "        }\n"
    "        s.sync();\n"
    "        int32[] got #= heap int32[1];\n"
    "        out.download(got);\n"
    "        out.free();\n"
    "        int64 off = (int64) got[0] - 80000L;\n"
    "        if (off < 0L) { off = -off; }\n"
    "        return thrown + (Device.launchFailures() - before) * 1000000L + off;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuVulkanScalarArena, eagerLaunchesKeepLaunchingPastTheArena) {
    CAJETA_SKIP_IF_NO_VULKAN();
    setenv("CAJETA_XPU_VK_SUBMIT", "eager", 1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.A", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    testing::internal::CaptureStderr();
    const int64_t r = run();
    const std::string err = testing::internal::GetCapturedStderr();
    unsetenv("CAJETA_XPU_VK_SUBMIT");
    EXPECT_EQ(r, 0) << "failures x 1e6 + |sum - 80000|; stderr: " << err.substr(0, 400);
}
