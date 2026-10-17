// A block above the device limit is refused on the host, counted once and thrown,
// and never reaches the driver (xpu-kernel-adaptor 1.6.7).

#include <gtest/gtest.h>

#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class L {\n"
    "    @Kernel\n"
    "    public static void fill(KernelBuffer<int32> out, int32 v, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[(int64) i] = v;\n"
    "        }\n"
    "    }\n"
    "    /** Failures one launch at `block` adds, +10 if it threw, +100 if a legal one\n"
    "     *  wrote wrong; -1 when the device limit is unknown or allows 2048. */\n"
    "    public static int64 failuresAt(int32 block) {\n"
    "        int32 limit = Device.maxThreadsPerBlock();\n"
    "        if (limit <= 0 || limit >= 2048) { return -1; }\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<int32> ob = heap KernelBuffer<int32>(0, n);\n"
    "        ob.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Device.launchFailures();\n"
    "        int64 thrown = 0;\n"
    "        try {\n"
    "            if (block == 2048) {\n"
    "                fill.launch(s, grid: [1], block: [2048])(ob, 7, n);\n"
    "            } else {\n"
    "                fill.launch(s, grid: [1], block: [64])(ob, 7, n);\n"
    "            }\n"
    "        } catch (XpuLaunchException e) {\n"
    "            thrown = 10;\n"
    "        }\n"
    "        s.sync();\n"
    "        int64 after = Device.launchFailures();\n"
    "        int64 wrong = 0;\n"
    "        if (block != 2048) {\n"
    "            int32[] got #= heap int32[(int32) n];\n"
    "            ob.download(got);\n"
    "            int32 i = 0;\n"
    "            while (i < (int32) n) {\n"
    "                if (got[i] != 7) { wrong = 100; }\n"
    "                i = i + 1;\n"
    "            }\n"
    "        }\n"
    "        ob.free();\n"
    "        return after - before + thrown + wrong;\n"
    "    }\n"
    "}\n";

using Failures = int64_t (*)(int32_t);

Failures compileFor(std::unique_ptr<CajetaJit>& jit, cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    jit = CajetaJit::compile(kSource, "test.L", o);
    if (!jit) return nullptr;
    return jit->lookup<Failures>("failuresAt");
}

// The oversized block counts one failure and throws; the legal one after it counts none.
void expectCounted(cajeta::xpu::Backend b) {
    std::unique_ptr<CajetaJit> jit;
    Failures at = compileFor(jit, b);
    ASSERT_NE(at, nullptr);
    const int64_t refused = at(2048);
    ASSERT_NE(refused, -1) << "the device limit is unknown or allows 2048 work-items";
    EXPECT_EQ(refused, 11) << "an oversized launch moved launchFailures() by "
                           << refused % 10 << (refused >= 10 ? " and threw" : " and did not throw");
    EXPECT_EQ(at(64), 0) << "a legal launch counted a failure (1), threw (10) or "
                            "did not run (100) after the refused one";
}

} // namespace

TEST(XpuLaunchFailureCount, aCudaBlockAboveTheLimitIsRefusedBeforeSubmit) {
    CAJETA_SKIP_IF_NO_CUDA();
    expectCounted(cajeta::xpu::Backend::Nvptx);
}

TEST(XpuLaunchFailureCount, aHipBlockAboveTheLimitIsRefusedBeforeSubmit) {
    CAJETA_SKIP_IF_NO_HIP();
    expectCounted(cajeta::xpu::Backend::Amdgpu);
}

TEST(XpuLaunchFailureCount, aVulkanBlockAboveTheLimitIsRefusedBeforeSubmit) {
    CAJETA_SKIP_IF_NO_VULKAN();
    expectCounted(cajeta::xpu::Backend::Spirv);
}
