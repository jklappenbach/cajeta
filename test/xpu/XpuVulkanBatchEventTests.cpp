// Events inside the Vulkan batch (vulkan-batch-pipelining Unit 4, spec 5): a recorded
// event marks a point in the batch the host can wait on while the device runs what was
// recorded after it, and a host read flushes only when a later dispatch writes the buffer.

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

// Group A: `spin` launches on `a` (each adds 1 after a long loop); an event; group B on `b`.
// run(mode) packs milliseconds: record << 40 | read << 20 | sync, or values for modes 2, 3.
const char* kSource =
    "package test;\n"
    "import cajeta.time.Clock;\n"
    "import cajeta.xpu.Event;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class E {\n"
    "    @Kernel\n"
    "    public static void spin(KernelBuffer<int32> out, uint32 iters) {\n"
    "        uint32 acc = KernelThread.x();\n"
    "        uint32 i = 0;\n"
    "        while (i < iters) { acc = acc * 1664525 + 1013904223 + i; i = i + 1; }\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + 1; }\n"
    "        if (acc == (uint32) 12345) { out[1] = 1; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void bump(KernelBuffer<int32> out, int32 a) {\n"
    "        if (KernelThread.x() == 0) { out[0] = out[0] + a; }\n"
    "    }\n"
    "    static void group(KernelStream s, KernelBuffer<int32> buf, int32 n, uint32 iters) {\n"
    "        int32 i = 0;\n"
    "        while (i < n) { spin.launch(s, grid: [1], block: [64])(buf, iters); i = i + 1; }\n"
    "    }\n"
    "    static int32 readOne(KernelBuffer<int32> buf) {\n"
    "        int32[] got #= heap int32[2];\n"
    "        buf.download(got);\n"
    "        return got[0];\n"
    "    }\n"
    "    public static int64 run(int32 mode, uint32 iters) {\n"
    "        KernelBuffer<int32> a = heap KernelBuffer<int32>(0, 2);\n"
    "        KernelBuffer<int32> b = heap KernelBuffer<int32>(0, 2);\n"
    "        a.allocate(); b.allocate();\n"
    "        int32[] zero #= heap int32[2];\n"
    "        a.upload(zero); b.upload(zero);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        Event ev #= Event.create();\n"
    "        int64 result = 0L;\n"
    "        if (mode == 0 || mode == 1) {\n"
    "            group(s, a, 20, iters);\n"
    "            int64 t0 = Clock.nanoTime();\n"
    "            ev.recordOn(s);\n"
    "            int64 t1 = Clock.nanoTime();\n"
    "            group(s, b, 20, iters);\n"
    "            ev.waitHost();\n"
    "            int64 t2 = Clock.nanoTime();\n"
    "            int32 v = mode == 0 ? readOne(a) : readOne(b);\n"
    "            int64 t3 = Clock.nanoTime();\n"
    "            s.sync();\n"
    "            int64 t4 = Clock.nanoTime();\n"
    "            int64 rec = (t1 - t0) / 1000000L;\n"
    "            int64 rd = (t3 - t2) / 1000000L;\n"
    "            int64 sy = (t4 - t3) / 1000000L;\n"
    "            if (v != 20) { rec = 9999L; }\n"
    "            result = (rec << 40) | (rd << 20) | sy;\n"
    "        } else if (mode == 2) {\n"
    "            group(s, a, 20, iters);\n"
    "            ev.recordOn(s);\n"
    "            group(s, b, 20, iters);\n"
    "            ev.waitHost();\n"
    "            int32 a1 = readOne(a);\n"
    "            group(s, a, 20, iters);\n"
    "            ev.recordOn(s);\n"
    "            group(s, b, 20, iters);\n"
    "            ev.waitHost();\n"
    "            int32 a2 = readOne(a);\n"
    "            s.sync();\n"
    "            result = (int64) a1 * 1000L + (int64) a2;\n"
    "        } else {\n"
    "            int32 ok = 0;\n"
    "            int32 k = 0;\n"
    "            while (k < 6) {\n"
    "                int32 i = 0;\n"
    "                while (i < 50) { bump.launch(s, grid: [1], block: [1])(a, 4); i = i + 1; }\n"
    "                ev.recordOn(s);\n"
    "                ev.waitHost();\n"
    "                if (readOne(a) == 200 * (k + 1)) { ok = ok + 1; }\n"
    "                k = k + 1;\n"
    "            }\n"
    "            s.sync();\n"
    "            result = (int64) ok * 10000L + (int64) readOne(a);\n"
    "        }\n"
    "        ev.destroy();\n"
    "        a.free(); b.free();\n"
    "        return result;\n"
    "    }\n"
    "}\n";

struct Prog {
    std::unique_ptr<CajetaJit> jit;
    int64_t (*run)(int32_t, uint32_t) = nullptr;
};
Prog compile() {
    Prog p;
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    p.jit = CajetaJit::compile(kSource, "test.E", o);
    if (p.jit) p.run = p.jit->lookup<int64_t (*)(int32_t, uint32_t)>("run");
    return p;
}
const uint32_t kIters = 3000000u;   // a group of 20 takes well over 100 ms on gfx1151

} // namespace

// 5.1/5.2 — recording is instant, the wait lets the host read group A while B runs, and
// the sync at the end is where B is waited for.
TEST(XpuVulkanBatchEvent, aWaitedEventDoesNotLandTheBatch) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    const int64_t r = p.run(0, kIters);
    const int64_t rec = r >> 40, rd = (r >> 20) & 0xFFFFF, sy = r & 0xFFFFF;
    EXPECT_LT(rec, 30) << "recording the event landed the batch (" << rec << " ms)";
    EXPECT_LT(rd, 30) << "reading group A's buffer flushed group B (" << rd << " ms)";
    EXPECT_GT(sy, 30) << "the sync had nothing left to wait for (" << sy << " ms): B did not overlap";
}

// 5.3 — a buffer written after the event is not readable without the flush.
TEST(XpuVulkanBatchEvent, aReadOfABufferWrittenAfterTheEventFlushes) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    const int64_t r = p.run(1, kIters);
    const int64_t rec = r >> 40, rd = (r >> 20) & 0xFFFFF;
    EXPECT_LT(rec, 30) << rec;
    EXPECT_GT(rd, 30) << "reading group B's buffer returned before B ran (" << rd << " ms)";
}

// 5.4 — a re-recorded event waits for its own, later point.
TEST(XpuVulkanBatchEvent, reRecordResetsTheEvent) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    EXPECT_EQ(p.run(2, 200000u), 20 * 1000 + 40);
}

// Bit-identity: six checkpoints of a dependent chain each read the exact sum.
TEST(XpuVulkanBatchEvent, resultsAreBitIdenticalWithEvents) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    EXPECT_EQ(p.run(3, 0u), 6 * 10000 + 1200);
}

// 5.5 — without device events the sentinel remains: record lands the batch, results hold.
TEST(XpuVulkanBatchEvent, withoutEventsTheSentinelRemains) {
    CAJETA_SKIP_IF_NO_VULKAN();
    Prog p = compile();
    ASSERT_NE(p.run, nullptr);
    setenv("CAJETA_XPU_FAULT", "vk-no-events", 1);
    const int64_t r2 = p.run(2, 200000u);
    const int64_t r3 = p.run(3, 0u);
    unsetenv("CAJETA_XPU_FAULT");
    EXPECT_EQ(r2, 20 * 1000 + 40);
    EXPECT_EQ(r3, 6 * 10000 + 1200);
}
