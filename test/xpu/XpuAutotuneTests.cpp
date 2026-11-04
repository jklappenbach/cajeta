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
#include <unistd.h>
using cajeta_test::CajetaJit;

namespace {
// One store per process and test. A shared fixed path let a parallel run's
// tests, or a sibling clone's, wipe each other's store mid-test (measured
// 2026-10-03: discardsAndReportsAHintTunedForAnotherBuild failed under a
// 32-worker run and passed alone).
std::string tuneDir() {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    auto d = std::filesystem::temp_directory_path()
           / ("cajeta-autotune-test-" + std::to_string(getpid()) + "-"
              + (info ? info->name() : "none"));
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

// Each build keeps its own entry, so two binaries alternating over one store
// do not re-time each other (cajeta-llm 15.2.19.9.2). Another build misses
// quietly; the first build's entry is still there afterwards.
TEST(XpuAutotune, anotherBuildMissesWithoutDisturbingTheFirst) {
    std::string dir = tuneDir();
    testing::internal::CaptureStderr();
    int rc = runWith(
        "        Autotune.rememberFor(dir, \"w\", 60, \"build-A\");\n"
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (!a.isPresent() || a.get() != 60) { return 1; }\n"
        "        return 0;\n", dir);
    ASSERT_EQ(rc, 0);
    rc = runWith(
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (b.isPresent()) { return 1; }\n"
        "        Autotune.rememberFor(dir, \"w\", 70, \"build-B\");\n"
        "        return 0;\n", dir);
    ASSERT_EQ(rc, 0);
    rc = runWith(
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (!a.isPresent() || a.get() != 60) { return 1; }\n"
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (!b.isPresent() || b.get() != 70) { return 2; }\n"
        "        return 0;\n", dir);
    std::string out = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 0) << out;
    EXPECT_EQ(out.find("XPU-T02"), std::string::npos)
        << "a build with its own entry must not warn: " << out;
    std::filesystem::remove_all(dir);
}

// A build's entry is a fixed, seed-free name: the tuned name, ".b", and 16
// hex digits of two FNV-1a lanes over the build id, so every process finds it.
TEST(XpuAutotune, aBuildsEntryNameIsStableAcrossProcesses) {
    EXPECT_EQ(runWith(
        "        String a #= Autotune.slotOf(\"w\", \"build-A\");\n"
        "        if (!a.equals(\"w.bc6aa8e915e991eeb\")) { return 1; }\n"
        "        String b #= Autotune.slotOf(\"w\", \"build-B\");\n"
        "        if (!b.equals(\"w.bc3aa89d81d3d06ee\")) { return 2; }\n"
        "        String e #= Autotune.slotOf(\"w\", \"\");\n"
        "        if (!e.equals(\"w.b811c9dc5050c5d1f\")) { return 3; }\n"
        "        return 0;\n", ""), 0);
}

// An entry whose recorded build is not the one asked for (a fingerprint
// collision, or a hand edit) is DISCARDED and reported, not trusted.
TEST(XpuAutotune, discardsAndReportsAnEntryRecordedForOtherCode) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Autotune.rememberFor(dir, \"w\", 60, \"build-A\");\n"
        "        return 0;\n", dir);
    ASSERT_EQ(rc, 0);
    int rewritten = 0;
    for (const auto& dev : std::filesystem::directory_iterator(dir)) {
        auto f = dev.path() / "w.bc6aa8e915e991eeb.for";
        if (std::filesystem::exists(f)) { std::ofstream(f) << "build-Z"; ++rewritten; }
    }
    ASSERT_EQ(rewritten, 1);
    testing::internal::CaptureStderr();
    int rc2 = runWith(
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (b.isPresent()) { return 2; }\n"
        "        return 0;\n", dir);
    std::string out2 = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc2, 0) << out2;
    EXPECT_NE(out2.find("XPU-T02"), std::string::npos) << out2;
    EXPECT_NE(out2.find("knob=w"), std::string::npos) << out2;
    EXPECT_NE(out2.find("device="), std::string::npos) << out2;
    EXPECT_NE(out2.find("value=60"), std::string::npos) << out2;
    EXPECT_NE(out2.find("report this line"), std::string::npos) << out2;
    std::filesystem::remove_all(dir);
}

// invalidate(name) drops the name for every build, in memory and on disk, and
// leaves a name that merely shares its prefix.
TEST(XpuAutotune, invalidateDropsEveryBuildsEntry) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Autotune.rememberFor(dir, \"w\", 60, \"build-A\");\n"
        "        Autotune.rememberFor(dir, \"w\", 70, \"build-B\");\n"
        "        Autotune.rememberFor(dir, \"w.bx\", 80, \"build-A\");\n"
        "        Autotune.rememberFor(dir, \"wide\", 90, \"build-A\");\n"
        "        Autotune.invalidate(dir, \"w\");\n"
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (b.isPresent()) { return 2; }\n"
        "        Optional<int32> x = Autotune.recallFor(dir, \"w.bx\", \"build-A\");\n"
        "        if (!x.isPresent() || x.get() != 80) { return 3; }\n"
        "        return 0;\n", dir);
    ASSERT_EQ(rc, 0);
    EXPECT_EQ(runWith(
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (b.isPresent()) { return 2; }\n"
        "        Optional<int32> x = Autotune.recallFor(dir, \"w.bx\", \"build-A\");\n"
        "        if (!x.isPresent() || x.get() != 80) { return 3; }\n"
        "        Optional<int32> d = Autotune.recallFor(dir, \"wide\", \"build-A\");\n"
        "        if (!d.isPresent() || d.get() != 90) { return 4; }\n"
        "        return 0;\n", dir), 0);
    std::filesystem::remove_all(dir);
}

// An underperforming hint is reported with both values and then dropped, so
// the next execution tunes again instead of repeating a known-worse choice.
TEST(XpuAutotune, reportsAndDropsAnUnderperformingHint) {
    std::string dir = tuneDir();
    testing::internal::CaptureStderr();
    int rc = runWith(
        "        Autotune.remember(dir, \"w\", 60);\n"
        "        Autotune.reportUnderperforming(dir, \"w\", 60, 72);\n"
        "        Optional<int32> a = Autotune.recall(dir, \"w\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        return 0;\n", dir);
    std::string out = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 0) << out;
    EXPECT_NE(out.find("XPU-T01"), std::string::npos) << out;
    EXPECT_NE(out.find("used=60"), std::string::npos) << out;
    EXPECT_NE(out.find("better=72"), std::string::npos) << out;
    std::filesystem::remove_all(dir);
}

// The in-process memo must not answer ahead of the build check: a sweep
// that remembers and resolves in one process is the real shape. Build B
// misses until it is tuned, then both builds answer their own values.
TEST(XpuAutotune, theMemoDoesNotAnswerAheadOfTheBuildCheck) {
    std::string dir = tuneDir();
    int rc = runWith(
        "        Autotune.rememberFor(dir, \"w\", 60, \"build-A\");\n"
        "        Optional<int32> same = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (!same.isPresent() || same.get() != 60) { return 1; }\n"
        "        Optional<int32> other = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (other.isPresent()) { return 2; }\n"
        "        Autotune.rememberFor(dir, \"w\", 70, \"build-B\");\n"
        "        Optional<int32> a = Autotune.recallFor(dir, \"w\", \"build-A\");\n"
        "        if (!a.isPresent() || a.get() != 60) { return 3; }\n"
        "        Optional<int32> b = Autotune.recallFor(dir, \"w\", \"build-B\");\n"
        "        if (!b.isPresent() || b.get() != 70) { return 4; }\n"
        "        return 0;\n", dir);
    EXPECT_EQ(rc, 0);
    std::filesystem::remove_all(dir);
}

// The same check with NO store directory. A process that tunes in memory only
// (dir empty) fell through to `recall`, which answers from the memo, and the
// build was never compared: a value measured against build A answered for
// build B. ShapeChoice keys its cache by the candidates' build, so it needs
// this to hold with or without a store.
TEST(XpuAutotune, theMemoChecksTheBuildWithNoStoreDirectory) {
    testing::internal::CaptureStderr();
    int rc = runWith(
        "        Autotune.rememberFor(\"\", \"w\", 60, \"build-A\");\n"
        "        Optional<int32> same = Autotune.recallFor(\"\", \"w\", \"build-A\");\n"
        "        if (!same.isPresent() || same.get() != 60) { return 1; }\n"
        "        Optional<int32> other = Autotune.recallFor(\"\", \"w\", \"build-B\");\n"
        "        if (other.isPresent()) { return 2; }\n"
        "        Autotune.rememberFor(\"\", \"w\", 70, \"build-B\");\n"
        "        Optional<int32> a = Autotune.recallFor(\"\", \"w\", \"build-A\");\n"
        "        if (!a.isPresent() || a.get() != 60) { return 3; }\n"
        "        Optional<int32> b = Autotune.recallFor(\"\", \"w\", \"build-B\");\n"
        "        if (!b.isPresent() || b.get() != 70) { return 4; }\n"
        "        return 0;\n", "");
    std::string out = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 0) << out;
}
