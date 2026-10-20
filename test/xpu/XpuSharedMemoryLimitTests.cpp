// A kernel whose shared memory exceeds the device's limit reads as unavailable and its
// launch is refused by name before the driver sees it (xpu-kernel-adaptor 1.8.6).

#include <gtest/gtest.h>

#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.lang.Optional;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelManifest;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class S {\n"
    "    @Kernel\n"
    "    public static void big(KernelBuffer<float32> out, uint32 n) {\n"
    "        Shared<float32> buf = shared float32[24576];\n"
    "        uint32 i = KernelThread.x();\n"
    "        buf[i] = (float32) i;\n"
    "        buf[i + 24512] = 1.0f;\n"
    "        Barrier.workgroup();\n"
    "        if (i < n) { out[i] = buf[i] + buf[24575 - i]; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void small(KernelBuffer<float32> out, uint32 n) {\n"
    "        Shared<float32> buf = shared float32[256];\n"
    "        uint32 i = KernelThread.x();\n"
    "        buf[i] = (float32) i;\n"
    "        Barrier.workgroup();\n"
    "        if (i < n) { out[i] = buf[63 - i] + 1.0f; }\n"
    "    }\n"
    "    /** 0 when the 96 KB kernel's manifest records it (else +1000), it reads as\n"
    "     *  unavailable (+1), its launch throws (+4) and counts one failure (+8), and\n"
    "     *  the 1 KB kernel is available (+2) and writes right (+16); -1 when the\n"
    "     *  device allows 96 KB a block or does not say. */\n"
    "    public static int64 probe() {\n"
    "        int64 limit = Device.sharedBytesPerBlock();\n"
    "        if (Device.sharedBytesPerBlockOptIn() > limit) { limit = Device.sharedBytesPerBlockOptIn(); }\n"
    "        if (limit <= 0 || limit >= 98304) { return -1; }\n"
    "        int64 r = 0;\n"
    "        KernelManifest m = big.manifest();\n"
    "        if (m == null) { r = r + 1000; }\n"
    "        else {\n"
    "            Optional<int32> lds = m.ldsStaticBytes();\n"
    "            if (!lds.isPresent() || lds.get() < 98304) { r = r + 1000; }\n"
    "        }\n"
    "        if (Device.kernelAvailable(\"big\")) { r = r + 1; }\n"
    "        if (!Device.kernelAvailable(\"small\")) { r = r + 2; }\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<float32> ob = heap KernelBuffer<float32>(0, n);\n"
    "        ob.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Device.launchFailures();\n"
    "        boolean thrown = false;\n"
    "        try {\n"
    "            big.launch(s, grid: [1], block: [64])(ob, n);\n"
    "        } catch (XpuLaunchException e) {\n"
    "            thrown = true;\n"
    "        }\n"
    "        s.sync();\n"
    "        if (!thrown) { r = r + 4; }\n"
    "        if (Device.launchFailures() - before != 1) { r = r + 8; }\n"
    "        small.launch(s, grid: [1], block: [64])(ob, n);\n"
    "        s.sync();\n"
    "        float32[] got #= heap float32[(int32) n];\n"
    "        ob.download(got);\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) n) {\n"
    "            if (got[i] != (float32) (64 - i)) { r = r + 16; i = (int32) n; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        ob.free();\n"
    "        return r;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuSharedMemoryLimit, aVulkanKernelOverTheSharedLimitIsUnavailableAndRefused) {
    CAJETA_SKIP_IF_NO_VULKAN();
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.S", o);
    ASSERT_NE(jit, nullptr);
    auto probe = jit->lookup<int64_t (*)()>("probe");
    ASSERT_NE(probe, nullptr);
    const int64_t r = probe();
    if (r == -1) GTEST_SKIP() << "the device allows 96 KB of shared memory a block or does not say";
    EXPECT_EQ(r, 0) << "+1000 manifest, +1 available, +2 small unavailable, +4 no throw, "
                       "+8 failure count, +16 small wrong";
}
