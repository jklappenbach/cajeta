//
// CajetaXPU — @Occupancy override probe (kernel-occupancy-autotune U2, §3).
// GPU-free: the portable @Occupancy(maxThreads/minResident/maxRegisters) annotation
// parses into XpuKernelAttr and lowers to the AMDGPU function attributes, and an
// explicit override wins over the §2 automatic workgroup-size budgeting.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/core/XpuKernelAttr.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>
#include <fstream>
#include <random>
#include "KernelLoweringProbe.h"

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::XpuKernelAttr;

namespace {

const char* kOccSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256, minResident = 2, maxRegisters = 128)\n"
    "    public static void occ(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = a[tid] + 1.0f; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void plain(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = a[tid] + 1.0f; }\n"
    "    }\n"
    "}\n";

using cajeta::xpu::probe::compileForInspection;


cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string attrOf(llvm::Function* fn, const char* key) {
    if (!fn || !fn->hasFnAttribute(key)) return "<none>";
    return fn->getFnAttribute(key).getValueAsString().str();
}

} // namespace

// 2.1.a — @Occupancy parses into the typed view (all three params).

// 2.1.b — AMDGPU lowering maps the override to the function attributes; an
// un-annotated kernel gets none from the override path.

// 2.1.d — an explicit override wins over the §2 automatic budgeting: once
// @Occupancy has pinned flat-work-group-size, the auto setter is a no-op.
TEST(XpuOccupancyAttrProbeTests, overrideWinsOverAuto) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kOccSrc);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_TRUE(tm != nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("occ_auto", ctx);
    configureDeviceModule(dev, *tm);
    auto* fn = cajeta::xpu::amd::lowerKernel(
        findMethod(module->getStructures()["test.M"], "occ"), dev);
    ASSERT_NE(fn, nullptr);
    ASSERT_EQ(attrOf(fn, "amdgpu-flat-work-group-size"), "1,256");

    setKernelWorkgroupSize(fn, 999);   // the §2 auto path with a different size
    EXPECT_EQ(attrOf(fn, "amdgpu-flat-work-group-size"), "1,256")
        << "the explicit @Occupancy override must win over auto budgeting";
}


// --- @Occupancy(maxWaves = N): a bound stated in the kernel's own waves -----

namespace {

// A kernel built as N waves per workgroup launches N x the wave width, which
// is 256 threads at wave 32 and 512 at wave 64. A thread bound can only state
// one of them, so it was written as the larger, and on a wave-32 part that
// halved the register budget it promised (cajeta-llm's three 8-wave WMMA
// kernels spilled at 128 registers on sm_89 once the bound reached ptxas,
// 2026-10-02). `maxWaves` is resolved per kernel against the wave the
// backend actually gave it.
const char* kWavesSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class W {\n"
    "    @Kernel\n"
    "    @Occupancy(maxWaves = 8)\n"
    "    public static void waves(KernelBuffer<float32> c, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = 1.0f; }\n"
    "    }\n"
    "    @Kernel\n"
    "    @Wave(width = 64)\n"
    "    @Occupancy(maxWaves = 8)\n"
    "    public static void waves64(KernelBuffer<float32> c, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = 1.0f; }\n"
    "    }\n"
    "    @Kernel\n"
    "    @Occupancy(maxThreads = 256, maxWaves = 8)\n"
    "    public static void both(KernelBuffer<float32> c, uint32 n) {\n"
    "        uint32 tid = KernelThread.x();\n"
    "        if (tid < n) { c[tid] = 1.0f; }\n"
    "    }\n"
    "}\n";

llvm::Function* lowerAmd(const std::string& kernel, llvm::Module& dev,
                         std::string* why) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kWavesSrc, "test.W", "occwaves");
    auto tm = createAmdgpuTargetMachine("gfx1151");
    if (!tm) { *why = "no gfx1151 target machine"; return nullptr; }
    configureDeviceModule(dev, *tm);
    try {
        return cajeta::xpu::amd::lowerKernel(
            findMethod(module->getStructures()["test.W"], kernel), dev);
    } catch (cajeta::Exception& e) {
        *why = e.getMessage();
    } catch (std::exception& e) {
        *why = e.what();
    }
    return nullptr;
}

} // namespace

TEST(XpuOccupancyAttrProbeTests, maxWavesParsesAndResolvesAgainstAWave) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kWavesSrc, "test.W", "occwparse");
    auto attr = XpuKernelAttr::from(
        *findMethod(module->getStructures()["test.W"], "waves"));
    ASSERT_TRUE(attr.has_value());
    ASSERT_TRUE(attr->maxWaves().has_value());
    EXPECT_EQ(*attr->maxWaves(), 8u);
    EXPECT_FALSE(attr->maxThreads().has_value());
    EXPECT_TRUE(attr->hasOccupancy());
    EXPECT_EQ(attr->maxThreadsAt(32), std::optional<unsigned>(256u));
    EXPECT_EQ(attr->maxThreadsAt(64), std::optional<unsigned>(512u));
}

TEST(XpuOccupancyAttrProbeTests, maxWavesBoundsAnAmdKernelAtItsOwnWave) {
    llvm::LLVMContext ctx;
    llvm::Module dev32("occ_w32", ctx);
    std::string why;
    auto* fn32 = lowerAmd("waves", dev32, &why);
    ASSERT_NE(fn32, nullptr) << why;
    EXPECT_EQ(attrOf(fn32, "amdgpu-flat-work-group-size"), "1,256")
        << "8 waves at the default wave32";
    llvm::Module dev64("occ_w64", ctx);
    auto* fn64 = lowerAmd("waves64", dev64, &why);
    ASSERT_NE(fn64, nullptr) << why;
    EXPECT_EQ(attrOf(fn64, "amdgpu-flat-work-group-size"), "1,512")
        << "8 waves at a pinned wave64";
}

TEST(XpuOccupancyAttrProbeTests, maxThreadsAndMaxWavesTogetherAreRefused) {
    llvm::LLVMContext ctx;
    llvm::Module dev("occ_both", ctx);
    std::string why;
    auto* fn = lowerAmd("both", dev, &why);
    EXPECT_EQ(fn, nullptr);
    EXPECT_NE(why.find("maxWaves"), std::string::npos) << why;
}
