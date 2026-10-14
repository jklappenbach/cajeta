// The calibration set's device facts (xpu-tile-scheduling plan 2.1.1, 2.2.1).
//
// First contact with a device measures every fact the backend can show and
// records the rest as absent; the facts then come from memory, and a later
// process reads them from the store without probing. CPU backend, each test
// against its own repo-local XDG_CACHE_HOME.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
using cajeta_test::CajetaJit;

namespace {

std::string testDir() {
    const auto* info = testing::UnitTest::GetInstance()->current_test_info();
    std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto d = root / "tmp" / ("device-calibration-test-" + std::to_string(getpid()) + "-" + (info ? info->name() : "none"));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d.string();
}

std::unique_ptr<CajetaJit> compile(const std::string& body) {
    std::string program =
        "package test;\n"
        "import cajeta.io.file.File;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.xpu.calibration.DeviceCalibration;\n"
        "public final class T {\n" + body + "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return CajetaJit::compile(program, "test.T", o);
}

int call(CajetaJit* jit, const char* name) {
    auto fn = jit->lookup<int (*)()>(name);
    return fn ? fn() : -101;
}

} // namespace

// 2.1.1 — first contact measures and records; the cpu backend's absent facts are absent.
TEST(XpuDeviceCalibration, firstContactMeasuresAndRecordsAbsentFactsAsAbsent) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    auto jit = compile(
        "    public static int32 run() {\n"
        "        DeviceCalibration d = DeviceCalibration.current();\n"
        "        if (DeviceCalibration.probeRuns() != 1) { return 1; }\n"
        "        if (!d.has(DeviceCalibration.VECTOR_PEAK) || d.value(DeviceCalibration.VECTOR_PEAK) <= 0.0) { return 2; }\n"
        "        if (!d.has(DeviceCalibration.BANDWIDTH) || d.value(DeviceCalibration.BANDWIDTH) <= 0.0) { return 3; }\n"
        "        if (!d.has(DeviceCalibration.RIDGE_VECTOR)) { return 4; }\n"
        "        if (!d.has(DeviceCalibration.LAUNCH_BETA) || !d.has(DeviceCalibration.LAUNCH_ALPHA)) { return 5; }\n"
        "        if (!d.has(DeviceCalibration.EVENT)) { return 6; }\n"
        "        if (!d.has(DeviceCalibration.OCCUPANCY_4) || !d.has(DeviceCalibration.OCCUPANCY_48)) { return 7; }\n"
        "        if (!d.has(DeviceCalibration.PROBE) || d.value(DeviceCalibration.PROBE) <= 0.0) { return 8; }\n"
        "        if (!d.absent(DeviceCalibration.MATRIX_PEAK)) { return 20; }\n"
        "        if (!d.absent(DeviceCalibration.BANDWIDTH_HOST_LOAD)) { return 21; }\n"
        "        if (!d.absent(DeviceCalibration.RIDGE_MATRIX)) { return 22; }\n"
        "        if (!d.absent(DeviceCalibration.BYTES_IN_FLIGHT)) { return 23; }\n"
        "        if (!d.absent(DeviceCalibration.GRAPH_BUILD) || !d.absent(DeviceCalibration.GRAPH_NODE)) { return 24; }\n"
        "        if (!d.absent(DeviceCalibration.INTER_QUEUE)) { return 25; }\n"
        "        if (!d.absent(DeviceCalibration.SCHED_DELAY_0)) { return 26; }\n"
        "        if (!d.absent(DeviceCalibration.YIELD) || !d.absent(DeviceCalibration.PREEMPT)) { return 27; }\n"
        "        if (!d.absent(DeviceCalibration.POWER_DROOP)) { return 28; }\n"
        "        if (d.value(DeviceCalibration.MATRIX_PEAK) != 0.0) { return 29; }\n"
        "        DeviceCalibration again = DeviceCalibration.current();\n"
        "        if (DeviceCalibration.probeRuns() != 1) { return 30; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
    std::filesystem::remove_all(dir);
}

// 2.1.1, 2.2.1 — a later process reads the facts from the store and does not probe.
TEST(XpuDeviceCalibration, aLaterProcessReadsTheStoreWithoutProbing) {
    std::string dir = testDir();
    setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    std::string facts = dir + "/facts.txt";
    auto first = compile(
        "    public static int32 run() {\n"
        "        DeviceCalibration d = DeviceCalibration.current();\n"
        "        String t #= d.text();\n"
        "        int8[] b #= t.toBytes();\n"
        "        File.writeAllBytes(\"" + facts + "\", b, (int64) b.count());\n"
        "        return DeviceCalibration.probeRuns();\n"
        "    }\n");
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(call(first.get(), "run"), 1);
    auto second = compile(
        "    public static int32 run() {\n"
        "        DeviceCalibration d = DeviceCalibration.current();\n"
        "        if (DeviceCalibration.probeRuns() != 0) { return 1; }\n"
        "        int8[] b #= File.readAllBytes(\"" + facts + "\");\n"
        "        String want = heap String(#b, (int32) b.count());\n"
        "        String got #= d.text();\n"
        "        if (!got.equals(want)) { return 2; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(call(second.get(), "run"), 0);
    std::filesystem::remove_all(dir);
}

// 2.2.1 — facts round-trip through their text; an unparseable line is skipped.
TEST(XpuDeviceCalibration, factsRoundTripThroughTheirText) {
    auto jit = compile(
        "    public static int32 run() {\n"
        "        DeviceCalibration d = heap DeviceCalibration();\n"
        "        d.put(\"a\", 12.3456);\n"
        "        d.putAbsent(\"b\");\n"
        "        d.put(\"c\", 0.0 - 2.5);\n"
        "        String t #= d.text();\n"
        "        String noisy #= t + \"junk\\nd=1x\\n\";\n"
        "        int8[] b #= noisy.toBytes();\n"
        "        DeviceCalibration p #= DeviceCalibration.parse(b);\n"
        "        if (p.value(\"a\") != 12.346) { return 1; }\n"
        "        if (!p.absent(\"b\") || p.has(\"b\")) { return 2; }\n"
        "        if (p.value(\"c\") != 0.0 - 2.5) { return 3; }\n"
        "        if (p.has(\"d\") || p.absent(\"d\")) { return 4; }\n"
        "        String t2 #= p.text();\n"
        "        if (!t2.equals(t)) { return 5; }\n"
        "        return 0;\n"
        "    }\n");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(call(jit.get(), "run"), 0);
}
