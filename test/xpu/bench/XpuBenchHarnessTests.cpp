// The baseline harness's discipline (xpu-tile-scheduling plan 0.1.1, 0.1.2):
//
//   * the idle gate REFUSES beside another cajeta_test / bench / GPU client,
//     and does NOT refuse on a clean process table or on its own process;
//   * the arm order is A/B/B/A, so a decaying background load lands on every
//     arm equally (a fixed order faked a speedup once);
//   * the noise band is min/max over blocks, the median the lower middle,
//     p95 nearest-rank — the numbers every report row is built from.
//
// The harness's own Stats and Gate sources are compiled from disk through the
// multi-source JIT, so the test exercises the code the harness ships, not a
// copy of it. CPU backend, no device needed. The live-gate test spawns a real
// process whose NAME matches a gate pattern (a copy of /bin/sleep), so the
// "fires" and "does not fire" halves both run against the real process table.
#include "gtest/gtest.h"
#include "../../jit/JitTestHelper.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using cajeta_test::CajetaJit;

namespace {

std::string here() {
    std::string f = __FILE__;
    return f.substr(0, f.find_last_of('/'));
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// A driver class beside the harness's own Stats/Gate sources. Each entry
// returns an int so the test needs no marshalling of cajeta values.
const char* kDriverSource =
    "package xpubench;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "public class Drive {\n"
    // ABBA over 2 arms x 3 rounds must read 0 1 1 0 0 1 (encoded base-10).
    "    public static int32 abba() {\n"
    "        int32[] o #= Stats.abbaOrder(2, 3);\n"
    "        int32 code = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) o.count()) { code = code * 10 + o[i]; i = i + 1; }\n"
    "        return code;\n"
    "    }\n"
    // Every arm gets the same number of slots under ABBA (3 arms x 4 rounds).
    "    public static int32 abbaBalanced() {\n"
    "        int32[] o #= Stats.abbaOrder(3, 4);\n"
    "        int32 a = 0; int32 b = 0; int32 c = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) o.count()) {\n"
    "            if (o[i] == 0) { a = a + 1; } else if (o[i] == 1) { b = b + 1; } else { c = c + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        if (a != 4 || b != 4 || c != 4) { return 1; }\n"
    "        return 0;\n"
    "    }\n"
    // median / band / p95 of {9, 1, 5, 3, 7}: median 5, min 1, max 9, p95 9.
    "    public static int32 band() {\n"
    "        float64[] a = heap float64[5];\n"
    "        a[0] = 9.0; a[1] = 1.0; a[2] = 5.0; a[3] = 3.0; a[4] = 7.0;\n"
    "        if (Stats.median(a, 5) != 5.0) { return 1; }\n"
    "        if (Stats.min(a, 5) != 1.0) { return 2; }\n"
    "        if (Stats.max(a, 5) != 9.0) { return 3; }\n"
    "        if (Stats.percentile(a, 5, 95) != 9.0) { return 4; }\n"
    "        if (Stats.percentile(a, 5, 50) != 5.0) { return 5; }\n"
    "        if (a[0] != 9.0) { return 6; }\n"      // the caller's array is untouched
    "        return 0;\n"
    "    }\n"
    // Even n takes the lower middle: {4, 2} -> 2.
    "    public static int32 medianEven() {\n"
    "        float64[] a = heap float64[2];\n"
    "        a[0] = 4.0; a[1] = 2.0;\n"
    "        return (int32) Stats.median(a, 2);\n"
    "    }\n"
    // parseFloat round-trips the shapes the harness prints.
    "    public static int32 parse() {\n"
    "        if (Stats.parseFloat(\"12.5\") != 12.5) { return 1; }\n"
    "        if (Stats.parseFloat(\"-3\") != -3.0) { return 2; }\n"
    "        if (Stats.parseFloat(\"1e3\") != 1000.0) { return 3; }\n"
    "        if (Stats.parseFloat(\"2.5e-1\") != 0.25) { return 4; }\n"
    "        return 0;\n"
    "    }\n"
    // Pure gate: a competitor is reported, self and the pgrep probe are not.
    "    public static int32 gateFires() {\n"
    "        ArrayList<String> lines = heap ArrayList<String>();\n"
    "        lines.add(\"100 /usr/bin/zsh\");\n"
    "        lines.add(\"200 cajeta_test --gtest_filter=Xpu*\");\n"
    "        lines.add(\"300 pgrep -a cajeta_test\");\n"
    "        ArrayList<String> hits #= Gate.offenders(lines, 999);\n"
    "        return hits.count();\n"
    "    }\n"
    "    public static int32 gateQuietOnClean() {\n"
    "        ArrayList<String> lines = heap ArrayList<String>();\n"
    "        lines.add(\"100 /usr/bin/zsh\");\n"
    "        lines.add(\"101 ninja -C build\");\n"
    "        ArrayList<String> hits #= Gate.offenders(lines, 999);\n"
    "        return hits.count();\n"
    "    }\n"
    "    public static int32 gateSkipsSelf() {\n"
    "        ArrayList<String> lines = heap ArrayList<String>();\n"
    "        lines.add(\"4242 xpubench --workloads=seam\");\n"
    "        ArrayList<String> hits #= Gate.offenders(lines, 4242);\n"
    "        return hits.count();\n"
    "    }\n"
    // Live gate: the real process table right now.
    "    public static int32 liveHits() {\n"
    "        ArrayList<String> hits #= Gate.liveOffenders();\n"
    "        return hits.count();\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileHarness() {
    std::map<std::string, std::string> sources;
    sources["xpubench.Stats"] = readFile(here() + "/src/xpubench/Stats.cajeta");
    sources["xpubench.Gate"] = readFile(here() + "/src/xpubench/Gate.cajeta");
    sources["xpubench.Drive"] = kDriverSource;
    return CajetaJit::compile(sources, "xpubench.Drive", cpuOptions());
}

} // namespace

// 0.1.2 — A/B/B/A: the reflected order, balanced across arms.
TEST(XpuBenchHarness, armOrderIsAbba) {
    auto jit = compileHarness();
    ASSERT_NE(jit, nullptr);
    auto abba = jit->lookup<int (*)()>("abba");
    ASSERT_NE(abba, nullptr);
    EXPECT_EQ(abba(), 11001) << "expected 0 1 1 0 0 1 for two arms over three rounds";
    auto balanced = jit->lookup<int (*)()>("abbaBalanced");
    ASSERT_NE(balanced, nullptr);
    EXPECT_EQ(balanced(), 0);
}

// 0.1.2 — the noise band is min/max, the median a real sample, p95 nearest-rank.
TEST(XpuBenchHarness, noiseBandAndMedian) {
    auto jit = compileHarness();
    ASSERT_NE(jit, nullptr);
    auto band = jit->lookup<int (*)()>("band");
    ASSERT_NE(band, nullptr);
    EXPECT_EQ(band(), 0);
    auto medianEven = jit->lookup<int (*)()>("medianEven");
    ASSERT_NE(medianEven, nullptr);
    EXPECT_EQ(medianEven(), 2);
    auto parse = jit->lookup<int (*)()>("parse");
    ASSERT_NE(parse, nullptr);
    EXPECT_EQ(parse(), 0);
}

// 0.1.1 — the gate's logic: fires on a competitor, not on a clean table, not
// on itself, never on the pgrep probe's own line.
TEST(XpuBenchHarness, idleGateLogic) {
    auto jit = compileHarness();
    ASSERT_NE(jit, nullptr);
    auto fires = jit->lookup<int (*)()>("gateFires");
    ASSERT_NE(fires, nullptr);
    EXPECT_EQ(fires(), 1) << "one cajeta_test line must be reported; zsh and pgrep must not";
    auto quiet = jit->lookup<int (*)()>("gateQuietOnClean");
    ASSERT_NE(quiet, nullptr);
    EXPECT_EQ(quiet(), 0);
    auto self = jit->lookup<int (*)()>("gateSkipsSelf");
    ASSERT_NE(self, nullptr);
    EXPECT_EQ(self(), 0) << "the harness must not refuse because of its own pid";
}

// 0.1.1 — the gate against the LIVE process table: a process whose name
// matches a pattern makes it refuse; once that process is gone it does not.
// The test binary itself is `cajeta_test`, so the harness's self-exclusion
// is what keeps the "quiet" half honest: this process is excluded by pid,
// and any OTHER cajeta_test (a parallel sweep) would make this test skip.
TEST(XpuBenchHarness, idleGateLive) {
    auto jit = compileHarness();
    ASSERT_NE(jit, nullptr);
    auto live = jit->lookup<int (*)()>("liveHits");
    ASSERT_NE(live, nullptr);

    if (live() != 0) {
        GTEST_SKIP() << "another cajeta_test / bench is live; the gate is already refusing, "
                        "which is the point, but the quiet half cannot be shown now";
    }

    // A real process named like a bench: a shell script whose basename the
    // kernel installs as the process name (comm). Not a copy of /bin/sleep —
    // on a multi-call coreutils that dispatches on argv[0] and exits at
    // once, and a zombie would still satisfy pgrep. The script keeps its
    // own name while `sleep` runs as its child.
    std::string dir = here() + "/../../../tmp";
    ::mkdir(dir.c_str(), 0755);
    std::string fake = dir + "/xpubench-gate-probe";
    {
        std::ofstream dst(fake);
        dst << "#!/bin/sh\nsleep 30\n";
    }
    ::chmod(fake.c_str(), 0755);
    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::execl(fake.c_str(), "xpubench-gate-probe", (char*) nullptr);
        _exit(127);
    }
    ::usleep(300000);
    int alive = ::kill(child, 0) == 0 ? 1 : 0;
    int during = live();
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    ::usleep(100000);
    int after = live();
    ::unlink(fake.c_str());

    ASSERT_EQ(alive, 1) << "the staged process must still be running when the gate is asked";
    EXPECT_GE(during, 1) << "a live process named xpubench-* must make the gate refuse";
    EXPECT_EQ(after, 0) << "with it gone the gate must not refuse (self is excluded by pid)";
}

// ── The trial verdict (report §1 "Verdict rule") ─────────────────────────
//
// `keep` inside the before row's band or better, `worse` outside it in the
// bad direction, and `single` when the before row has no band at all (n < 2).
// The first real trial (cpu-barrier-fission-loops 1.3.3, 2026-09-06) read
// twelve phantom regressions because derived and single-sample rows carried
// a zero-width band, so any delta at all was "worse". The report tool's own
// Verdict source is compiled from disk, like the harness sources above.
namespace {

const char* kVerdictDriver =
    "package xpubenchreport;\n"
    "public class DriveV {\n"
    // a duration (lower is better) with a five-block band [40, 50]
    "    public static int32 keepInside()  { return Verdict.of(true, 40.0, 50.0, (int64) 5, 45.0); }\n"
    "    public static int32 keepBetter()  { return Verdict.of(true, 40.0, 50.0, (int64) 5, 39.0); }\n"
    "    public static int32 worseSlower() { return Verdict.of(true, 40.0, 50.0, (int64) 5, 50.5); }\n"
    // a rate (higher is better)
    "    public static int32 rateBetter()  { return Verdict.of(false, 40.0, 50.0, (int64) 5, 51.0); }\n"
    "    public static int32 rateWorse()   { return Verdict.of(false, 40.0, 50.0, (int64) 5, 39.0); }\n"
    // a single-sample before row (a frame p99): zero-width band, any delta
    "    public static int32 singleUp()    { return Verdict.of(true, 5.184, 5.184, (int64) 1, 5.5); }\n"
    "    public static int32 singleDown()  { return Verdict.of(true, 5.184, 5.184, (int64) 1, 4.9); }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileVerdict() {
    std::map<std::string, std::string> sources;
    sources["xpubenchreport.Verdict"] =
        readFile(here() + "/../../../tools/xpubench-report/src/xpubenchreport/Verdict.cajeta");
    sources["xpubenchreport.DriveV"] = kVerdictDriver;
    return CajetaJit::compile(sources, "xpubenchreport.DriveV", cpuOptions());
}

} // namespace

// cpu-barrier-fission-loops 1.3.4 — keep / worse / single, both directions.
TEST(XpuBenchHarness, trialVerdictRule) {
    auto jit = compileVerdict();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("keepInside"), 0);
    EXPECT_EQ(call("keepBetter"), 0);
    EXPECT_EQ(call("worseSlower"), 1) << "outside the band, slower: the gate must fail";
    EXPECT_EQ(call("rateBetter"), 0);
    EXPECT_EQ(call("rateWorse"), 1) << "outside the band, lower rate: the gate must fail";
    EXPECT_EQ(call("singleUp"), 2) << "a zero-width band must not read a delta as a regression";
    EXPECT_EQ(call("singleDown"), 2) << "nor as an improvement: no band, no verdict";
}

// ── Device spans → rows (xpu-tile-scheduling 0.2.4) ──────────────────────
//
// A hand-written `cajeta profile summary --csv` stands in for a profiled
// pass. Each derivation is asserted on its number; a lossy ring (fewer
// records than the harness launched, or the ring's own drop count) turns
// the rows pending rather than wrong. The report tool's own Spans source
// is compiled from disk.
namespace {

const char* kSpansDriver =
    "package xpubenchreport;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "public class DriveS {\n"
    "    static String CSV = \"# gpu_records_kept=2733 gpu_records_dropped=0\\n\"\n"
    "        + \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "        + \"dot,111,11100000,11100000,100000,150000\\n\"\n"
    "        + \"finalSum2,111,555000,555000,5000,9000\\n\"\n"
    "        + \"stencil5,600,60000000,60000000,100000,120000\\n\"\n"
    "        + \"saxpy,1800,36000000,36000000,20000,30000\\n\";\n"
    "    static String LOSSY = \"# gpu_records_kept=8000 gpu_records_dropped=35874\\n\"\n"
    "        + \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "        + \"dot,111,11100000,11100000,100000,150000\\n\"\n"
    "        + \"finalSum2,111,555000,555000,5000,9000\\n\";\n"
    "    static String NORING = \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "        + \"dot,100,10000000,10000000,100000,150000\\n\"\n"
    "        + \"finalSum2,111,555000,555000,5000,9000\\n\";\n"
    // §3.5's kernels, totals in ns, avg in ns
    "    static String LLM = \"# gpu_records_kept=43874 gpu_records_dropped=0\\n\"\n"
    "        + \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "        + \"q4kWmmaKernel,3264,7904200000,7904200000,2421600,6194474\\n\"\n"
    "        + \"q6kWmmaEpiKernel,544,1470900000,1470900000,2703900,6883774\\n\"\n"
    "        + \"attnFlashPrefillGqa4Kernel,544,498800000,498800000,916900,2170330\\n\"\n"
    "        + \"q4kQ8WaveMatVecKernel,7168,871800000,871800000,121600,246425\\n\"\n"
    "        + \"attnFlashDecodeGqa4Kernel,2048,89200000,89200000,43555,90000\\n\"\n"
    "        + \"attnFlashDecodeReduceKernel,2048,37300000,37300000,18213,30000\\n\";\n"
    "    static int32 tenths(float64 v) { return (int32) (v * 10.0 + (v >= 0.0 ? 0.5 : -0.5)); }\n"
    // parse: four kernels, the ring line read
    "    public static int32 parsed() {\n"
    "        Spans s #= Spans.parse(DriveS.CSV);\n"
    "        if (s.count() != 4) { return 1; }\n"
    "        if (s.kept != (int64) 2733 || s.dropped != (int64) 0) { return 2; }\n"
    "        Span d = s.find(\"dot\");\n"
    "        if (d == null || d.count != (int64) 111 || d.avgNs != (int64) 100000) { return 3; }\n"
    "        if (s.find(\"gather\") != null) { return 4; }\n"
    "        Spans n #= Spans.parse(DriveS.NORING);\n"
    "        if (n.kept != (int64) -1 || n.dropped != (int64) -1) { return 5; }\n"
    "        return 0;\n"
    "    }\n"
    // dot = dot + finalSum2 = 105.0 us; cg = 100 + 2x100 + 5 + 3x20 = 365.0 us; absent -> -1
    "    public static int32 perIteration() {\n"
    "        Spans s #= Spans.parse(DriveS.CSV);\n"
    "        if (DriveS.tenths(s.perIterationUs(\"dot,finalSum2\")) != 1050) { return 1; }\n"
    "        if (DriveS.tenths(s.perIterationUs(\"stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy\")) != 3650) { return 2; }\n"
    "        if (s.perIterationUs(\"gather\") >= 0.0) { return 3; }\n"
    "        return 0;\n"
    "    }\n"
    // lossy: counts short of iterations x multiplicity, or the ring's drop count
    "    public static int32 lossy() {\n"
    "        Spans s #= Spans.parse(DriveS.CSV);\n"
    "        if (s.lossy(\"dot,finalSum2\", (int64) 111)) { return 1; }\n"
    "        if (!s.lossy(\"dot,finalSum2\", (int64) 112)) { return 2; }\n"
    "        if (s.lossy(\"stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy\", (int64) 55)) { return 3; }\n"
    "        if (!s.lossy(\"stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy\", (int64) 56)) { return 4; }\n"
    "        Spans l #= Spans.parse(DriveS.LOSSY);\n"
    "        if (!l.lossy(\"dot,finalSum2\", (int64) 111)) { return 5; }\n"
    "        Spans n #= Spans.parse(DriveS.NORING);\n"
    "        if (!n.lossy(\"dot,finalSum2\", (int64) 111)) { return 6; }\n"   // dot has 100 < 111
    "        return 0;\n"
    "    }\n"
    // cg rows: device_span 365.0, device_time_per_iteration 365.0, queue_empty 100 x (1 - 365/492.7) = 25.9
    "    public static int32 cgRows() {\n"
    "        Spans s #= Spans.parse(DriveS.CSV);\n"
    "        ArrayList<Derived> d #= Spans.derive(s, \"cg\", \"1024x1024x2000\",\n"
    "            \"stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy\", (int64) 55, 492.7);\n"
    "        if (d.count() != 3) { return 1; }\n"
    "        if (!d.get(0).kpi.equals(\"device_span\") || d.get(0).pending) { return 2; }\n"
    "        if (DriveS.tenths(d.get(0).value) != 3650) { return 3; }\n"
    "        if (!d.get(1).kpi.equals(\"device_time_per_iteration\") || DriveS.tenths(d.get(1).value) != 3650) { return 4; }\n"
    "        if (!d.get(2).kpi.equals(\"queue_empty_time\") || !d.get(2).unit.equals(\"%\")) { return 5; }\n"
    "        if (DriveS.tenths(d.get(2).value) != 259) { return 6; }\n"
    "        return 0;\n"
    "    }\n"
    // a kernel row: one KPI, no wall
    "    public static int32 kernelRow() {\n"
    "        Spans s #= Spans.parse(DriveS.CSV);\n"
    "        ArrayList<Derived> d #= Spans.derive(s, \"kernel.dot\", \"1048576\", \"dot,finalSum2\", (int64) 111, 0.0);\n"
    "        if (d.count() != 1) { return 1; }\n"
    "        if (d.get(0).pending || DriveS.tenths(d.get(0).value) != 1050) { return 2; }\n"
    "        if (!d.get(0).unit.equals(\"us\")) { return 3; }\n"
    "        return 0;\n"
    "    }\n"
    // a lossy pass: every row pending, the note says what was kept and launched
    "    public static int32 lossyRows() {\n"
    "        Spans l #= Spans.parse(DriveS.LOSSY);\n"
    "        ArrayList<Derived> d #= Spans.derive(l, \"cg\", \"1024x1024x2000\", \"dot,finalSum2\", (int64) 111, 492.7);\n"
    "        if (d.count() != 3) { return 1; }\n"
    "        int32 i = 0;\n"
    "        while (i < d.count()) {\n"
    "            if (!d.get(i).pending || !d.get(i).unit.equals(\"pending\")) { return 2; }\n"
    "            if (!d.get(i).note.contains(\"35874\")) { return 3; }\n"
    "            if (!d.get(i).note.contains(\"CAJETA_PROFILER_GPU_RING\")) { return 4; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return 0;\n"
    "    }\n"
    // llm: fraction 9375.1 / (10872.2 - 998.3) = 94.9%; attention 43.555 + 18.213 = 61.8 us;
    //      prefill attention 916.9 us; device_busy 10872.2 / 11802 = 92.1%
    "    public static int32 llmRows() {\n"
    "        Spans s #= Spans.parse(DriveS.LLM);\n"
    "        ArrayList<Derived> d #= Spans.deriveLlm(s, \"prompt2048+gen64\", 11802.0);\n"
    "        if (d.count() != 4) { return 1; }\n"
    "        if (!d.get(0).kpi.equals(\"matrix_core_fraction\") || d.get(0).pending) { return 2; }\n"
    "        if (DriveS.tenths(d.get(0).value) != 949) { return 3; }\n"
    "        if (!d.get(1).kpi.equals(\"device_busy\") || DriveS.tenths(d.get(1).value) != 921) { return 4; }\n"
    "        if (!d.get(2).kpi.equals(\"attention_kernel_duration\") || !d.get(2).workload.equals(\"llm.decode\")) { return 5; }\n"
    "        if (DriveS.tenths(d.get(2).value) != 618) { return 6; }\n"
    "        if (!d.get(3).workload.equals(\"llm.prefill\") || DriveS.tenths(d.get(3).value) != 9169) { return 7; }\n"
    "        return 0;\n"
    "    }\n"
    // llm on a lossy ring: the totals-based rows pending, the averages still stand
    "    public static int32 llmLossy() {\n"
    "        String csv = \"# gpu_records_kept=8000 gpu_records_dropped=35874\\n\"\n"
    "            + \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "            + \"q4kQ8WaveMatVecKernel,7168,871800000,871800000,121600,246425\\n\"\n"
    "            + \"attnFlashDecodeGqa4Kernel,832,36200000,36200000,43555,90000\\n\";\n"
    "        Spans s #= Spans.parse(csv);\n"
    "        ArrayList<Derived> d #= Spans.deriveLlm(s, \"prompt2048+gen64\", 11802.0);\n"
    "        if (d.count() != 4) { return 1; }\n"
    "        if (!d.get(0).pending || !d.get(0).note.contains(\"35874\")) { return 2; }\n"
    "        if (!d.get(1).pending) { return 3; }\n"
    "        if (d.get(2).pending || DriveS.tenths(d.get(2).value) != 436) { return 4; }\n"
    "        if (!d.get(3).pending) { return 5; }\n"   // prefill attention lost with the head of the ring
    "        return 0;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileSpans() {
    std::map<std::string, std::string> sources;
    std::string dir = here() + "/../../../tools/xpubench-report/src/xpubenchreport/";
    sources["xpubenchreport.Span"] = readFile(dir + "Span.cajeta");
    sources["xpubenchreport.Derived"] = readFile(dir + "Derived.cajeta");
    sources["xpubenchreport.Spans"] = readFile(dir + "Spans.cajeta");
    sources["xpubenchreport.DriveS"] = kSpansDriver;
    return CajetaJit::compile(sources, "xpubenchreport.DriveS", cpuOptions());
}

} // namespace

// 0.2.4 — each derived row asserted on its number.
TEST(XpuBenchHarness, deviceSpansDeriveRows) {
    auto jit = compileSpans();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("parsed"), 0);
    EXPECT_EQ(call("perIteration"), 0);
    EXPECT_EQ(call("cgRows"), 0);
    EXPECT_EQ(call("kernelRow"), 0);
    EXPECT_EQ(call("llmRows"), 0);
}

// 0.2.4 — a lossy ring marks the rows pending rather than wrong.
TEST(XpuBenchHarness, lossyRingMarksSpanRowsPending) {
    auto jit = compileSpans();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("lossy"), 0);
    EXPECT_EQ(call("lossyRows"), 0);
    EXPECT_EQ(call("llmLossy"), 0);
}
