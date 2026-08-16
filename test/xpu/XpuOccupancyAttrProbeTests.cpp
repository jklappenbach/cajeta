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

