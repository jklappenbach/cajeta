// The Vulkan batch's command-buffer ring and scalar arena reclaim one chunk at a time, so a
// wrap waits for the oldest chunk alone and the device keeps its queue (vulkan-batch-pipelining
// Unit 6, spec 2.8).

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

// wrap(iters): 26 bumps, 6 spins, 10 bumps; packs record ms << 40 | sync ms << 20 | sums.
// run(n): n bumps (each adds 4 through four scalars), one sync; returns the sum.
const char* kSource =
    "package test;\n"
    "import cajeta.time.Clock;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class R {\n"
    "    @Kernel\n"
    "    public static void spin(KernelBuffer<int32> out, uint32 iters) {\n"
    "        uint32 acc = KernelThread.x();\n"
    "        uint32 i = 0;\n"
    "        while (i < iters) { acc = acc * 1664525 + 1013904223 + i; i = i + 1; }\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + 1; }\n"
    "        if (acc == (uint32) 12345) { out[1] = 1; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void bump(KernelBuffer<int32> out, int32 a, int32 b, int32 c, int32 d) {\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + a + b + c + d; }\n"
    "    }\n"
    "    static int32 readOne(KernelBuffer<int32> buf) {\n"
    "        int32[] got #= heap int32[2];\n"
    "        buf.download(got);\n"
    "        return got[0];\n"
    "    }\n"
    "    public static int64 wrap(uint32 iters) {\n"
    "        KernelBuffer<int32> a = heap KernelBuffer<int32>(0, 2);\n"
    "        KernelBuffer<int32> b = heap KernelBuffer<int32>(0, 2);\n"
    "        a.allocate(); b.allocate();\n"
    "        int32[] zero #= heap int32[2];\n"
    "        a.upload(zero); b.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 i = 0;\n"
    "        while (i < 26) { bump.launch(s, grid: [1], block: [1])(a, 1, 1, 1, 1); i = i + 1; }\n"
    "        int64 t0 = Clock.nanoTime();\n"
    "        i = 0;\n"
    "        while (i < 6) { spin.launch(s, grid: [1], block: [64])(b, iters); i = i + 1; }\n"
    "        i = 0;\n"
    "        while (i < 10) { bump.launch(s, grid: [1], block: [1])(a, 1, 1, 1, 1); i = i + 1; }\n"
    "        int64 t1 = Clock.nanoTime();\n"
    "        s.sync();\n"
    "        int64 t2 = Clock.nanoTime();\n"
    "        int64 sums = (int64) readOne(a) * 100L + (int64) readOne(b);\n"
    "        a.free(); b.free();\n"
    "        return (((t1 - t0) / 1000000L) << 40) | (((t2 - t1) / 1000000L) << 20) | sums;\n"
    "    }\n"
    "    public static int64 run(int32 n) {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 2);\n"
    "        out.allocate();\n"
    "        int32[] zero #= heap int32[2];\n"
    "        out.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 i = 0;\n"
    "        while (i < n) { bump.launch(s, grid: [1], block: [1])(out, 1, 1, 1, 1); i = i + 1; }\n"
    "        s.sync();\n"
    "        int64 r = (int64) readOne(out);\n"
    "        out.free();\n"
    "        return r;\n"
    "    }\n"
    "}\n";

struct Prog {
    std::unique_ptr<CajetaJit> jit;
    int64_t (*wrap)(uint32_t) = nullptr;
    int64_t (*run)(int32_t) = nullptr;
};
Prog compile() {
    Prog p;
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    p.jit = CajetaJit::compile(kSource, "test.R", o);
    if (p.jit) {
        p.wrap = p.jit->lookup<int64_t (*)(uint32_t)>("wrap");
        p.run = p.jit->lookup<int64_t (*)(int32_t)>("run");
    }
    return p;
}
const uint32_t kIters = 3000000u;   // one spin takes several ms on gfx1151

// Runs `f` with CAJETA_XPU_VK_CHUNK=chunk and the sync log on; returns stderr.
template <class F> std::string withChunk(const char* chunk, F f) {
    setenv("CAJETA_XPU_VK_SYNC_LOG", "1", 1);
    setenv("CAJETA_XPU_VK_CHUNK", chunk, 1);
    testing::internal::CaptureStderr();
    f();
    std::string err = testing::internal::GetCapturedStderr();
    unsetenv("CAJETA_XPU_VK_CHUNK");
    unsetenv("CAJETA_XPU_VK_SYNC_LOG");
    return err;
}

size_t count(const std::string& s, const char* needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}

} // namespace

// 6.1.1 — the wrap onto slot 0 waits for slot 0's chunk, not for the spins queued behind it.
TEST(XpuVulkanBatchRing, aRingWrapDoesNotWaitForTheQueue) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.wrap, nullptr);
    int64_t r = 0;
    const std::string err = withChunk("1", [&] { r = p.wrap(kIters); });
    const int64_t rec = r >> 40, sy = (r >> 20) & 0xFFFFF, sums = r & 0xFFFFF;
    EXPECT_EQ(sums, 144 * 100 + 6) << err.substr(0, 400);
    EXPECT_LT(rec, 15) << "the wrap waited for the queue (" << rec << " ms recording)";
    EXPECT_GT(sy, 15) << "the spins had finished before the sync (" << sy << " ms): no overlap to lose";
    EXPECT_EQ(count(err, "FAILED"), 0u) << err.substr(0, 400);
}

// 6.1.2 — many wraps, each reusing a slot's command buffer and descriptor sets.
TEST(XpuVulkanBatchRing, manyWrapsKeepEveryResult) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    int64_t one = 0, four = 0;
    const std::string e1 = withChunk("1", [&] { one = p.run(2000); });
    const std::string e4 = withChunk("4", [&] { four = p.run(2000); });
    EXPECT_EQ(one, 8000) << e1.substr(0, 400);
    EXPECT_EQ(four, 8000) << e4.substr(0, 400);
    EXPECT_EQ(count(e1, "FAILED") + count(e4, "FAILED"), 0u);
}

// 6.1.3 — more scalar slots than the arena holds: it reclaims behind the device, no extra flush.
TEST(XpuVulkanBatchRing, theScalarArenaReclaimsBehindTheDevice) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    int64_t sum = 0;
    const std::string err = withChunk("128", [&] { sum = p.run(70000); });
    EXPECT_EQ(sum, 280000) << err.substr(0, 400);
    EXPECT_EQ(count(err, "sync-log "), 1u) << "flushes besides the sync's:\n" << err.substr(0, 800);
}

// 6.1.4 — a launch's scalar views own no Vulkan object, so freeing them never lands the batch.
TEST(XpuVulkanBatchRing, scalarViewsDoNotLandTheBatch) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    int64_t sum = 0;
    // 5000 launches free 20000 scalar views, past the 16384 deferred frees; the arena holds them.
    const std::string err = withChunk("128", [&] { sum = p.run(5000); });
    EXPECT_EQ(sum, 20000) << err.substr(0, 400);
    EXPECT_EQ(count(err, "sync-log "), 1u) << "flushes besides the sync's:\n" << err.substr(0, 800);
}
