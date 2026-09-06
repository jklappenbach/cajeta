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
