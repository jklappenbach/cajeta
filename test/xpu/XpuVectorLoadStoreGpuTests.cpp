//
// Kernel-aware vectorized load/store on the GPU backends (kernel-vector-
// loadstore plan, Unit 5: NVPTX + AMD). The vload/vstore interception lives in
// the backend-neutral lowerBuiltinCall, and NVPTX/AMD inherit the default
// LoweringTarget::vectorLoad/vectorStore (bufferElementPtr GEP + packed
// `<N x T>` memory op). These emit-only tests prove the same kernel source
// lowers to valid device code on both — no GPU required (PTX/ISA is text).
//
// Note on native wide loads (spec §6.1.2/6.1.3): the packed `<N x T>` load is
// emitted with the element's natural alignment (correct for any index); the
// backend fuses it into a hardware vector load (ld.global.v4 / global_load_*x4)
// when it can prove vector alignment, and otherwise legalizes to the minimum
// number of native-width ops. Correctness is alignment-independent; fusion is a
// backend perf detail. Here we assert valid lowering (entry + global load/store).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// float32 x 4 — load an 4-block, double it, store it. One thread per 4-block.
const char* kVaddSource =
    "package test;\n"
    "import cajeta.gpu.KernelBuffer;\n"
    "import cajeta.gpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void vadd(KernelBuffer<float32> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 4;\n"
    "        Vector<float32,4> v = c.vload<4>(base);\n"
    "        c.vstore(base, v + v);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vlsgpu_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vlsgpu_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

} // namespace

// 5.x — NVPTX: a vload/vstore kernel lowers to valid PTX with a global
// load + store (the buffer access lowered through the vector seam).
TEST(XpuVectorLoadStoreGpuTests, lowersToValidNvptx) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVaddSource);
    auto k = findMethod(module->getStructures()["test.M"], "vadd");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vls_nvptx", deviceCtx);
    cajeta::xpu::nvidia::configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(k, deviceModule), nullptr);

    std::string ptx = cajeta::xpu::nvidia::emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    EXPECT_NE(ptx.find(".visible .entry vadd"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("ld.global"), std::string::npos) << ptx;
    EXPECT_NE(ptx.find("st.global"), std::string::npos) << ptx;
}

// 5.x — AMD: the same kernel lowers to valid gfx ISA with global memory ops.
TEST(XpuVectorLoadStoreGpuTests, lowersToValidAmdgpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVaddSource);
    auto k = findMethod(module->getStructures()["test.M"], "vadd");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vls_amd", deviceCtx);
    cajeta::xpu::amd::configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, deviceModule), nullptr);

    std::string isa = cajeta::xpu::amd::emitIsa(deviceModule, *tm);
    ASSERT_FALSE(isa.empty());
    EXPECT_NE(isa.find("vadd"), std::string::npos);
    EXPECT_NE(isa.find("global_load"), std::string::npos) << isa;
    EXPECT_NE(isa.find("global_store"), std::string::npos) << isa;
}
