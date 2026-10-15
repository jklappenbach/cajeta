// Meta-tests for the fork server (compile-cache plan Unit 6). Each drives THIS
// binary as a subprocess with CAJETA_FORK_SERVER=1, feeds it RUN requests on
// stdin and reads its DONE answers, so the tests live at the process level.

#if !defined(_WIN32)

#include <gtest/gtest.h>
#include "PortableEnv.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int countOf(const std::string& hay, const std::string& needle) {
    int c = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
            at = hay.find(needle, at + needle.size())) ++c;
    return c;
}

// Runs the server with `requests` on its stdin (bounded by `timeout` seconds)
// and returns everything it wrote to stdout and stderr.
std::string runServer(const fs::path& dir, const std::string& requests,
                      const std::string& envPrefix = "", int timeoutS = 400) {
    fs::path req = dir / "requests.txt";
    { std::ofstream out(req); out << requests; }
    std::string cmd = "timeout " + std::to_string(timeoutS) + " env " + envPrefix
        + " CAJETA_FORK_SERVER=1 CAJETA_STDLIB_REUSE=1 CAJETA_PRIME_TIMING=1 '"
        + cajeta_self_exe().string() + "' --gtest_brief=0 --gtest_filter=NoSuchSuite.* < '" + req.string() + "' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "<popen failed>";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

fs::path scratch() {
    const char* root = std::getenv("CAJETA_SOURCE_ROOT");
    fs::path dir = fs::path(root ? root : ".") / "tmp" / ("fork-server-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

// 6.1.1: two requests, two answers, each chunk's output in its file, one prime.
TEST(ForkServerModeTests, servesChunksFromOnePrime) {
    fs::path dir = scratch();
    fs::path a = dir / "a.out", b = dir / "b.out";
    std::string out = runServer(dir,
        "RUN 120 ZoneOffsetTests.* " + a.string() + "\n"
        "RUN 120 SharedStdlibDefaultTests.reusingCompilesLinkOneStdlib " + b.string() + "\n"
        "QUIT\n");
    EXPECT_EQ(countOf(out, "DONE 0"), 2) << out;
    EXPECT_EQ(countOf(out, "[CAJETA_PRIME_TIMING]"), 1) << out;
    std::string aOut = slurp(a), bOut = slurp(b);
    EXPECT_NE(aOut.find("[  PASSED  ]"), std::string::npos) << aOut;
    EXPECT_GT(countOf(aOut, "[       OK ] ZoneOffsetTests."), 0) << aOut;
    EXPECT_NE(bOut.find("[       OK ] SharedStdlibDefaultTests.reusingCompilesLinkOneStdlib"),
              std::string::npos) << bOut;
    EXPECT_EQ(countOf(aOut + bOut, "[CAJETA_PRIME_TIMING]"), 0);
    fs::remove_all(dir);
}

// 6.1.2: a crashing chunk answers its signal and the server keeps serving.
TEST(ForkServerModeTests, aCrashingChunkDoesNotTakeTheServer) {
    fs::path dir = scratch();
    fs::path a = dir / "a.out", b = dir / "b.out";
    std::string out = runServer(dir,
        "RUN 120 ForkCrashProbe.segvOnDemand " + a.string() + "\n"
        "RUN 120 ZoneOffsetTests.hoursMinutes " + b.string() + "\n",
        "CAJETA_FORK_PROBE_CRASH=1");
    EXPECT_NE(out.find("DONE 139"), std::string::npos) << out;
    EXPECT_EQ(countOf(out, "DONE 0"), 1) << out;
    EXPECT_NE(slurp(b).find("[       OK ] ZoneOffsetTests.hoursMinutes"), std::string::npos);
    fs::remove_all(dir);
}

// 6.1.3: a chunk past its deadline is killed and answered 124.
TEST(ForkServerModeTests, aChunkPastItsDeadlineIsKilled) {
    fs::path dir = scratch();
    fs::path a = dir / "a.out", b = dir / "b.out";
    std::string out = runServer(dir,
        "RUN 2 ForkHangProbe.sleepOnDemand " + a.string() + "\n"
        "RUN 120 ZoneOffsetTests.hoursMinutes " + b.string() + "\n",
        "CAJETA_FORK_PROBE_HANG=1");
    EXPECT_NE(out.find("DONE 124"), std::string::npos) << out;
    EXPECT_EQ(countOf(out, "DONE 0"), 1) << out;
    EXPECT_NE(slurp(b).find("[       OK ] ZoneOffsetTests.hoursMinutes"), std::string::npos);
    fs::remove_all(dir);
}

// The hang probe the deadline test forks: inert unless armed.
TEST(ForkHangProbe, sleepOnDemand) {
    if (!std::getenv("CAJETA_FORK_PROBE_HANG")) GTEST_SKIP() << "armed only by ForkServerModeTests";
    ::sleep(60);
}

#endif
