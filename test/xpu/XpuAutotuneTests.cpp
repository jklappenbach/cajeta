//
// XpuAutotuneTests — the per-device tuning store.
//
// The point of the store is that a value tuned on one machine is never handed
// to another, so the tests that matter are: a round trip returns what was
// written, an absent entry stays absent, and a corrupt one reads as absent
// rather than as a number that happens to parse.
//
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <filesystem>
#include <fstream>
#include <string>
using cajeta_test::CajetaJit;

namespace {
std::string tuneDir() {
    auto d = std::filesystem::temp_directory_path() / "cajeta-autotune-test";
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d.string();
}

int runWith(const std::string& body, const std::string& dir) {
    std::string program =
        "package test;\n"
        "import cajeta.xpu.Autotune;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.lang.String;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        String dir = \"" + dir + "\";\n"
        + body +
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program, "test.T", o);
    if (!jit) return -100;
    auto fn = jit->lookup<int (*)()>("run");
    if (!fn) return -101;
    return fn();
}
} // namespace

TEST(XpuAutotune, roundTripsAValueAndReportsAbsentOtherwise) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Optional<int32> a = Autotune.recall(dir, \"partWidth\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        Autotune.remember(dir, \"partWidth\", 60);\n"
        "        Optional<int32> b = Autotune.recall(dir, \"partWidth\");\n"
        "        if (!b.isPresent()) { return 2; }\n"
        "        if (b.get() != 60) { return 3; }\n"
        "        Autotune.remember(dir, \"partWidth\", 72);\n"
        "        Optional<int32> c = Autotune.recall(dir, \"partWidth\");\n"
        "        if (c.get() != 72) { return 4; }\n"
        "        Optional<int32> d = Autotune.recall(dir, \"neverWritten\");\n"
        "        if (d.isPresent()) { return 5; }\n"
        "        return 0;\n", dir);
    EXPECT_EQ(rc, 0);
    std::filesystem::remove_all(dir);
}

// A key that does not name this machine must not be read. Writing a value under
// a foreign device directory and asking for it here has to come back absent,
// which is what stops a copied cache from mis-tuning a different part.
TEST(XpuAutotune, ignoresAnEntryWrittenForAnotherDevice) {
    std::string dir = tuneDir();
    auto foreign = std::filesystem::path(dir) / "amdgpu-999x8w64r1r1t1";
    std::filesystem::create_directories(foreign);
    { std::ofstream f(foreign / "partWidth"); f << "4096"; }
    int rc = runWith(
        "        Optional<int32> a = Autotune.recall(dir, \"partWidth\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        return 0;\n", dir);
    EXPECT_EQ(rc, 0);
    std::filesystem::remove_all(dir);
}

// A hand-edited or truncated entry reads as absent, never as a parsed number.
TEST(XpuAutotune, corruptEntryReadsAsAbsent) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Autotune.remember(dir, \"w\", 60);\n"
        "        return 0;\n", dir);
    ASSERT_EQ(rc, 0);
    // Find the device directory the run just created and corrupt its entry.
    for (auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_directory()) { std::ofstream f(e.path() / "w"); f << "6O"; }
    }
    int rc2 = runWith(
        "        Optional<int32> a = Autotune.recall(dir, \"w\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        return 0;\n", dir);
    EXPECT_EQ(rc2, 0);
    std::filesystem::remove_all(dir);
}

// Negative values are refused, so absent and "tuned negative" cannot be
// confused by a caller that reads absent as "use the default".
TEST(XpuAutotune, refusesNegativeValues) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Autotune.remember(dir, \"w\", 0 - 5);\n"
        "        Optional<int32> a = Autotune.recall(dir, \"w\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        return 0;\n", dir);
    EXPECT_EQ(rc, 0);
    std::filesystem::remove_all(dir);
}

// No writable store still tunes ONCE PER EXECUTION: the value is remembered in
// memory, recall returns it, and persists() reports that the next run will have
// to tune again. This is the read-only-box contract.
TEST(XpuAutotune, survivesAnUnwritableStoreForThisExecution) {
    int rc = runWith(
        "        Autotune.remember(\"/proc/cajeta-cannot-write\", \"w\", 61);\n"
        "        Optional<int32> a = Autotune.recall(\"/proc/cajeta-cannot-write\", \"w\");\n"
        "        if (!a.isPresent()) { return 1; }\n"
        "        if (a.get() != 61) { return 2; }\n"
        "        if (Autotune.persists()) { return 3; }\n"
        "        return 0;\n", "/proc/cajeta-cannot-write");
    EXPECT_EQ(rc, 0);
}

// An empty directory means "no store at all", which must still tune in memory
// rather than recompute on every call.
TEST(XpuAutotune, emptyDirectoryTunesInMemoryOnly) {
    int rc = runWith(
        "        Optional<int32> a = Autotune.recall(\"\", \"w\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        Autotune.remember(\"\", \"w\", 62);\n"
        "        Optional<int32> b = Autotune.recall(\"\", \"w\");\n"
        "        if (!b.isPresent() || b.get() != 62) { return 2; }\n"
        "        return 0;\n", "");
    EXPECT_EQ(rc, 0);
}

// claimOnce is the latch a LAZY calibration hangs on: true the first time a
// kernel path asks, false forever after, independent of any store.
TEST(XpuAutotune, claimOnceFiresExactlyOncePerExecution) {
    int rc = runWith(
        "        if (!Autotune.claimOnce(\"characterize\")) { return 1; }\n"
        "        if (Autotune.claimOnce(\"characterize\")) { return 2; }\n"
        "        if (Autotune.claimOnce(\"characterize\")) { return 3; }\n"
        "        if (!Autotune.claimOnce(\"other\")) { return 4; }\n"
        "        return 0;\n", "");
    EXPECT_EQ(rc, 0);
}
