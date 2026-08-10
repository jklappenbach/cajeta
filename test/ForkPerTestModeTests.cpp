// Meta-tests for the fork-per-test prime server (compile-cache plan Unit 3F).
// Each drives THIS binary as a subprocess with CAJETA_FORK_PER_TEST=1 and
// asserts on its output — the mode is a process-level behavior, so the tests
// live at the process level too.

#if !defined(_WIN32)

#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

std::string selfExe() {
    // Resolved HERE, not left as the literal "/proc/self/exe" — inside the
    // popen'd shell that symlink names the SHELL, which would re-exec sh.
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return buf;
}

std::string runSelf(const std::string& envPrefix, const std::string& filter) {
    std::string cmd = envPrefix +
        " '" + selfExe() + "' --gtest_filter='" + filter + "' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "<popen failed>";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

int countOf(const std::string& hay, const std::string& needle) {
    int c = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
            at = hay.find(needle, at + needle.size())) {
        ++c;
    }
    return c;
}

}  // namespace

// The crash probe the isolation test forks: inert (skipped) unless the meta-
// test arms it via env, so ordinary sweeps never see a SIGSEGV from it.
TEST(ForkCrashProbe, segvOnDemand) {
    if (!std::getenv("CAJETA_FORK_PROBE_CRASH")) {
        GTEST_SKIP() << "armed only by ForkPerTestModeTests";
    }
    raise(SIGSEGV);
}

// 3F.1.1 — fork mode over a small JIT filter: same pass set as the serial
// reuse run, and exactly ONE prime.
TEST(ForkPerTestModeTests, matchesSerialAndPrimesOnce) {
    const std::string filter = "ZoneOffsetTests.*";
    std::string forked = runSelf(
        "CAJETA_FORK_PER_TEST=1 CAJETA_PRIME_TIMING=1", filter);
    std::string serial = runSelf(
        "CAJETA_STDLIB_REUSE=1 CAJETA_PRIME_TIMING=1", filter);

    EXPECT_EQ(countOf(forked, "[CAJETA_PRIME_TIMING]"), 1) << forked;
    // Every test the serial run passed, the forked run passed.
    int serialOk = countOf(serial, "[       OK ]");
    ASSERT_GT(serialOk, 0) << serial;
    EXPECT_EQ(countOf(forked, "[       OK ]"), serialOk) << forked;
    EXPECT_EQ(countOf(forked, "[  FAILED  ]"), 0) << forked;
}

// 3F.1.2 — a SIGSEGV kills the child, not the server: the crash is reported
// with its signal and a LATER test still runs and passes.
TEST(ForkPerTestModeTests, crashIsolatesToOneChild) {
    std::string out = runSelf(
        "CAJETA_FORK_PER_TEST=1 CAJETA_FORK_PROBE_CRASH=1",
        "ForkCrashProbe.segvOnDemand:ZoneOffsetTests.hoursMinutes");
    EXPECT_NE(out.find("killed by signal"), std::string::npos) << out;
    EXPECT_NE(out.find("[       OK ] ZoneOffsetTests.hoursMinutes"),
              std::string::npos) << out;
    EXPECT_NE(out.find("[  PASSED  ] 1 tests."), std::string::npos) << out;
    EXPECT_NE(out.find("[  FAILED  ] 1 tests"), std::string::npos) << out;
}

#endif  // !_WIN32
