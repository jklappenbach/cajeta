//
// SpirvMixedDotTests — a float16 dotSum on Vulkan gets a "$d2" build whose pairs are each
// one OpFDot2MixAcc32VALVE, launched where VK_VALVE_shader_mixed_float_dot_product is
// enabled (gpu-kernel-parity 18).
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;
using cajeta::xpu::probe::compileForInspection;
using cajeta::xpu::probe::findMethod;

namespace {

// out[g] = dot(a, b) + acc over 8 lanes of small integers, so every product and sum is exact.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.Capability;
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class MD {
    @Kernel
    public static void hd(KernelBuffer<float32> out, KernelBuffer<float32> x) {
        uint32 g = KernelThread.globalIdX();
        int64 o = (int64) g * 16L;
        Vector<float32,8> fa = x.vload<8>(o);
        Vector<float32,8> fb = x.vload<8>(o + 8L);
        Vector<float16,8> a = fa.toF16();
        Vector<float16,8> b = fb.toF16();
        out[g] = a.dotSum(b, (float32) g);
    }
    public static int32 run() {
        float32[] h = heap float32[1024];
        float32[] r = heap float32[64];
        for (int32 i = 0; i < 1024; i = i + 1) { h[i] = (float32) ((i * 7) % 11 - 5); }
        for (int32 i = 0; i < 64; i = i + 1) { r[i] = 0.0f; }
        KernelBuffer<float32> x = heap KernelBuffer<float32>(1024);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(64);
        x.upload(h);
        y.upload(r);
        KernelStream s #= KernelStream.current();
        hd.launch(s, grid: [2], block: [32])(y, x);
        s.sync();
        y.download(r);
        boolean mixed = Device.supports(Capability.FloatDot2Mix);
        for (int32 g = 0; g < 64; g = g + 1) {
            float32 want = (float32) g;
            for (int32 k = 0; k < 8; k = k + 1) { want = want + h[g * 16 + k] * h[g * 16 + 8 + k]; }
            float32 d = r[g] - want;
            if (d < 0.0f) { d = 0.0f - d; }
            float32 tol = 0.0f;
            float32 mag = want;
            if (mag < 0.0f) { mag = 0.0f - mag; }
            if (mixed) { tol = 0.000001f * (mag + 1.0f); }
            if (d > tol) { return 1000 + g; }
        }
        if (mixed) { return 778; }
        return 777;
    }
}
)CJ";

struct Census {
    unsigned mixedDots = 0;
    bool capability = false;
    bool extension = false;
};

Census census(const std::vector<uint8_t>& bytes) {
    Census c;
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    for (size_t i = 5; i < w.size();) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (wc == 0) break;
        if (op == 6916) ++c.mixedDots;
        if (op == 17 && w[i + 1] == 6912) c.capability = true;
        if (op == 10 && std::string(reinterpret_cast<const char*>(&w[i + 1]))
                == "SPV_VALVE_mixed_float_dot_product")
            c.extension = true;
        i += wc;
    }
    return c;
}

std::vector<uint8_t> emitHd(bool mixedDot) {
    using namespace cajeta::xpu::vulkan;
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kSource, "test.MD", mixedDot ? "spvd2" : "spvplain");
    auto k = findMethod(module->getStructures()["test.MD"], "hd");
    EXPECT_NE(k, nullptr);
    if (!k) return {};
    auto tm = createSpirvTargetMachine("vulkan1.3");
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_md", ctx);
    configureDeviceModule(dev, *tm);
    EXPECT_NE(mixedDot ? lowerKernelMixedDot(k, dev, "hd$d2") : lowerKernel(k, dev), nullptr);
    std::string failure;
    auto spirv = emitSpirv(dev, *tm, &failure);
    EXPECT_FALSE(spirv.empty()) << failure;
    return spirv;
}

} // namespace

TEST(SpirvMixedDotTests, eachHalfPairIsOneMixedFloatDot) {
    Census c = census(emitHd(true));
    EXPECT_EQ(c.mixedDots, 4u) << "an 8-lane dotSum is four pairs";
    EXPECT_TRUE(c.capability);
    EXPECT_TRUE(c.extension);
}

TEST(SpirvMixedDotTests, thePlainBuildDeclaresNoMixedFloatDot) {
    Census c = census(emitHd(false));
    EXPECT_EQ(c.mixedDots, 0u);
    EXPECT_FALSE(c.capability);
    EXPECT_FALSE(c.extension);
}

// 778: the "$d2" build ran, within the hardware dot2's rounding (AMD's is one ulp off on an
// exact integer sum, on hip as on Vulkan); 777: the plain build ran, exact.
TEST(SpirvMixedDotTests, aHalfDotSumMatchesTheHostOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.MD", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    int got = run();
    EXPECT_TRUE(got == 777 || got == 778) << "1000 + g: the first wrong cell, got " << got;
}

// A "$d2" build whose pipeline will not build (an older validation layer refuses the
// mixed-float dot) is set aside, and the launch runs the plain build: exact, 777.
TEST(SpirvMixedDotTests, aRefusedMixedDotBuildFallsBackToThePlainKernel) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    std::string exact = kSource;
    for (const std::string cut : {std::string("if (mixed) { tol = 0.000001f * (mag + 1.0f); }"),
                                  std::string("if (mixed) { return 778; }")}) {
        size_t at = exact.find(cut);
        ASSERT_NE(at, std::string::npos) << cut;
        exact.erase(at, cut.size());
    }
    setenv("CAJETA_XPU_VK_REFUSE_D2", "1", 1);
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(exact, "test.MD", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int (*)()>("run");
    ASSERT_NE(run, nullptr);
    int got = run();
    unsetenv("CAJETA_XPU_VK_REFUSE_D2");
    EXPECT_EQ(got, 777) << "1000 + g: the first inexact cell, so the refused build ran";
}
