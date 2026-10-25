// The Vulkan batch hands its first chunk to the device early and the rest in chunks,
// so the device runs while the host records (vulkan-batch-pipelining Unit 1).

#include <gtest/gtest.h>

#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// `run(n)`: n dependent launches (each adds 4 to out[0]) on the current stream, one
// sync; returns the sum, or +1e9 when a launch throws.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class B {\n"
    "    @Kernel\n"
    "    public static void bump(KernelBuffer<int32> out, int32 a, int32 b, int32 c, int32 d) {\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + a + b + c + d; }\n"
    "    }\n"
    "    public static int64 run(int32 n) {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        out.allocate();\n"
    "        int32[] zero #= heap int32[1];\n"
    "        out.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 thrown = 0L;\n"
    "        int32 i = 0;\n"
    "        try {\n"
    "            while (i < n) {\n"
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
    "        return thrown + (int64) got[0];\n"
    "    }\n"
    "}\n";

struct Cadence {
    std::unique_ptr<CajetaJit> jit;
    int64_t (*run)(int32_t) = nullptr;
};

Cadence compile() {
    Cadence c;
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    c.jit = CajetaJit::compile(kSource, "test.B", o);
    if (c.jit) c.run = c.jit->lookup<int64_t (*)(int32_t)>("run");
    return c;
}

// Run `n` launches with the given knobs; returns the sum and the sync-log stderr.
int64_t runWith(const Cadence& c, int n, const char* chunk, const char* first,
                bool nochunk, std::string* err) {
    setenv("CAJETA_XPU_VK_SYNC_LOG", "1", 1);
    if (chunk) setenv("CAJETA_XPU_VK_CHUNK", chunk, 1); else unsetenv("CAJETA_XPU_VK_CHUNK");
    if (first) setenv("CAJETA_XPU_VK_FIRST_CHUNK", first, 1); else unsetenv("CAJETA_XPU_VK_FIRST_CHUNK");
    if (nochunk) setenv("CAJETA_XPU_VK_NOCHUNK", "1", 1); else unsetenv("CAJETA_XPU_VK_NOCHUNK");
    testing::internal::CaptureStderr();
    const int64_t r = c.run(n);
    *err = testing::internal::GetCapturedStderr();
    unsetenv("CAJETA_XPU_VK_SYNC_LOG");
    unsetenv("CAJETA_XPU_VK_CHUNK");
    unsetenv("CAJETA_XPU_VK_FIRST_CHUNK");
    unsetenv("CAJETA_XPU_VK_NOCHUNK");
    return r;
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

} // namespace

TEST(XpuVulkanBatchCadence, firstChunkSubmitsAfterEightDispatches) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Cadence c = compile();
    ASSERT_NE(c.run, nullptr);
    std::string err;
    EXPECT_EQ(runWith(c, 44, nullptr, nullptr, false, &err), 176);
    // 44 dispatches: 8 (first), 16, 16, and 4 with the flush.
    EXPECT_TRUE(has(err, "sync-log 44 dispatches")) << err.substr(0, 400);
    EXPECT_TRUE(has(err, "3 chunks, first 8")) << err.substr(0, 400);
}

TEST(XpuVulkanBatchCadence, knobsSetTheCadence) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Cadence c = compile();
    ASSERT_NE(c.run, nullptr);
    std::string err;
    EXPECT_EQ(runWith(c, 40, "8", nullptr, false, &err), 160);
    EXPECT_TRUE(has(err, "5 chunks, first 8")) << err.substr(0, 400);
    EXPECT_EQ(runWith(c, 40, "100", "4", false, &err), 160);
    EXPECT_TRUE(has(err, "1 chunks, first 4")) << err.substr(0, 400);
    EXPECT_EQ(runWith(c, 40, nullptr, nullptr, true, &err), 160);
    EXPECT_TRUE(has(err, "0 chunks")) << err.substr(0, 400);
}

TEST(XpuVulkanBatchCadence, resultsAreBitIdenticalUnderEveryCadence) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Cadence c = compile();
    ASSERT_NE(c.run, nullptr);
    std::string err;
    EXPECT_EQ(runWith(c, 300, "4", nullptr, false, &err), 1200) << err.substr(0, 300);
    EXPECT_EQ(runWith(c, 300, "16", nullptr, false, &err), 1200) << err.substr(0, 300);
    EXPECT_EQ(runWith(c, 300, "100", nullptr, false, &err), 1200) << err.substr(0, 300);
    EXPECT_EQ(runWith(c, 300, nullptr, nullptr, true, &err), 1200) << err.substr(0, 300);
}

TEST(XpuVulkanBatchCadence, ringWrapWaitsAndReclaims) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Cadence c = compile();
    ASSERT_NE(c.run, nullptr);
    std::string err;
    // Chunks of one cross the 32-slot command-buffer ring; the batch waits, resets and goes on.
    EXPECT_EQ(runWith(c, 40, "1", nullptr, false, &err), 160) << err.substr(0, 300);
    EXPECT_TRUE(has(err, "40 chunks, first 1")) << err.substr(0, 400);
    EXPECT_FALSE(has(err, "FAILED")) << err.substr(0, 400);
}
