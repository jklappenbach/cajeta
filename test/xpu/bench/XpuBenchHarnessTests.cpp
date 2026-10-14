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
#ifndef _WIN32
#include <signal.h>     // kill(): glibc leaks it via <sys/wait.h>, Apple libc does not
#include <sys/wait.h>   // POSIX process-wait; absent on mingw
#include <unistd.h>
#endif

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
    // ABBA over 2 arms x 3 rounds must read 0 1 1 0 1 0 (encoded base-10).
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

// 0.1.2, 0.2.8 — A/B/B/A with rounds reflected in Thue-Morse order, balanced across arms.
TEST(XpuBenchHarness, armOrderIsAbba) {
    auto jit = compileHarness();
    ASSERT_NE(jit, nullptr);
    auto abba = jit->lookup<int (*)()>("abba");
    ASSERT_NE(abba, nullptr);
    EXPECT_EQ(abba(), 11010) << "expected 0 1 1 0 1 0 for two arms over three rounds (Thue-Morse rounds)";
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
#ifdef _WIN32
    // The live-gate probe forks/execs a real named process and reaps it with
    // waitpid — POSIX process primitives absent on mingw. POSIX behavior is
    // unchanged; Windows simply skips this one probe.
    GTEST_SKIP() << "fork/exec/waitpid live-gate probe is POSIX-only";
#else
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
#endif  // _WIN32
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

// ── Trial bands from the day-apart pair (xpu-tile-scheduling 0.3.3) ───────
//
// One run's five-block band is a few percent wide; runs minutes apart drift
// further (T-002: 9 of 43 KPIs `worse` with no change; T-003: 4 of 47). With
// `--bands=<rerun rows>` a KPI's band is the union of the two runs' bands and
// its n their sum: a pair of single-sample rows (frame p99) becomes banded by
// the two runs' spread, a drifting five-block KPI widens to that drift, and a
// KPI the rerun does not have keeps its own band (or stays `single`). Driven
// in memory through the report tool's own trial core, compiled from disk.
namespace {

const char* kTrialDriver =
    "package xpubenchreport;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "public class DriveT {\n"
    // the run a row belongs to: before 06, the day-apart pair 07, after 08
    "    static String when = \"2026-09-06\";\n"
    "    static #String row(String wl, String kpi, String unit, float64 med, float64 lo, float64 hi, int32 n) {\n"
    "        return \"{\\\"workload\\\":\\\"\" + wl + \"\\\",\\\"shape\\\":\\\"s\\\",\\\"kpi\\\":\\\"\" + kpi\n"
    "            + \"\\\",\\\"unit\\\":\\\"\" + unit + \"\\\",\\\"median\\\":\" + med + \",\\\"min\\\":\" + lo\n"
    "            + \",\\\"max\\\":\" + hi + \",\\\"p95\\\":0,\\\"n\\\":\" + n\n"
    "            + \",\\\"device\\\":\\\"d\\\",\\\"backend\\\":\\\"hip\\\",\\\"driver\\\":\\\"v\\\",\\\"compiler\\\":\\\"c\\\",\"\n"
    "            + \"\\\"powerMode\\\":\\\"auto\\\",\\\"date\\\":\\\"\" + DriveT.when + \"\\\",\\\"arm\\\":\\\"baseline\\\",\\\"note\\\":\\\"\\\"}\\n\";\n"
    "    }\n"
    "    static #ArrayList<Report.Row> rows(String json) {\n"
    "        ArrayList<Report.Row> list = heap ArrayList<Report.Row>();\n"
    "        int8[] bytes #= json.toBytes();\n"
    "        Report.load(list, bytes, (int32) bytes.count());\n"
    "        return list;\n"
    "    }\n"
    // before: a frame p99 (single), a five-block saxpy duration, a seam kernel_time (single)
    "    static #String before() {\n"
    "        DriveT.when = \"2026-09-06\";\n"
    "        return DriveT.row(\"frame\", \"frame_p99\", \"ms\", 5.184, 5.184, 5.184, 1)\n"
    "            + DriveT.row(\"kernel.saxpy\", \"duration_isolated\", \"us\", 44.71, 43.45, 57.74, 5)\n"
    "            + DriveT.row(\"seam\", \"kernel_time\", \"us\", 6.46, 6.46, 6.46, 1);\n"
    "    }\n"
    // the day-apart rerun of the same code: p99 4.937, saxpy drifted to 60 [58, 62]; no seam row
    "    static #String pair() {\n"
    "        DriveT.when = \"2026-09-07\";\n"
    "        return DriveT.row(\"frame\", \"frame_p99\", \"ms\", 4.937, 4.937, 4.937, 1)\n"
    "            + DriveT.row(\"kernel.saxpy\", \"duration_isolated\", \"us\", 60.0, 58.0, 62.0, 5);\n"
    "    }\n"
    "    static int32 run(String afterJson, boolean withBands) {\n"
    "        ArrayList<Report.Row> b #= DriveT.rows(DriveT.before());\n"
    "        ArrayList<Report.Row> a #= DriveT.rows(afterJson);\n"
    "        ArrayList<Report.Row> p #= DriveT.rows(withBands ? DriveT.pair() : \"\");\n"
    "        TrialResult r #= Report.trialRows(b, a, p, \"T-test\", \"u\", \"c\", \"\");\n"
    "        if (r.refused) { return -1; }\n"
    "        return r.worse * 1000 + r.single * 100 + r.widened * 10 + r.paired;\n"
    "    }\n"
    // the after rows, built as a fresh run (08) unless a case says otherwise
    "    static #String after(float64 p99) {\n"
    "        return DriveT.row(\"frame\", \"frame_p99\", \"ms\", p99, p99, p99, 1)\n"
    "            + DriveT.row(\"kernel.saxpy\", \"duration_isolated\", \"us\", 61.0, 60.0, 62.5, 5)\n"
    "            + DriveT.row(\"seam\", \"kernel_time\", \"us\", 7.0, 7.0, 7.0, 1);\n"
    "    }\n"
    // --bands handed the after run itself (same identity): refused, not 100% keep
    "    public static int32 bandsIsAfter() {\n"
    "        DriveT.when = \"2026-09-07\";\n"
    "        String a #= DriveT.after(5.1);\n"
    "        return DriveT.run(a, true);\n"
    "    }\n"
    // p99 5.1 inside [4.937, 5.184]; saxpy 61 inside the union [43.45, 62] though
    // outside one run's band; kernel_time 7.0 has no partner -> single.
    // Expect worse 0, single 1, widened 2, paired 3 -> 0123.
    "    public static int32 insideUnion() {\n"
    "        DriveT.when = \"2026-09-08\";\n"
    "        String a #= DriveT.after(5.1);\n"
    "        return DriveT.run(a, true);\n"
    "    }\n"
    // p99 5.5 past the pair's spread, slower: worse 1 -> 1123.
    "    public static int32 outsideUnion() {\n"
    "        DriveT.when = \"2026-09-08\";\n"
    "        String a #= DriveT.after(5.5);\n"
    "        return DriveT.run(a, true);\n"
    "    }\n"
    // the same after rows without --bands: p99 and kernel_time single, saxpy 61 is
    // outside [43.45, 57.74] -> worse 1, single 2, widened 0, paired 3 -> 1203.
    "    public static int32 noBands() {\n"
    "        DriveT.when = \"2026-09-08\";\n"
    "        String a #= DriveT.after(5.1);\n"
    "        return DriveT.run(a, false);\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileTrial() {
    std::map<std::string, std::string> sources;
    std::string dir = here() + "/../../../tools/xpubench-report/src/xpubenchreport/";
    sources["xpubenchreport.Report"] = readFile(dir + "Report.cajeta");
    sources["xpubenchreport.Verdict"] = readFile(dir + "Verdict.cajeta");
    sources["xpubenchreport.Span"] = readFile(dir + "Span.cajeta");
    sources["xpubenchreport.Derived"] = readFile(dir + "Derived.cajeta");
    sources["xpubenchreport.Spans"] = readFile(dir + "Spans.cajeta");
    sources["xpubenchreport.TrialResult"] = readFile(dir + "TrialResult.cajeta");
    sources["xpubenchreport.DriveT"] = kTrialDriver;
    return CajetaJit::compile(sources, "xpubenchreport.DriveT", cpuOptions());
}

} // namespace

// 0.3.3 — the union of two runs' bands gates; a partnerless single stays single.
TEST(XpuBenchHarness, trialBandsFromDayApartPair) {
    auto jit = compileTrial();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("insideUnion"), 123) << "worse.single.widened.paired: expected 0/1/2/3";
    EXPECT_EQ(call("outsideUnion"), 1123) << "a p99 past the pair's spread must gate";
    EXPECT_EQ(call("noBands"), 1203) << "without --bands the single-run rule stands";
    EXPECT_EQ(call("bandsIsAfter"), -1)
        << "--bands with the after run's own identity must be refused, not read as all keep";
}

// ── Contention-proof trials (xpu-tile-scheduling 0.1.3 to 0.1.5) ─────────
//
// Report §4 T-006: a scheduled leg's profiled CG pass read +27% while the
// bypass legs either side agreed to 0.2%. The cause was CPU memory traffic
// on the shared APU memory system (saxpy +42%, stencil5 +55% beside a CPU
// stream), which the name-based gate cannot see. Three defences, each
// driven through the shipping sources: arms interleave inside one process
// and an arm's rounds merge into one banded row; a profiled pass repeats
// and its spans are banded; and a witness of foreign CPU and GPU busy turns
// a `worse` taken under contention into `contended`.
namespace {

const char* kArmDriver =
    "package xpubench;\n"
    "import cajeta.xpu.Scheduler;\n"
    "public class DriveArm {\n"
    "    public static int32 toggles() {\n"
    "        Arm.apply(\"bypass\");\n"
    "        if (!Scheduler.bypassed()) { return 1; }\n"
    "        Arm.apply(\"scheduled\");\n"
    "        if (Scheduler.bypassed()) { return 2; }\n"
    "        Arm.apply(\"baseline\");\n"
    "        if (Scheduler.bypassed()) { return 3; }\n"
    "        return 0;\n"
    "    }\n"
    "    public static int32 childEnv() {\n"
    "        String[] b #= Arm.envPrefix(\"bypass\");\n"
    "        if (b.count() != 2 || !b[0].equals(\"env\") || !b[1].equals(\"CAJETA_XPU_SCHEDULER=bypass\")) { return 1; }\n"
    "        String[] s #= Arm.envPrefix(\"scheduled\");\n"
    "        if (s.count() != 3 || !s[1].equals(\"-u\") || !s[2].equals(\"CAJETA_XPU_SCHEDULER\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"
    "    public static int32 twoArmsFourRounds() {\n"
    "        if (Stats.armRounds(1) != 1) { return 1; }\n"
    "        if (Stats.armRounds(2) != 4) { return 2; }\n"
    "        int32[] o #= Stats.abbaOrder(2, Stats.armRounds(2));\n"
    "        int32 code = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) o.count()) { code = code * 10 + o[i]; i = i + 1; }\n"
    "        return code;\n"
    "    }\n"
    "    public static int32 statTicks() {\n"
    "        int64[] t #= Gate.statTicks(\"cpu  100 0 50 1000 10 0 5 0 0 0\");\n"
    "        if (t[0] != 155L) { return 1; }\n"
    "        if (t[1] != 1165L) { return 2; }\n"
    "        return 0;\n"
    "    }\n"
    "    public static int32 procTicks() {\n"
    "        return (int32) Gate.procTicks(\"1234 (x y) S 1 2 3 4 5 6 7 8 9 10 70 30 0 0 20 0 1 0 5\");\n"
    "    }\n"
    "    public static int32 engineNs() {\n"
    "        return (int32) Gate.engineNs(\"drm-client-id:\\t60\\ndrm-engine-gfx:\\t1000 ns\\n\"\n"
    "            + \"drm-engine-compute:\\t234 ns\\ndrm-engine-dma:\\t5 ns\\n\");\n"
    "    }\n"
    "    public static int32 foreignCpu() {\n"
    "        return (int32) (Gate.foreignCpuPct(100L, 1000L, 10L, 300L, 2000L, 60L) * 10.0);\n"
    "    }\n"
    "    public static int32 contended() {\n"
    "        int32 r = 0;\n"
    "        if (Gate.contended(15.0, 0.0, 10.0, 5.0)) { r = r + 1; }\n"
    "        if (Gate.contended(5.0, 6.0, 10.0, 5.0)) { r = r + 10; }\n"
    "        if (Gate.contended(5.0, 1.0, 10.0, 5.0)) { r = r + 100; }\n"
    "        return r;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileArm() {
    std::map<std::string, std::string> sources;
    sources["xpubench.Stats"] = readFile(here() + "/src/xpubench/Stats.cajeta");
    sources["xpubench.Gate"] = readFile(here() + "/src/xpubench/Gate.cajeta");
    sources["xpubench.Arm"] = readFile(here() + "/src/xpubench/Arm.cajeta");
    sources["xpubench.DriveArm"] = kArmDriver;
    return CajetaJit::compile(sources, "xpubench.DriveArm", cpuOptions());
}

const char* kArmTrialDriver =
    "package xpubenchreport;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "public class DriveAT {\n"
    "    static #String row(String arm, String wl, String shape, String kpi, String unit,\n"
    "                       float64 med, float64 lo, float64 hi, int32 n) {\n"
    "        return \"{\\\"workload\\\":\\\"\" + wl + \"\\\",\\\"shape\\\":\\\"\" + shape + \"\\\",\\\"kpi\\\":\\\"\" + kpi\n"
    "            + \"\\\",\\\"unit\\\":\\\"\" + unit + \"\\\",\\\"median\\\":\" + med + \",\\\"min\\\":\" + lo\n"
    "            + \",\\\"max\\\":\" + hi + \",\\\"p95\\\":0,\\\"n\\\":\" + n\n"
    "            + \",\\\"device\\\":\\\"d\\\",\\\"backend\\\":\\\"hip\\\",\\\"driver\\\":\\\"v\\\",\\\"compiler\\\":\\\"c\\\",\"\n"
    "            + \"\\\"powerMode\\\":\\\"auto\\\",\\\"date\\\":\\\"2026-10-13\\\",\\\"arm\\\":\\\"\" + arm + \"\\\",\\\"note\\\":\\\"\\\"}\\n\";\n"
    "    }\n"
    "    static #ArrayList<Report.Row> rows(String json) {\n"
    "        ArrayList<Report.Row> list = heap ArrayList<Report.Row>();\n"
    "        int8[] bytes #= json.toBytes();\n"
    "        Report.load(list, bytes, (int32) bytes.count());\n"
    "        return list;\n"
    "    }\n"
    "    static int32 trial(String json) {\n"
    "        ArrayList<Report.Row> all #= DriveAT.rows(json);\n"
    "        ArrayList<Report.Row> b #= Report.armRows(all, \"bypass\");\n"
    "        ArrayList<Report.Row> a #= Report.armRows(all, \"scheduled\");\n"
    "        ArrayList<Report.Row> none = heap ArrayList<Report.Row>();\n"
    "        TrialResult r #= Report.trialRows(b, a, none, \"T-test\", \"u\", \"c\", \"\");\n"
    "        return r.contended * 10000 + r.worse * 1000 + r.single * 100 + r.widened * 10 + r.paired;\n"
    "    }\n"
    // ABBA in one file: bypass 10.0 then 10.5, scheduled 10.5 then 11.0.
    "    static #String abba() {\n"
    "        return DriveAT.row(\"bypass\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 10.0, 9.5, 10.5, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 10.5, 10.25, 10.75, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 11.0, 10.75, 11.25, 5)\n"
    "            + DriveAT.row(\"bypass\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 10.5, 10.0, 10.75, 5);\n"
    "    }\n"
    // merged: bypass 10.25 [9.5, 10.75] n 10; scheduled 10.75 inside it -> keep.
    // The last round alone (10.5 [10.0, 10.75] against 11.0) would read worse.
    "    public static int32 mergedRounds() {\n"
    "        String j #= DriveAT.abba();\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    "    public static int32 mergedBand() {\n"
    "        String j #= DriveAT.abba();\n"
    "        ArrayList<Report.Row> all #= DriveAT.rows(j);\n"
    "        ArrayList<Report.Row> b #= Report.armRows(all, \"bypass\");\n"
    "        if (b.count() != 1) { return 1; }\n"
    "        Report.Row m = b.get(0);\n"
    "        if (m.median != 10.25) { return 2; }\n"
    "        if (m.lo != 9.5 || m.hi != 10.75) { return 3; }\n"
    "        if (m.n != 10L) { return 4; }\n"
    "        return 0;\n"
    "    }\n"
    // after reads 12.0 against [9.5, 10.5]; the after arm's kernels block saw 40% foreign CPU.
    "    static #String regressed(float64 witness) {\n"
    "        return DriveAT.row(\"bypass\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 10.0, 9.5, 10.5, 5)\n"
    "            + DriveAT.row(\"bypass\", \"witness\", \"kernels\", \"foreign_cpu_pct\", \"witness\", 1.0, 1.0, 1.0, 1)\n"
    "            + DriveAT.row(\"scheduled\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 12.0, 11.9, 12.1, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"witness\", \"kernels\", \"foreign_cpu_pct\", \"witness\", witness, witness, witness, 1);\n"
    "    }\n"
    "    public static int32 contendedWorse() {\n"
    "        String j #= DriveAT.regressed(40.0);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    "    public static int32 quietWorse() {\n"
    "        String j #= DriveAT.regressed(2.0);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    // 0.2.8: rounds of one KPI, bypass then scheduled values in slot order B S S B (B S S B ...).
    "    static #String rounds(String kpi, String unit, float64[] bv, float64[] sv, int32 n) {\n"
    "        String j = \"\";\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) bv.count()) {\n"
    "            j = j + DriveAT.row(\"bypass\", \"seam\", \"50us\", kpi, unit, bv[i], bv[i] - 0.01, bv[i] + 0.01, n);\n"
    "            j = j + DriveAT.row(\"scheduled\", \"seam\", \"50us\", kpi, unit, sv[i], sv[i] - 0.01, sv[i] + 0.01, n);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return j;\n"
    "    }\n"
    "    static int32 armTrial(String kpi, String unit, float64 b0, float64 b1, float64 s0, float64 s1, int32 n) {\n"
    "        float64[] bv = heap float64[2];\n"
    "        float64[] sv = heap float64[2];\n"
    "        bv[0] = b0; bv[1] = b1; sv[0] = s0; sv[1] = s1;\n"
    "        String j #= DriveAT.rounds(kpi, unit, bv, sv, n);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    // the 0.3.4 cpu seam 50us launch_call: B1 is the outlier, B2 sits with the scheduled rounds
    "    public static int32 firstBlockStep() {\n"
    "        return DriveAT.armTrial(\"launch_call\", \"us\", 45.345999, 50.835999, 50.835999, 51.036, 5);\n"
    "    }\n"
    // the 0.3.4 cpu seam 5us kernel_time: one sample per round, one scheduled outlier
    "    public static int32 oneOutlierRound() {\n"
    "        return DriveAT.armTrial(\"kernel_time\", \"us\", 6.25, 6.429999, 6.549999, 9.769999, 1);\n"
    "    }\n"
    "    public static int32 fourRoundsPast() {\n"
    "        float64[] bv = heap float64[4];\n"
    "        float64[] sv = heap float64[4];\n"
    "        bv[0] = 100.0; bv[1] = 101.0; bv[2] = 100.5; bv[3] = 101.5;\n"
    "        sv[0] = 106.0; sv[1] = 106.5; sv[2] = 105.5; sv[3] = 107.0;\n"
    "        String j #= DriveAT.rounds(\"launch_call\", \"us\", bv, sv, 5);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    // the 0.3.4 cpu reduceSum 1M: two rounds separate, inside a wide block band
    "    public static int32 twoRoundsInsideBand() {\n"
    "        String j #= DriveAT.row(\"bypass\", \"seam\", \"50us\", \"launch_call\", \"us\", 240.63, 220.12, 283.52, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"seam\", \"50us\", \"launch_call\", \"us\", 264.11, 236.44, 295.45, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"seam\", \"50us\", \"launch_call\", \"us\", 267.69, 262.06, 293.8, 5)\n"
    "            + DriveAT.row(\"bypass\", \"seam\", \"50us\", \"launch_call\", \"us\", 249.62, 236.58, 263.69, 5);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    // four rounds a step apart inside a wide block band: the rounds alone decide
    "    public static int32 fourRoundsInsideBand() {\n"
    "        String j = \"\";\n"
    "        int32 i = 0;\n"
    "        while (i < 4) {\n"
    "            float64 d = (float64) i * 0.001;\n"
    "            j = j + DriveAT.row(\"bypass\", \"cg\", \"s\", \"host_cost_per_node\", \"us\", 0.855 + d, 0.849, 1.035, 5)\n"
    "                + DriveAT.row(\"scheduled\", \"cg\", \"s\", \"host_cost_per_node\", \"us\", 0.904 + d, 0.894, 0.975, 5);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    "    public static int32 orderedInsideSpread() {\n"
    "        return DriveAT.armTrial(\"launch_call\", \"us\", 100.0, 110.0, 111.0, 112.0, 5);\n"
    "    }\n"
    "    public static int32 rateDropped() {\n"
    "        return DriveAT.armTrial(\"achieved_bandwidth\", \"GB/s\", 100.0, 101.0, 90.0, 91.0, 5);\n"
    "    }\n"
    // 0.2.11: one span row per arm carrying its five passes as rounds (cpu saxpy 1M of 0.3.4)
    "    static #String spanRow(String arm, float64 med, float64 lo, float64 hi, float64 mean) {\n"
    "        return \"{\\\"workload\\\":\\\"kernel.saxpy\\\",\\\"shape\\\":\\\"1048576\\\",\\\"kpi\\\":\\\"device_span\\\",\"\n"
    "            + \"\\\"unit\\\":\\\"us\\\",\\\"median\\\":\" + med + \",\\\"min\\\":\" + lo + \",\\\"max\\\":\" + hi\n"
    "            + \",\\\"p95\\\":0,\\\"n\\\":5,\\\"rounds\\\":5,\\\"roundLo\\\":\" + lo + \",\\\"roundHi\\\":\" + hi\n"
    "            + \",\\\"roundMean\\\":\" + mean + \",\\\"device\\\":\\\"d\\\",\\\"backend\\\":\\\"cpu\\\",\\\"driver\\\":\\\"v\\\",\"\n"
    "            + \"\\\"compiler\\\":\\\"c\\\",\\\"powerMode\\\":\\\"auto\\\",\\\"date\\\":\\\"2026-10-13\\\",\\\"arm\\\":\\\"\" + arm\n"
    "            + \"\\\",\\\"note\\\":\\\"\\\"}\\n\";\n"
    "    }\n"
    "    public static int32 passesAsRounds() {\n"
    "        String j = DriveAT.spanRow(\"bypass\", 54.07, 52.97, 55.68, 54.36)\n"
    "            + DriveAT.spanRow(\"scheduled\", 55.79, 54.92, 64.14, 57.69);\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    // 0.2.9: after reads 12.0 against [9.5, 10.5]; the gate's idle GPU level, then the block's
    "    static int32 gpuRegressed(float64 idle, float64 block, boolean gate) {\n"
    "        String j = DriveAT.row(\"bypass\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 10.0, 9.5, 10.5, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"kernel.saxpy\", \"s\", \"duration_isolated\", \"us\", 12.0, 11.9, 12.1, 5)\n"
    "            + DriveAT.row(\"scheduled\", \"witness\", \"kernels\", \"foreign_gpu_pct\", \"witness\", block, block, block, 1);\n"
    "        if (gate) {\n"
    "            j = j + DriveAT.row(\"bypass\", \"witness\", \"gate\", \"foreign_gpu_pct\", \"witness\", idle, idle, idle, 1)\n"
    "                + DriveAT.row(\"scheduled\", \"witness\", \"gate\", \"foreign_gpu_pct\", \"witness\", idle, idle, idle, 1);\n"
    "        }\n"
    "        return DriveAT.trial(j);\n"
    "    }\n"
    "    public static int32 desktopLevelBlock() {\n"
    "        return DriveAT.gpuRegressed(9.0, 10.0, true);\n"
    "    }\n"
    "    public static int32 loadAboveDesktop() {\n"
    "        return DriveAT.gpuRegressed(9.0, 20.0, true);\n"
    "    }\n"
    "    public static int32 noGateLevel() {\n"
    "        return DriveAT.gpuRegressed(0.0, 10.0, false);\n"
    "    }\n"
    // queue_empty_time is device idle: less is better, though its unit is a percentage
    "    public static int32 queueEmptyLowerIsBetter() {\n"
    "        String j #= DriveAT.row(\"bypass\", \"cg\", \"s\", \"queue_empty_time\", \"%\", 15.0, 15.0, 15.0, 1);\n"
    "        ArrayList<Report.Row> r #= DriveAT.rows(j);\n"
    "        if (Report.lowerIsBetter(r.get(0))) { return 0; }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

const char* kRepeatDriver =
    "package xpubenchreport;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.lang.String;\n"
    "public class DriveR {\n"
    "    static #String csv(int32 avgNs) {\n"
    "        return \"# gpu_records_kept=1800 gpu_records_dropped=0\\n\"\n"
    "            + \"name,count,total_ns,self_ns,avg_ns,max_ns\\n\"\n"
    "            + \"saxpy,1800,\" + (1800 * avgNs) + \",\" + (1800 * avgNs) + \",\" + avgNs + \",\" + avgNs + \"\\n\";\n"
    "    }\n"
    // two passes, saxpy 20 us and 30 us: one row, median 25, band [20, 30], n 2
    "    public static int32 repeated() {\n"
    "        ArrayList<Spans> passes = heap ArrayList<Spans>();\n"
    "        String a #= DriveR.csv(20000);\n"
    "        String b #= DriveR.csv(30000);\n"
    "        passes.add(#Spans.parse(a));\n"
    "        passes.add(#Spans.parse(b));\n"
    "        float64[] walls = heap float64[2];\n"
    "        ArrayList<Derived> d #= Spans.deriveRepeated(passes, \"kernel.saxpy\", \"1048576\", \"saxpy\",\n"
    "                                                     (int64) 1800, walls);\n"
    "        if (d.count() != 1) { return 1; }\n"
    "        Derived r = d.get(0);\n"
    "        if (!r.kpi.equals(\"device_span\")) { return 2; }\n"
    "        if (r.value != 25.0) { return 3; }\n"
    "        if (r.lo != 20.0 || r.hi != 30.0) { return 4; }\n"
    "        if (r.n != 2L) { return 5; }\n"
    "        if (r.mean != 25.0) { return 6; }\n"
    "        return 0;\n"
    "    }\n"
    // 0.2.11: a 90 us device time over walls of 100 and 200 us: queue empty 10% and 55%
    "    public static int32 wallPerPass() {\n"
    "        ArrayList<Spans> passes = heap ArrayList<Spans>();\n"
    "        String a #= DriveR.csv(90000);\n"
    "        String b #= DriveR.csv(90000);\n"
    "        passes.add(#Spans.parse(a));\n"
    "        passes.add(#Spans.parse(b));\n"
    "        float64[] walls = heap float64[2];\n"
    "        walls[0] = 100.0; walls[1] = 200.0;\n"
    "        ArrayList<Derived> d #= Spans.deriveRepeated(passes, \"cg\", \"s\", \"saxpy\", (int64) 1800, walls);\n"
    "        int32 i = 0;\n"
    "        while (i < d.count()) {\n"
    "            Derived r = d.get(i);\n"
    "            i = i + 1;\n"
    "            if (r.kpi.equals(\"queue_empty_time\")) {\n"
    "                if (r.lo != 10.0 || r.hi != 55.0) { return 2; }\n"
    "                return 0;\n"
    "            }\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> compileReport(const char* driverName, const char* driver) {
    std::map<std::string, std::string> sources;
    std::string dir = here() + "/../../../tools/xpubench-report/src/xpubenchreport/";
    sources["xpubenchreport.Report"] = readFile(dir + "Report.cajeta");
    sources["xpubenchreport.Verdict"] = readFile(dir + "Verdict.cajeta");
    sources["xpubenchreport.Span"] = readFile(dir + "Span.cajeta");
    sources["xpubenchreport.Derived"] = readFile(dir + "Derived.cajeta");
    sources["xpubenchreport.Spans"] = readFile(dir + "Spans.cajeta");
    sources["xpubenchreport.TrialResult"] = readFile(dir + "TrialResult.cajeta");
    sources[std::string("xpubenchreport.") + driverName] = driver;
    return CajetaJit::compile(sources, std::string("xpubenchreport.") + driverName, cpuOptions());
}

} // namespace

// 0.1.3 — the arm reaches the scheduler and the llm child; two arms run ABBA.
TEST(XpuBenchHarness, anArmTogglesTheSchedulerAndTheChild) {
    auto jit = compileArm();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("toggles"), 0);
    EXPECT_EQ(call("childEnv"), 0);
    EXPECT_EQ(call("twoArmsFourRounds"), 1101001) << "two arms, four rounds: 0 1 1 0 1 0 0 1";
}

// 0.1.3 — an arm's rounds merge into one banded row, and the merge decides.
TEST(XpuBenchHarness, armRoundsMergeIntoOneBandedRow) {
    auto jit = compileReport("DriveAT", kArmTrialDriver);
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("mergedBand"), 0);
    EXPECT_EQ(call("mergedRounds"), 1) << "contended.worse.single.widened.paired: 0/0/0/0/1";
}

// 0.1.5 — a regression taken under foreign load reads contended, not worse.
TEST(XpuBenchHarness, aWorseUnderForeignLoadReadsContended) {
    auto jit = compileReport("DriveAT", kArmTrialDriver);
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("contendedWorse"), 10001) << "40% foreign CPU in the after block";
    EXPECT_EQ(call("quietWorse"), 1001) << "a quiet box keeps the worse verdict";
    EXPECT_EQ(call("queueEmptyLowerIsBetter"), 0) << "a busier device must not read as a regression";
}

// 0.2.8 — an arm trial judges the arm effect against the round-to-round spread.
TEST(XpuBenchHarness, anArmTrialJudgesTheArmEffect) {
    auto jit = compileReport("DriveAT", kArmTrialDriver);
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("firstBlockStep"), 1) << "B 45.3, 50.8 and S 50.8, 51.0: a first-block step, not the arm";
    EXPECT_EQ(call("oneOutlierRound"), 1) << "one-sample rounds are judged by rounds, and one outlier keeps";
    EXPECT_EQ(call("fourRoundsPast"), 1001) << "every scheduled round 5% past every bypass round";
    EXPECT_EQ(call("orderedInsideSpread"), 1) << "ordered, but the gap is inside the bypass spread";
    EXPECT_EQ(call("rateDropped"), 1001) << "a rate is worse downward";
    EXPECT_EQ(call("twoRoundsInsideBand"), 1) << "two rounds separate by chance one time in six: the band must agree";
    EXPECT_EQ(call("fourRoundsInsideBand"), 1001) << "four separated rounds decide alone, past a wide block band";
    EXPECT_EQ(call("passesAsRounds"), 1) << "five overlapping profiled passes per arm are rounds, not one band (0.2.11)";
}

// 0.2.9 — the GPU witness is judged above the desktop's idle level.
TEST(XpuBenchHarness, theGpuWitnessIsJudgedAboveTheIdleLevel) {
    auto jit = compileReport("DriveAT", kArmTrialDriver);
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("desktopLevelBlock"), 1001) << "a 10% block over a 9% desktop is not contention";
    EXPECT_EQ(call("loadAboveDesktop"), 10001) << "a 20% block over a 9% desktop is";
    EXPECT_EQ(call("noGateLevel"), 10001) << "rows without a gate level judge against zero";
}

// 0.1.4 — repeated profiled passes give a banded device span.
TEST(XpuBenchHarness, repeatedProfiledPassesBandTheSpan) {
    auto jit = compileReport("DriveR", kRepeatDriver);
    ASSERT_NE(jit, nullptr);
    auto f = jit->lookup<int (*)()>("repeated");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f(), 0);
    auto w = jit->lookup<int (*)()>("wallPerPass");
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(w(), 0) << "each pass divides by its own wall (0.2.11)";
}

// 0.1.5 — the witness's parsers and threshold.
TEST(XpuBenchHarness, contentionWitnessParsers) {
    auto jit = compileArm();
    ASSERT_NE(jit, nullptr);
    auto call = [&](const char* name) {
        auto f = jit->lookup<int (*)()>(name);
        return f ? f() : -1;
    };
    EXPECT_EQ(call("statTicks"), 0);
    EXPECT_EQ(call("procTicks"), 100) << "utime 70 + stime 30, after a comm with a space";
    EXPECT_EQ(call("engineNs"), 1234) << "gfx + compute, not dma";
    EXPECT_EQ(call("foreignCpu"), 150) << "(200 busy - 50 own) / 1000 total";
    EXPECT_EQ(call("contended"), 11) << "over on CPU, over on GPU, under both";
}
