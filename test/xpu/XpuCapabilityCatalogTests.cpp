//
// XpuCapabilityCatalogTests — Device.supports answers the capability catalog
// from the driver on every backend (gpu-kernel-parity 13.1.1).
//
// The catalog is what host code and the fallback ladders read to know which
// hardware is there: the matrix units the driver lists, f64, f16 arithmetic,
// integer dot products and the wave widths a kernel can run at. The program
// launches a kernel first: with none, no backend is active and every answer is no.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.Capability;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class C {
    @Kernel
    public static void touch(KernelBuffer<int32> y) { y[KernelThread.globalIdX()] = 1; }
    public static int32 run() {
        KernelBuffer<int32> y = heap KernelBuffer<int32>(32);
        KernelStream s #= KernelStream.current();
        touch.launch(s, grid: [1], block: [32])(y);
        s.sync();
        int32 bits = 0;
        if (Device.supports(Capability.CoopMatrixF16F32Acc)) { bits = bits | 1; }
        if (Device.supports(Capability.CoopMatrixInt8Int32Acc)) { bits = bits | 2; }
        if (Device.supports(Capability.CoopMatrixBf16F32AccNative)) { bits = bits | 4; }
        if (Device.supports(Capability.Float64)) { bits = bits | 8; }
        if (Device.supports(Capability.Float16Arithmetic)) { bits = bits | 16; }
        if (Device.supports(Capability.IntegerDot4x8)) { bits = bits | 32; }
        if (Device.supports(Capability.SubgroupWidth32)) { bits = bits | 64; }
        if (Device.supports(Capability.SubgroupWidth64)) { bits = bits | 128; }
        return bits;
    }
}
)CJ";

const char* kNames[] = {"f16 matrix", "int8 matrix", "bf16 matrix", "f64", "f16 arithmetic",
                        "int dot 4x8", "wave 32", "wave 64"};

int catalogOn(cajeta::xpu::Backend be, const char* name) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.C", o);
    if (!jit) return -1;
    auto run = jit->lookup<int (*)()>("run");
    if (!run) return -1;
    int bits = run();
    std::string line = std::string(name) + " catalog:";
    for (int i = 0; i < 8; ++i) line += std::string(" ") + kNames[i] + "=" + ((bits >> i) & 1 ? "1" : "0");
    std::printf("  %s\n", line.c_str());
    return bits;
}

} // namespace

TEST(XpuCapabilityCatalog, theCpuBackendAnswersFromWhatItIs) {
    int bits = catalogOn(cajeta::xpu::Backend::Cpu, "cpu");
    ASSERT_GE(bits, 0);
    EXPECT_EQ(bits & 7, 0) << "the cpu backend has no native matrix unit";
    EXPECT_EQ(bits & 8, 8) << "the cpu backend runs f64";
    EXPECT_EQ(bits & 192, 192) << "the cpu backend runs any wave width";
}

TEST(XpuCapabilityCatalog, vulkanReportsTheMatrixConfigsItsDriverLists) {
    using cajeta::xpu::vulkan::VulkanDriver;
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    int bits = catalogOn(cajeta::xpu::Backend::Spirv, "vulkan");
    ASSERT_GE(bits, 0);
    const char* deny = std::getenv("CAJETA_XPU_DENY");
    if (deny && std::string(deny).find("coopmatrix") != std::string::npos)
        EXPECT_EQ(bits & 7, 0) << "cooperative matrices are denied, yet a matrix config was reported";
    else if (VulkanDriver::coopMatrixAvailable())
        EXPECT_NE(bits & 3, 0) << "the driver has cooperative matrices but lists no f16 or int8 config";
    else
        EXPECT_EQ(bits & 7, 0) << "no cooperative matrix, yet a matrix config was reported";
    EXPECT_NE(bits & 192, 0) << "no wave width at all";
}

TEST(XpuCapabilityCatalog, hipReportsItsArchitecture) {
    if (!cajeta::xpu::test::hipAvailable()) GTEST_SKIP() << "no HIP device";
    int bits = catalogOn(cajeta::xpu::Backend::Amdgpu, "amdgpu");
    ASSERT_GE(bits, 0);
    EXPECT_EQ(bits & (8 | 16 | 128), 8 | 16 | 128) << "every AMD GPU runs f64, f16 and wave 64";
}
