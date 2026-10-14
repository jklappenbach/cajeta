// The shared stdlib dylib's machine code is cached across processes
// (compile-cache plan Unit 4): a cold process compiles and stores the stdlib
// object, a warm one links the stored object, a corrupt object recompiles, and
// every arm computes the same result.

#include "gtest/gtest.h"
#include "JitTestHelper.h"
#include "../PortableEnv.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using cajeta_test::CajetaJit;
namespace fs = std::filesystem;

// The sweep's prime-once step (cajeta_tests.sh, compile-cache 5.2.1), also
// driven by StdlibObjectCacheTests: armed by CAJETA_STDLIB_PRIME, it compiles a
// program against the shared stdlib, which primes and materializes it and
// fills the object cache, and reports the cache counters on one line.
TEST(SharedStdlibPrime, primesTheSharedStdlib) {
    if (!std::getenv("CAJETA_STDLIB_PRIME")) GTEST_SKIP() << "armed only by the sweep and StdlibObjectCacheTests";
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.xpu.Autotune;\n"
        "public final class P {\n"
        "    public static int32 run() {\n"
        "        Autotune.remember(\"\", \"k\", 4242);\n"
        "        Optional<int32> a = Autotune.recall(\"\", \"k\");\n"
        "        if (a.isPresent()) { return a.get(); }\n"
        "        return -1;\n"
        "    }\n"
        "}\n", "test.P");
    int32_t result = jit->lookup<int32_t (*)()>("run")();
    std::printf("[stdlib-prime] %s served=%zu compiled=%zu result=%d\n",
                jit->usesSharedStdlib() ? "shared" : "fresh",
                CajetaJit::sharedStdlibObjectsServed(),
                CajetaJit::sharedStdlibObjectsCompiled(), result);
    std::fflush(stdout);
}

#if !defined(_WIN32)

namespace {

struct ProbeReport {
    long served = -1;
    long compiled = -1;
    long result = -1;
    std::string output;
};

ProbeReport runProbe(const std::string& envPrefix) {
    std::string cmd = envPrefix + " CAJETA_STDLIB_PRIME=1 CAJETA_STDLIB_REUSE=1 '"
        + cajeta_self_exe().string()
        + "' --gtest_filter='SharedStdlibPrime.*' 2>&1";
    ProbeReport r;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { r.output = "<popen failed>"; return r; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.output.append(buf, n);
    pclose(p);
    size_t at = r.output.find("[stdlib-prime] shared ");
    if (at != std::string::npos)
        std::sscanf(r.output.c_str() + at, "[stdlib-prime] shared served=%ld compiled=%ld result=%ld",
                    &r.served, &r.compiled, &r.result);
    return r;
}

std::vector<fs::path> objectsUnder(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    for (auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file() && e.path().extension() == ".o") out.push_back(e.path());
    return out;
}

} // namespace

TEST(StdlibObjectCacheTests, aWarmProcessLinksTheStoredStdlibObject) {
    const char* srcRoot = std::getenv("CAJETA_SOURCE_ROOT");
    if (!srcRoot) GTEST_SKIP() << "needs CAJETA_SOURCE_ROOT for a repo-local scratch dir";
    fs::path dir = fs::path(srcRoot) / "tmp" / ("objcache-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string env = "CAJETA_PRIME_CACHE_DIR='" + dir.string() + "'";

    ProbeReport cold = runProbe(env);
    ASSERT_EQ(cold.compiled, 1) << cold.output;
    EXPECT_EQ(cold.served, 0) << cold.output;
    EXPECT_EQ(cold.result, 4242) << cold.output;
    auto objects = objectsUnder(dir);
    ASSERT_EQ(objects.size(), 1u) << cold.output;
    const uintmax_t storedSize = fs::file_size(objects[0]);
    EXPECT_GT(storedSize, 1u << 20);

    ProbeReport warm = runProbe(env);
    EXPECT_EQ(warm.served, 1) << warm.output;
    EXPECT_EQ(warm.compiled, 0) << warm.output;
    EXPECT_EQ(warm.result, 4242) << warm.output;
    EXPECT_EQ(objectsUnder(dir).size(), 1u);

    fs::resize_file(objects[0], 64);
    ProbeReport corrupt = runProbe(env);
    EXPECT_EQ(corrupt.served, 0) << corrupt.output;
    EXPECT_EQ(corrupt.compiled, 1) << corrupt.output;
    EXPECT_EQ(corrupt.result, 4242) << corrupt.output;
    ASSERT_EQ(objectsUnder(dir).size(), 1u);
    EXPECT_EQ(fs::file_size(objects[0]), storedSize);

    ProbeReport off = runProbe(env + " CAJETA_PRIME_CACHE=0");
    EXPECT_EQ(off.served, 0) << off.output;
    EXPECT_EQ(off.compiled, 0) << off.output;
    EXPECT_EQ(off.result, 4242) << off.output;

    fs::remove_all(dir);
}

#endif
