// xpu-tile-scheduling Unit 1 — every launch is a submission (spec §2, D12).
// The unchanged `k.launch(s, grid:, block:)(args)` form arrives at the
// scheduler as a default-policy submission with its access sets from the
// manifest; a KernelSubmission in the stream's place carries its policy,
// window, protection, CU budget and capture identity; the bypass switch is
// the only unscheduled path; a kernel with no manifest runs unclassified with
// a diagnostic; a rejection returns its reason; the submission is a value
// that crosses a fiber boundary. CPU backend throughout.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// One program, one kernel, one entry per use case. Each entry returns 0 on
// success or a distinct code naming the failed check.
const char* kSource =
    "package test;\n"
    "import cajeta.time.Clock;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.KernelSubmission;\n"
    "import cajeta.xpu.Scheduler;\n"
    "import cajeta.xpu.SchedulePolicy;\n"
    "import cajeta.xpu.SubmissionRecord;\n"
    "import cajeta.xpu.XpuLaunchException;\n"
    "public class Sub {\n"
    "    @Kernel\n"
    "    public static void add(KernelBuffer<int32> out, KernelBuffer<int32> a,\n"
    "                           KernelBuffer<int32> b) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < 64) { out[i] = a[i] + b[i]; }\n"
    "    }\n"
    "    static int32 n = 64;\n"
    "    static KernelBuffer<int32> da;\n"
    "    static KernelBuffer<int32> db;\n"
    "    static KernelBuffer<int32> dc;\n"
    "    static void setUp() {\n"
    "        int32[] ha = heap int32[Sub.n];\n"
    "        int32[] hb = heap int32[Sub.n];\n"
    "        for (int32 i = 0; i < Sub.n; i = i + 1) { ha[i] = i; hb[i] = 2 * i; }\n"
    "        Sub.da = heap KernelBuffer<int32>(0, Sub.n);\n"
    "        Sub.db = heap KernelBuffer<int32>(0, Sub.n);\n"
    "        Sub.dc = heap KernelBuffer<int32>(0, Sub.n);\n"
    "        Sub.da.allocate();\n"
    "        Sub.db.allocate();\n"
    "        Sub.dc.allocate();\n"
    "        Sub.da.upload(ha);\n"
    "        Sub.db.upload(hb);\n"
    "        int32[] zero = heap int32[Sub.n];\n"
    "        Sub.dc.upload(zero);\n"
    "    }\n"
    "    static int32 sumOut() {\n"
    "        int32[] hc = heap int32[Sub.n];\n"
    "        Sub.dc.download(hc);\n"
    "        int32 s = 0;\n"
    "        for (int32 i = 0; i < Sub.n; i = i + 1) { s = s + hc[i]; }\n"
    "        return s;\n"
    "    }\n"
    "    static void tearDown() {\n"
    "        Sub.da.free();\n"
    "        Sub.db.free();\n"
    "        Sub.dc.free();\n"
    "    }\n"
    // 1.1.1 (a): the unchanged launch form is a default-policy submission whose
    // read and write sets come from the manifest.
    "    public static int32 plainLaunch() {\n"
    "        Sub.setUp();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        add.launch(s, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        s.sync();\n"
    "        int32 sum = Sub.sumOut();\n"
    "        SubmissionRecord r #= Scheduler.last();\n"
    "        int64 ha = Sub.da.handle();\n"
    "        int64 hb = Sub.db.handle();\n"
    "        int64 hc = Sub.dc.handle();\n"
    "        Sub.tearDown();\n"
    "        if (sum != 3 * (63 * 64 / 2)) { return 10; }\n"
    "        if (Scheduler.submissionCount() != before + 1L) { return 11; }\n"
    "        if (r == null) { return 12; }\n"
    "        if (!r.kernel.equals(\"add\")) { return 13; }\n"
    "        if (r.policy != SchedulePolicy.Throughput) { return 14; }\n"
    "        if (r.protectedLaunch) { return 15; }\n"
    "        if (r.gridX != 64 || r.blockX != 1) { return 16; }\n"
    "        if (!r.classified) { return 17; }\n"
    "        if (r.readSet.count() != 2) { return 18; }\n"
    "        if (r.writeSet.count() != 1) { return 19; }\n"
    "        if (r.readSet.get(0) != ha || r.readSet.get(1) != hb) { return 20; }\n"
    "        if (r.writeSet.get(0) != hc) { return 21; }\n"
    "        if (r.accumulateSet.count() != 0) { return 22; }\n"
    "        return 0;\n"
    "    }\n"
    // 1.1.1 (b): a KernelSubmission in the stream's place carries what a
    // default cannot know.
    "    public static int32 submissionLaunch() {\n"
    "        Sub.setUp();\n"
    "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
    "        sub.policy = SchedulePolicy.Latency;\n"
    "        int64 now = Clock.nanoTime();\n"
    "        sub.earliestNs = now;\n"
    "        sub.latestNs = now + 5000000000L;\n"
    "        sub.protectedLaunch = true;\n"
    "        sub.cuBudget = 4;\n"
    "        sub.captureId = 77L;\n"
    "        sub.energyJoules = 0.5;\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        add.launch(sub, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        s.sync();\n"
    "        int32 sum = Sub.sumOut();\n"
    "        SubmissionRecord r #= Scheduler.last();\n"
    "        Sub.tearDown();\n"
    "        if (sum != 3 * (63 * 64 / 2)) { return 10; }\n"
    "        if (Scheduler.submissionCount() != before + 1L) { return 11; }\n"
    "        if (r.policy != SchedulePolicy.Latency) { return 30; }\n"
    "        if (r.earliestNs != now) { return 31; }\n"
    "        if (r.latestNs != now + 5000000000L) { return 32; }\n"
    "        if (!r.protectedLaunch) { return 33; }\n"
    "        if (r.cuBudget != 4) { return 34; }\n"
    "        if (r.captureId != 77L) { return 35; }\n"
    "        if (r.energyJoules != 0.5) { return 36; }\n"
    "        if (r.readSet.count() != 2 || r.writeSet.count() != 1) { return 37; }\n"
    "        return 0;\n"
    "    }\n"
    // 1.1.2: with the bypass switch set the launch takes the raw ABI and the
    // scheduler sees nothing.
    "    public static int32 bypassed() {\n"
    "        Sub.setUp();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        int64 bypassedBefore = Scheduler.bypassedCount();\n"
    "        Scheduler.bypass(true);\n"
    "        add.launch(s, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        s.sync();\n"
    "        Scheduler.bypass(false);\n"
    "        int32 sum = Sub.sumOut();\n"
    "        Sub.tearDown();\n"
    "        if (sum != 3 * (63 * 64 / 2)) { return 10; }\n"
    "        if (Scheduler.submissionCount() != before) { return 40; }\n"
    "        if (Scheduler.bypassedCount() != bypassedBefore + 1L) { return 41; }\n"
    "        return 0;\n"
    "    }\n"
    // 1.1.4 (a): no manifest for the kernel on the active target; the launch
    // runs as an unclassified submission.
    "    public static int32 unclassified() {\n"
    "        Sub.setUp();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        add.launch(s, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        s.sync();\n"
    "        int32 sum = Sub.sumOut();\n"
    "        SubmissionRecord r #= Scheduler.last();\n"
    "        Sub.tearDown();\n"
    "        if (sum != 3 * (63 * 64 / 2)) { return 10; }\n"
    "        if (Scheduler.submissionCount() != before + 1L) { return 11; }\n"
    "        if (r.classified) { return 50; }\n"
    "        if (r.policy != SchedulePolicy.Throughput) { return 51; }\n"
    "        if (r.readSet.count() != 0 || r.writeSet.count() != 0) { return 52; }\n"
    "        return 0;\n"
    "    }\n"
    // 1.1.4 (b): a window that closed before the submission is a rejection
    // with its reason, never a late run.
    "    public static int32 rejected() {\n"
    "        Sub.setUp();\n"
    "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
    "        sub.policy = SchedulePolicy.Latency;\n"
    "        sub.latestNs = Clock.nanoTime() - 1L;\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        int64 rejectedBefore = Scheduler.rejectedCount();\n"
    "        int32 thrown = 0;\n"
    "        try {\n"
    "            add.launch(sub, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        } catch (XpuLaunchException e) {\n"
    "            if (e.getMessage().contains(\"rejected\")\n"
    "                    && e.getMessage().contains(\"window\")) { thrown = 1; }\n"
    "        }\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        s.sync();\n"
    "        int32 sum = Sub.sumOut();\n"
    "        Sub.tearDown();\n"
    "        if (thrown != 1) { return 60; }\n"
    "        if (sum != 0) { return 61; }\n"
    "        if (Scheduler.submissionCount() != before) { return 62; }\n"
    "        if (Scheduler.rejectedCount() != rejectedBefore + 1L) { return 63; }\n"
    "        return 0;\n"
    "    }\n"
    // 1.1.5: the submission is a value; a fiber launches it.
    "    public static async int32 fromFiber(KernelSubmission sub) {\n"
    "        add.launch(sub, grid: [64], block: [1])(Sub.dc, Sub.da, Sub.db);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        s.sync();\n"
    "        return Sub.sumOut();\n"
    "    }\n"
    "    public static int32 acrossFiber() {\n"
    "        Sub.setUp();\n"
    "        KernelSubmission sub = heap KernelSubmission(64, 1);\n"
    "        sub.policy = SchedulePolicy.Latency;\n"
    "        sub.captureId = 91L;\n"
    "        int64 before = Scheduler.submissionCount();\n"
    "        int32 sum = 0;\n"
    "        scope {\n"
    "            Task<int32> t = spawn fromFiber(sub);\n"
    "            sum = await t;\n"
    "        }\n"
    "        SubmissionRecord r #= Scheduler.last();\n"
    "        Sub.tearDown();\n"
    "        if (sum != 3 * (63 * 64 / 2)) { return 10; }\n"
    "        if (Scheduler.submissionCount() != before + 1L) { return 11; }\n"
    "        if (r.policy != SchedulePolicy.Latency) { return 70; }\n"
    "        if (r.captureId != 91L) { return 71; }\n"
    "        return 0;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileProgram() {
    return CajetaJit::compile(kSource, "test.Sub", cpuOptions());
}

int runEntry(CajetaJit& jit, const char* entry) {
    auto fn = jit.lookup<int (*)()>(entry);
    if (!fn) return -2;
    return fn();
}

} // namespace

TEST(XpuLaunchSubmission, thePlainLaunchFormIsADefaultSubmission) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(runEntry(*jit, "plainLaunch"), 0);
}

TEST(XpuLaunchSubmission, aSubmissionInTheStreamsPlaceCarriesItsPolicy) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(runEntry(*jit, "submissionLaunch"), 0);
}

TEST(XpuLaunchSubmission, theBypassSwitchIsTheOnlyUnscheduledPath) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(runEntry(*jit, "bypassed"), 0);
}

TEST(XpuLaunchSubmission, aKernelWithoutAManifestRunsUnclassifiedWithADiagnostic) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    using DropFn = void (*)(const char*, int32_t);
    using BackendFn = int32_t (*)();
    auto drop = reinterpret_cast<DropFn>(
        jit->lookupRawSymbol("__cajeta_xpu_unregister_kernel_manifest"));
    auto backend = reinterpret_cast<BackendFn>(
        jit->lookupRawSymbol("__cajeta_xpu_active_backend_id"));
    ASSERT_NE(drop, nullptr);
    ASSERT_NE(backend, nullptr);
    drop("add", backend());
    testing::internal::CaptureStderr();
    int rc = runEntry(*jit, "unclassified");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 0);
    EXPECT_NE(err.find("[xpu-unclassified]"), std::string::npos) << err;
    EXPECT_NE(err.find("add"), std::string::npos) << err;
}

TEST(XpuLaunchSubmission, aClosedWindowIsRejectedWithItsReason) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(runEntry(*jit, "rejected"), 0);
}

TEST(XpuLaunchSubmission, theSubmissionCrossesAFiberBoundary) {
    auto jit = compileProgram();
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(runEntry(*jit, "acrossFiber"), 0);
}
