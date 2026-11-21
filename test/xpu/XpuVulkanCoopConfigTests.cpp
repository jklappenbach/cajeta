// A native Vulkan tile kernel needs the 16x16x16 config of its tiles, not only the
// CooperativeMatrixKHR capability (gpu-kernel-parity 14.2.6). The 4090's Vulkan lists int8
// only at 16x16x32, and a native int8 tile launched there wrote zeros. These tests read the
// SPIR-V the compiler actually emits, so the scan is checked against real tiles rather than
// a hand-built module (XpuVulkanSpirvCapabilityTests has those).
#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" uint32_t __cajeta_xpu_vk_spv_missing_capability(const uint32_t* w, size_t nwords,
                                                            uint32_t have, const char** name);

using namespace cajeta::xpu::probe;

namespace {

constexpr uint32_t kBase = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
                           (1u << 5) | (1u << 6) | (1u << 9);   // Float16 .. CooperativeMatrixKHR, VMM
constexpr uint32_t kCoopF16F32 = 1u << 11;
constexpr uint32_t kCoopS8 = 1u << 12;
constexpr uint32_t kCoopBf16 = 1u << 13;
constexpr uint32_t kCoopF16F16 = 1u << 14;
constexpr uint32_t kAll = kBase | kCoopF16F32 | kCoopS8 | kCoopBf16 | kCoopF16F16;

const char* kSource = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
public class C {
    @Kernel @Occupancy(maxThreads = 32)
    public static void s8(KernelBuffer<int32> y, KernelBuffer<int8> a, KernelBuffer<int8> b) {
        CooperativeMatrix<int8,16,16,0> ta;
        CooperativeMatrix<int8,16,16,1> tb;
        CooperativeMatrix<int32,16,16,2> acc;
        ta.load(a, 0, 0, 16);
        tb.load(b, 0, 0, 16);
        acc.splat(0);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    @Kernel @Occupancy(maxThreads = 32)
    public static void f16(KernelBuffer<float32> y, KernelBuffer<float16> a, KernelBuffer<float16> b) {
        CooperativeMatrix<float16,16,16,0> ta;
        CooperativeMatrix<float16,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        ta.load(a, 0, 0, 16);
        tb.load(b, 0, 0, 16);
        acc.splat(0.0f);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
}
)CJ";

std::vector<uint32_t> spirvOf(const char* kernel) {
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.C", "coopcfg");
    auto k = findMethod(module->getStructures()["test.C"], kernel);
    if (!k) return {};
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext ctx;
    llvm::Module m("coopcfg", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(m, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, m);
    std::vector<uint8_t> bytes = cajeta::xpu::vulkan::emitSpirv(m, *tm);
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    return w;
}

} // namespace

TEST(XpuVulkanCoopConfig, anInt8TileKernelNeedsTheInt8Config) {
    std::vector<uint32_t> w = spirvOf("s8");
    ASSERT_FALSE(w.empty());
    const char* name = nullptr;
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kAll, &name)) << name;
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kAll & ~kCoopS8, &name));
    ASSERT_NE(nullptr, name);
    EXPECT_NE(std::string(name).find("int8"), std::string::npos) << name;
}

TEST(XpuVulkanCoopConfig, anF16TileKernelNeedsTheF16Config) {
    std::vector<uint32_t> w = spirvOf("f16");
    ASSERT_FALSE(w.empty());
    const char* name = nullptr;
    // The int8 config does not matter to it; its own does.
    EXPECT_EQ(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kAll & ~kCoopS8, &name))
        << name;
    EXPECT_NE(0u, __cajeta_xpu_vk_spv_missing_capability(w.data(), w.size(), kAll & ~kCoopF16F32,
                                                         &name));
}

// On a device with matrix units, CAJETA_XPU_DENY=coop-int8 takes the int8 kernel onto "$pt",
// which gives the right cells: the rung a device without the config takes.
TEST(XpuVulkanCoopConfigDeviceTests, aDeniedInt8ConfigRunsTheTileOnThePortableRung) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    setenv("CAJETA_XPU_DENY", "coop-int8", 1);
    const char* src = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.XpuLaunchException;
public class R {
    @Kernel @Occupancy(maxThreads = 32)
    public static void s8(KernelBuffer<int32> y, KernelBuffer<int8> a, KernelBuffer<int8> e) {
        CooperativeMatrix<int8,16,16,0> ta;
        CooperativeMatrix<int8,16,16,1> tb;
        CooperativeMatrix<int32,16,16,2> acc;
        ta.load(e, 0, 0, 16);
        tb.load(a, 0, 0, 16);
        acc.splat(0);
        acc.mma(ta, tb);
        acc.store(y, 0, 0, 16);
    }
    // -1 right, -2 refused by name, else wrong cells * 1000 + the first.
    public static int32 run() {
        int8[] h = heap int8[256];
        int8[] he = heap int8[256];
        for (int32 i = 0; i < 256; i = i + 1) { h[i] = (int8) ((i * 7) % 101); he[i] = (int8) ((i / 16 == i % 16) ? 1 : 0); }
        KernelBuffer<int8> a = heap KernelBuffer<int8>(256);
        KernelBuffer<int8> e = heap KernelBuffer<int8>(256);
        KernelBuffer<int32> y = heap KernelBuffer<int32>(256);
        a.upload(h);
        e.upload(he);
        KernelStream s #= KernelStream.current();
        try {
            s8.launch(s, grid: [1], block: [32])(y, a, e);
            s.sync();
        } catch (XpuLaunchException ex) {
            return -2;
        }
        int32[] g = heap int32[256];
        y.download(g);
        int32 bad = 0;
        int32 first = -1;
        for (int32 i = 0; i < 256; i = i + 1) {
            if (g[i] != (int32) h[i]) { if (first < 0) { first = i; } bad = bad + 1; }
        }
        if (bad == 0) { return -1; }
        return bad * 1000 + first;
    }
}
)CJ";
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = cajeta_test::CajetaJit::compile(src, "test.R", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    testing::internal::CaptureStderr();
    const int32_t got = run();
    const std::string err = testing::internal::GetCapturedStderr();
    unsetenv("CAJETA_XPU_DENY");
    // A device whose subgroups are narrower than the portable tile's 32 lanes refuses it by
    // name (14.2.4); everywhere else the portable rung gives the right cells.
    if (err.find("subgroups of") != std::string::npos) {
        EXPECT_EQ(got, -2) << err;
        GTEST_SKIP() << "the portable tile is refused here (subgroups under 32)";
    }
    EXPECT_EQ(got, -1) << err;
    EXPECT_NE(err.find("[xpu-rung]"), std::string::npos) << err;
}
