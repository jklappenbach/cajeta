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
#include "cajeta/xpu/core/KernelTuner.h"

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

const char* kAutotuneSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Autotune(blocks = {64, 128, 256})\n"
    "    public static void tuned(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        while (i < n) { c[i] = a[i] + 1.0f; i = i + 256; }\n"
    "    }\n"
    "    @Kernel\n"
    "    @Autotune\n"
    "    public static void tunedDefault(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        while (i < n) { c[i] = a[i] + 1.0f; i = i + 256; }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void notTuned(KernelBuffer<float32> c, KernelBuffer<float32> a, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        if (i < n) { c[i] = a[i] + 1.0f; }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_occatt_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_occatt_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

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
TEST(XpuOccupancyAttrProbeTests, parsesPortableParams) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kOccSrc);
    auto occ = findMethod(module->getStructures()["test.M"], "occ");
    ASSERT_NE(occ, nullptr);
    auto attr = XpuKernelAttr::from(*occ);
    ASSERT_TRUE(attr.has_value());
    EXPECT_TRUE(attr->hasOccupancy());
    ASSERT_TRUE(attr->maxThreads().has_value());
    EXPECT_EQ(*attr->maxThreads(), 256u);
    ASSERT_TRUE(attr->minResident().has_value());
    EXPECT_EQ(*attr->minResident(), 2u);
    ASSERT_TRUE(attr->maxRegisters().has_value());
    EXPECT_EQ(*attr->maxRegisters(), 128u);

    auto plain = findMethod(module->getStructures()["test.M"], "plain");
    ASSERT_NE(plain, nullptr);
    auto pattr = XpuKernelAttr::from(*plain);
    ASSERT_TRUE(pattr.has_value());
    EXPECT_FALSE(pattr->hasOccupancy());
}

// 2.1.b — AMDGPU lowering maps the override to the function attributes; an
// un-annotated kernel gets none from the override path.
TEST(XpuOccupancyAttrProbeTests, amdgpuLowersOverride) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kOccSrc);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_TRUE(tm != nullptr);

    {
        llvm::LLVMContext ctx;
        llvm::Module dev("occ_isa", ctx);
        configureDeviceModule(dev, *tm);
        auto* fn = cajeta::xpu::amd::lowerKernel(
            findMethod(module->getStructures()["test.M"], "occ"), dev);
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(attrOf(fn, "amdgpu-flat-work-group-size"), "1,256");
        EXPECT_EQ(attrOf(fn, "amdgpu-waves-per-eu"), "2");
    }
    {
        llvm::LLVMContext ctx;
        llvm::Module dev("plain_isa", ctx);
        configureDeviceModule(dev, *tm);
        auto* fn = cajeta::xpu::amd::lowerKernel(
            findMethod(module->getStructures()["test.M"], "plain"), dev);
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(attrOf(fn, "amdgpu-flat-work-group-size"), "<none>")
            << "no @Occupancy -> the override path sets nothing";
    }
}

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

// 3b — @Autotune parses (explicit + default candidate sets) and the compile-time
// bound is the LARGEST candidate (so every swept block is a legal launch).
TEST(XpuOccupancyAttrProbeTests, autotuneParsesAndBoundsToMaxCandidate) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAutotuneSrc);
    auto& cls = module->getStructures()["test.M"];

    auto tuned = XpuKernelAttr::from(*findMethod(cls, "tuned"));
    ASSERT_TRUE(tuned.has_value());
    EXPECT_TRUE(tuned->autotune());
    ASSERT_EQ(tuned->autotuneBlocks().size(), 3u);
    EXPECT_EQ(tuned->autotuneBlocks()[0], 64u);
    EXPECT_EQ(tuned->autotuneBlocks()[2], 256u);
    EXPECT_EQ(tuned->autotuneMaxThreads(), 256u) << "bound = largest candidate";

    // Bare @Autotune -> the tuner's default candidates; bound = their max.
    auto def = XpuKernelAttr::from(*findMethod(cls, "tunedDefault"));
    ASSERT_TRUE(def.has_value());
    EXPECT_TRUE(def->autotune());
    EXPECT_TRUE(def->autotuneBlocks().empty());
    EXPECT_EQ(def->autotuneMaxThreads(),
              cajeta::xpu::KernelTuner::defaultCandidateBlocks().back());

    auto plain = XpuKernelAttr::from(*findMethod(cls, "notTuned"));
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(plain->autotune());
    EXPECT_EQ(plain->autotuneMaxThreads(), 0u);
}
