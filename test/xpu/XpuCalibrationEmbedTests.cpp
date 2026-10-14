// Records embedded at build (xpu-tile-scheduling plan 2.1.5, §12.4c): the default
// CalibrationStore serves a record built in with --with-calibration, and only
// when the record's device identity is this device's. CPU backend, each test
// against its own empty repo-local XDG_CACHE_HOME so nothing comes from the cache.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
using cajeta_test::CajetaJit;

namespace {

std::filesystem::path testDir() {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    auto d = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "tmp" /
             ("calibration-embed-" + std::to_string(getpid()) + "-" + (info ? info->name() : "none"));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string program(const std::string& body) {
    return "package test;\n"
           "import cajeta.io.file.File;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.xpu.Calibration;\n"
           "import cajeta.xpu.CalibrationKey;\n"
           "import cajeta.xpu.CalibrationRecord;\n"
           "import cajeta.xpu.DefaultCalibrationStore;\n"
           "public final class T {\n"
           "    static #String str(int8[] b) {\n"
           "        if (b == null) { return \"<null>\"; }\n"
           "        int32 n = (int32) b.count();\n"
           "        int8[] c = heap int8[n];\n"
           "        int32 i = 0;\n"
           "        while (i < n) { c[i] = b[i]; i = i + 1; }\n"
           "        return heap String(#c, n);\n"
           "    }\n"
           "    static void save(String path, String s) {\n"
           "        int8[] b #= s.toBytes();\n"
           "        File.writeAllBytes(path, b, (int64) b.count());\n"
           "    }\n"
           "    static void saveBytes(String path, int8[] b) {\n"
           "        File.writeAllBytes(path, b, (int64) b.count());\n"
           "    }\n" + body + "}\n";
}

int run(const std::string& body, const std::string& snapshot, const char* fn) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    o.withCalibration = snapshot;
    auto jit = CajetaJit::compile(program(body), "test.T", o);
    if (!jit) return -100;
    auto f = jit->lookup<int (*)()>(fn);
    return f ? f() : -101;
}

// Phase one writes, for `device` (empty = this device's), the record payload `p` and its slot.
std::string snapshotFor(const std::filesystem::path& dir, const std::string& device, const std::string& tag) {
    std::string d = (dir / tag).string();
    std::string keyExpr = device.empty()
        ? "CalibrationKey.live(\"embed-subject\")"
        : "heap CalibrationKey(\"embed-subject\", \"" + device + "\", \"\" + live.driver, \"\" + live.compiler, \"\" + live.powerMode)";
    int rc = run(
        "    public static int32 prepare() {\n"
        "        CalibrationKey live #= CalibrationKey.live(\"embed-subject\");\n"
        "        CalibrationKey k #= " + keyExpr + ";\n"
        "        int8[] p #= \"launch from=build\".toBytes();\n"
        "        int8[] rec #= CalibrationRecord.encode(k, p, (int32) p.count());\n"
        "        String slot #= k.slot();\n"
        "        T.save(\"" + d + ".slot\", slot);\n"
        "        T.saveBytes(\"" + d + ".rec\", rec);\n"
        "        return 0;\n"
        "    }\n", "", "prepare");
    if (rc != 0) return "";
    std::string slot = slurp(d + ".slot"), rec = slurp(d + ".rec");
    return "cajeta-calibration-snapshot-1\n" + slot + " " + std::to_string(rec.size()) + " test.T.k\n" + rec + "\n";
}

const char* kCheck =
    "    public static int32 served() {\n"
    "        CalibrationKey live #= CalibrationKey.live(\"embed-subject\");\n"
    "        int8[] p #= Calibration.load(live);\n"
    "        String s #= T.str(p);\n"
    "        if (s.equals(\"launch from=build\")) { return 1; }\n"
    "        if (p == null) { return 0; }\n"
    "        return 2;\n"
    "    }\n"
    "    public static int32 embeddedUnder(String slot) {\n"
    "        DefaultCalibrationStore d = heap DefaultCalibrationStore();\n"
    "        int8[] raw #= d.load(slot);\n"
    "        return raw == null ? 0 : 1;\n"
    "    }\n";

} // namespace

// 2.1.5 — a record built in for this device is served, with nothing in the cache.
TEST(XpuCalibrationEmbed, aBuiltInRecordForThisDeviceIsServed) {
    auto dir = testDir();
    setenv("XDG_CACHE_HOME", dir.string().c_str(), 1);
    std::string snap = snapshotFor(dir, "", "live");
    ASSERT_FALSE(snap.empty());
    EXPECT_EQ(run(kCheck, "", "served"), 0) << "control: nothing is served without the snapshot";
    EXPECT_EQ(run(kCheck, snap, "served"), 1) << "the built-in record is served";
    std::filesystem::remove_all(dir);
}

// 2.1.5 — a record built in for another device is in the artifact but never served here.
TEST(XpuCalibrationEmbed, aBuiltInRecordForAnotherDeviceIsNotServed) {
    auto dir = testDir();
    setenv("XDG_CACHE_HOME", dir.string().c_str(), 1);
    std::string snap = snapshotFor(dir, "hip/gfx9999/00000000000000000000000000000000", "foreign");
    ASSERT_FALSE(snap.empty());
    std::string slot = slurp(dir / "foreign.slot");
    std::string probe = std::string(kCheck) +
        "    public static int32 present() { return T.embeddedUnder(\"" + slot + "\"); }\n";
    EXPECT_EQ(run(probe, snap, "present"), 1) << "the foreign record must be embedded";
    EXPECT_EQ(run(probe, snap, "served"), 0) << "and never served for this device";
    std::filesystem::remove_all(dir);
}

// 2.1.5 — a malformed snapshot is refused at build, never embedded half-read.
TEST(XpuCalibrationEmbed, aMalformedSnapshotIsRefused) {
    std::vector<cajeta::xpu::CalibrationRecordBlob> out;
    std::string error;
    EXPECT_FALSE(cajeta::xpu::parseCalibrationSnapshot("not a snapshot\n", out, error));
    EXPECT_FALSE(cajeta::xpu::parseCalibrationSnapshot(
        "cajeta-calibration-snapshot-1\ncal-0000000000000000 999 k\nshort\n", out, error));
    EXPECT_FALSE(error.empty());
    out.clear();
    EXPECT_TRUE(cajeta::xpu::parseCalibrationSnapshot(
        "cajeta-calibration-snapshot-1\ncal-0000000000000000 3 k\nabc\n", out, error));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].bytes, "abc");
}
