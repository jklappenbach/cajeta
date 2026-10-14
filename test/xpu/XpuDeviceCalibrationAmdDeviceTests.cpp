// The calibration set on an AMD GPU (xpu-tile-scheduling plan 2.1.1): every
// fact the device can show is measured, the APU's host-load bandwidth among
// them, and the facts HIP cannot show (a recorded graph, yield, preemption,
// power) are absent. Skips without a HIP device.
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
using cajeta_test::CajetaJit;

TEST(XpuDeviceCalibrationAmdDeviceTests, theGpuFactsAreMeasuredAndTheRestAbsent) {
    CAJETA_SKIP_IF_NO_HIP();
    std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    auto dir = root / "tmp" / ("device-calibration-amd-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    setenv("XDG_CACHE_HOME", dir.string().c_str(), 1);
    std::string program =
        "package test;\n"
        "import cajeta.xpu.Device;\n"
        "import cajeta.xpu.calibration.DeviceCalibration;\n"
        "public final class T {\n"
        "    public static int32 run() {\n"
        "        if (!Device.activeBackend().equals(\"hip\")) { return 99; }\n"
        "        DeviceCalibration d = DeviceCalibration.current();\n"
        "        if (!d.has(DeviceCalibration.VECTOR_PEAK)) { return 1; }\n"
        "        if (!d.has(DeviceCalibration.MATRIX_PEAK)) { return 2; }\n"
        "        if (d.value(DeviceCalibration.MATRIX_PEAK) <= d.value(DeviceCalibration.VECTOR_PEAK)) { return 3; }\n"
        "        if (!d.has(DeviceCalibration.BANDWIDTH)) { return 4; }\n"
        "        if (Device.integrated()) {\n"
        "            if (!d.has(DeviceCalibration.BANDWIDTH_HOST_LOAD)) { return 5; }\n"
        "            if (d.value(DeviceCalibration.BANDWIDTH_HOST_LOAD) > 1.05 * d.value(DeviceCalibration.BANDWIDTH)) { return 6; }\n"
        "        } else if (!d.absent(DeviceCalibration.BANDWIDTH_HOST_LOAD)) { return 7; }\n"
        "        if (!d.has(DeviceCalibration.RIDGE_MATRIX)) { return 8; }\n"
        "        if (!d.has(DeviceCalibration.BYTES_IN_FLIGHT)) { return 9; }\n"
        "        if (!d.has(DeviceCalibration.INTER_QUEUE)) { return 10; }\n"
        "        if (!d.has(DeviceCalibration.SCHED_DELAY_0) || !d.has(DeviceCalibration.SCHED_DELAY_8)) { return 11; }\n"
        "        if (!d.has(DeviceCalibration.OCCUPANCY_4) || !d.has(DeviceCalibration.OCCUPANCY_48)) { return 12; }\n"
        "        if (!d.absent(DeviceCalibration.GRAPH_BUILD)) { return 13; }\n"
        "        if (!d.absent(DeviceCalibration.YIELD) || !d.absent(DeviceCalibration.POWER_DROOP)) { return 14; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    auto jit = CajetaJit::compile(program, "test.T", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int rc = fn();
    if (rc == 99) GTEST_SKIP() << "the HIP backend did not become active";
    EXPECT_EQ(rc, 0);
    std::filesystem::remove_all(dir);
}
