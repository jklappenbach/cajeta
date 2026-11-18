//
// SpirvSharedHelperParamTests — a @Device helper that takes Shared<T> lowers on
// vulkan, and each call site reads and writes its own array (spirv-kernel-parity 2.1).
//
// Until 2026-11-18 the SPIR-V target refused such a helper ("a Shared<T> parameter
// has no pointer form"), which held cajeta-llm's int8 tiles off vulkan through
// TileOps.scaledAccumI32. Helpers are always inlined before SPIR-V codegen, so the
// panel resolves to the caller's workgroup variable.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include <string>

using cajeta_test::CajetaJit;
using cajeta::xpu::vulkan::VulkanDriver;

namespace {

// Two panels through the same two helpers; a cross-lane read after the barrier.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "public class ShVk {\n"
    "    @Device\n"
    "    public static void put(Shared<int32> s, uint32 i, int32 v) { s[i] = v; }\n"
    "    @Device\n"
    "    public static int32 take(Shared<int32> s, uint32 i) { return s[i]; }\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out) {\n"
    "        Shared<int32> a = shared int32[64];\n"
    "        Shared<int32> b = shared int32[64];\n"
    "        uint32 t = KernelThread.x();\n"
    "        ShVk.put(a, t, (int32) t * 3);\n"
    "        ShVk.put(b, t, (int32) t * 5 + 1);\n"
    "        Barrier.workgroup();\n"
    "        out[t] = ShVk.take(a, 63 - t) + 1000 * ShVk.take(b, (t + 1) % 64);\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 n = 64;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);\n"
    "        out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(out);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            int32 want = (int32) ((63 - i) * 3) + 1000 * (int32) (((i + 1) % 64) * 5 + 1);\n"
    "            if (hout[i] != want) { return (int32) (100 + i); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

}  // namespace

TEST(SpirvSharedHelperParamTests, aHelperTakingSharedRunsOnVulkanWithEachSiteOnItsOwnPanel) {
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan device/driver available";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.ShVk", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: out[i] wrong, a panel read the other)";
}
