// The reserved multi-kernel-scheduler seams (xpu-cooperative-tile §7, Phase A
// Unit 6) — thin, no machinery. A kernel invocation routed through
// Scheduler.submit(submission, launchThunk) launches IMMEDIATELY today, while
// the submission carries the launch geometry, the SchedulePolicy, the
// ArithmeticCharacter, and the read/write buffer sets a future scheduler needs
// to reorder/fuse and to build the inter-kernel dependency DAG (§7.3). submit
// emits a KernelResourceDescriptor from what the schedule already knows (§7.1) —
// geometry + wave count real, LDS the declared reservation, VGPR the honest stub
// until the backend surfaces register counts (§5.2). NOTHING consumes the seams
// yet (§7.1 acceptance 6.3.1); these tests only prove the plug-in points exist,
// carry the right data, and do not disturb the immediate launch.
//
// CPU backend, so it always runs (no device needed): waveWidth is 1 there, so
// waveCount == gridX*blockX and the arithmetic is trivially checkable.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// run(): route an elementwise add through the submit seam, then assert the seam
//   (a) launched immediately (the kernel actually ran),
//   (b) recorded the read/write sets by buffer-handle identity (§7.2/§7.3),
//   (c) emitted a resource descriptor with real geometry+waves, LDS=0, VGPR stub,
//   (d) defaulted the policy to Throughput and carried the character (§6.3/§7.4).
// runPolicy(): the same seam with the policy + character OVERWRITTEN, proving the
//   parameters are carried through rather than pinned.
// Each returns 0 on success, or a distinct non-zero code for the failing check.
const char* kSubmitSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Scheduler;\n"
    "import cajeta.xpu.KernelSubmission;\n"
    "import cajeta.xpu.KernelResourceDescriptor;\n"
    "import cajeta.xpu.SchedulePolicy;\n"
    "import cajeta.xpu.ArithmeticCharacter;\n"
    "public class Sub {\n"
    "    @Kernel\n"
    "    public static void add(KernelBuffer<int32> out, KernelBuffer<int32> a,\n"
    "                           KernelBuffer<int32> b) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < 64) { out[i] = a[i] + b[i]; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int32 n = 64;\n"
    "        int32[] ha = heap int32[n];\n"
    "        int32[] hb = heap int32[n];\n"
    "        for (int32 i = 0; i < n; i = i + 1) { ha[i] = i; hb[i] = 2 * i; }\n"
    "        KernelBuffer<int32> da = heap KernelBuffer<int32>(0, n);\n"
    "        KernelBuffer<int32> db = heap KernelBuffer<int32>(0, n);\n"
    "        KernelBuffer<int32> dc = heap KernelBuffer<int32>(0, n);\n"
    "        da.allocate();\n"
    "        db.allocate();\n"
    "        dc.allocate();\n"
    "        da.upload(ha);\n"
    "        db.upload(hb);\n"
    "        int64 haH = da.handle();\n"
    "        int64 hbH = db.handle();\n"
    "        int64 hcH = dc.handle();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
    "        sub.reads(da.handle());\n"
    "        sub.reads(db.handle());\n"
    "        sub.writes(dc.handle());\n"
    "        KernelResourceDescriptor desc #= Scheduler.submit(sub);\n"
    "        add.launch(s, grid: [sub.gridX], block: [sub.blockX])(dc, da, db);\n"
    "        s.sync();\n"
    "        int32[] hc = heap int32[n];\n"
    "        dc.download(hc);\n"
    "        da.free();\n"
    "        db.free();\n"
    "        dc.free();\n"
    "        for (int32 i = 0; i < n; i = i + 1) {\n"
    "            if (hc[i] != ha[i] + hb[i]) { return 10; }\n"
    "        }\n"
    "        if (desc.gridX != 64) { return 20; }\n"
    "        if (desc.blockX != 1) { return 21; }\n"
    "        if (desc.waveCount != 64) { return 22; }\n"
    "        if (desc.ldsBytes != 0) { return 23; }\n"
    "        if (desc.vgprCount != 0) { return 24; }\n"
    "        if (sub.readSet.count() != 2) { return 30; }\n"
    "        if (sub.writeSet.count() != 1) { return 31; }\n"
    "        if (sub.readSet.get(0) != haH) { return 32; }\n"
    "        if (sub.readSet.get(1) != hbH) { return 33; }\n"
    "        if (sub.writeSet.get(0) != hcH) { return 34; }\n"
    "        if (sub.policy != SchedulePolicy.Throughput) { return 40; }\n"
    "        if (desc.character != ArithmeticCharacter.MemoryBound) { return 41; }\n"
    "        return 0;\n"
    "    }\n"
    "    public static int32 runPolicy() {\n"
    "        int32 n = 64;\n"
    "        int32[] ha = heap int32[n];\n"
    "        int32[] hb = heap int32[n];\n"
    "        for (int32 i = 0; i < n; i = i + 1) { ha[i] = i; hb[i] = i; }\n"
    "        KernelBuffer<int32> da = heap KernelBuffer<int32>(0, n);\n"
    "        KernelBuffer<int32> db = heap KernelBuffer<int32>(0, n);\n"
    "        KernelBuffer<int32> dc = heap KernelBuffer<int32>(0, n);\n"
    "        da.allocate();\n"
    "        db.allocate();\n"
    "        dc.allocate();\n"
    "        da.upload(ha);\n"
    "        db.upload(hb);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
    "        sub.writes(dc.handle());\n"
    "        sub.policy = SchedulePolicy.Latency;\n"
    "        sub.character = ArithmeticCharacter.ComputeBound;\n"
    "        sub.ldsBytes = 128;\n"
    "        KernelResourceDescriptor desc #= Scheduler.submit(sub);\n"
    "        add.launch(s, grid: [sub.gridX], block: [sub.blockX])(dc, da, db);\n"
    "        s.sync();\n"
    "        int32[] hc = heap int32[n];\n"
    "        dc.download(hc);\n"
    "        da.free();\n"
    "        db.free();\n"
    "        dc.free();\n"
    "        if (hc[3] != 6) { return 10; }\n"
    "        if (sub.policy != SchedulePolicy.Latency) { return 50; }\n"
    "        if (desc.character != ArithmeticCharacter.ComputeBound) { return 51; }\n"
    "        if (desc.ldsBytes != 128) { return 52; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

} // namespace

// 6.1.1 / 6.1.2 / 6.2.1 / 6.2.2: the submit seam launches immediately, records
// the read/write sets, and emits the resource descriptor.
TEST(XpuSchedulerSeam, submitLaunchesRecordsAndDescribes) {
    auto jit = CajetaJit::compile(kSubmitSource, "test.Sub", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0) << "submit-seam check failed (see the return code in "
                          "XpuSchedulerSeamTests.cpp)";
}

// 6.2.3 / 7.4: the schedule policy and arithmetic character are CARRIED through
// the seam (default Throughput; overwritable), not pinned.
TEST(XpuSchedulerSeam, policyAndCharacterAreCarried) {
    auto jit = CajetaJit::compile(kSubmitSource, "test.Sub", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("runPolicy");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0) << "submit-seam policy/character check failed";
}
