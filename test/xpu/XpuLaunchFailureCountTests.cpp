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

// 1.6.8: a bindless array over the 16-buffer cap is skipped while the launch's resources
// are set up, and counts like any other failure. count 0 keeps the kernel from reading.
const char* kBindlessSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class B {\n"
    "    @Kernel\n"
    "    public static void gather(KernelBuffer<int32>[] bufs, uint32 count,\n"
    "                              KernelBuffer<int32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int32 s = 0;\n"
    "            for (uint32 b = 0; b < count; b = b + 1) {\n"
    "                s = s + bufs[b][i];\n"
    "            }\n"
    "            out[i] = s;\n"
    "        }\n"
    "    }\n"
    "    /** Failures one launch over `k` buffers adds, +10 if it threw, +100 if a\n"
    "     *  legal one wrote wrong. */\n"
    "    public static int64 failuresWith(int32 k) {\n"
    "        uint32 n = 64;\n"
    "        KernelBuffer<int32>[] bufs = heap KernelBuffer<int32>[k];\n"
    "        for (int32 b = 0; b < k; b = b + 1) {\n"
    "            int32[] h = heap int32[n];\n"
    "            for (uint32 i = 0; i < n; i = i + 1) {\n"
    "                h[i] = (int32)((b + 1) * 10 + (int32) i);\n"
    "            }\n"
    "            bufs[b] = heap KernelBuffer<int32>(n);\n"
    "            bufs[b].upload(h);\n"
    "        }\n"
    "        uint32 used = 0;\n"
    "        if (k <= 16) { used = (uint32) k; }\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Device.launchFailures();\n"
    "        int64 thrown = 0;\n"
    "        try {\n"
    "            gather.launch(s, grid: [1], block: [64])(bufs, used, out, n);\n"
    "        } catch (XpuLaunchException e) {\n"
    "            thrown = 10;\n"
    "        }\n"
    "        s.sync();\n"
    "        int64 after = Device.launchFailures();\n"
    "        int64 wrong = 0;\n"
    "        if (k <= 16) {\n"
    "            int32[] ho = heap int32[n];\n"
    "            out.download(ho);\n"
    "            for (uint32 i = 0; i < n; i = i + 1) {\n"
    "                int32 want = 0;\n"
    "                for (int32 b = 0; b < k; b = b + 1) { want = want + (b + 1) * 10 + (int32) i; }\n"
    "                if (ho[i] != want) { wrong = 100; }\n"
    "            }\n"
    "        }\n"
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

// Seventeen buffers count one failure without throwing; three after it run and count none.
void expectBindlessCounted(cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    auto jit = CajetaJit::compile(kBindlessSource, "test.B", o);
    ASSERT_NE(jit, nullptr);
    auto with = jit->lookup<int64_t (*)(int32_t)>("failuresWith");
    ASSERT_NE(with, nullptr);
    const int64_t over = with(17);
    EXPECT_EQ(over, 1) << "a 17-buffer launch moved launchFailures() by " << over % 10
                       << (over >= 10 ? " and threw" : "");
    EXPECT_EQ(with(3), 0) << "a legal launch counted a failure (1), threw (10) or "
                             "wrote wrong (100) after the skipped one";
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

TEST(XpuLaunchFailureCount, aCudaBindlessArrayOverTheCapIsCounted) {
    CAJETA_SKIP_IF_NO_CUDA();
    expectBindlessCounted(cajeta::xpu::Backend::Nvptx);
}

TEST(XpuLaunchFailureCount, aHipBindlessArrayOverTheCapIsCounted) {
    CAJETA_SKIP_IF_NO_HIP();
    expectBindlessCounted(cajeta::xpu::Backend::Amdgpu);
}

TEST(XpuLaunchFailureCount, aVulkanBindlessArrayOverTheCapIsCounted) {
    CAJETA_SKIP_IF_NO_VULKAN();
    expectBindlessCounted(cajeta::xpu::Backend::Spirv);
}

TEST(XpuLaunchFailureCount, aCpuBindlessArrayOverTheCapIsCounted) {
    expectBindlessCounted(cajeta::xpu::Backend::Cpu);
}
