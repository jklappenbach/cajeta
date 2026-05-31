//
// CajetaXPU wave ops (@Wave) — the seam's headline test across all THREE
// backends.
//
// Wave-level ops (warp / wavefront / subgroup) are the archetypal variance-
// shaped feature (cajeta-amd.md §2): every backend has the hardware, but the
// intrinsic, lane width, and ballot shape all diverge. The SAME Cajeta source
// (Wave.shuffleSync / Wave.ballotSync) lowers through the shared AST walk with
// only the three new LoweringTarget wave methods forking:
//   NVPTX:  llvm.nvvm.shfl.sync.idx.i32 / llvm.nvvm.vote.ballot.sync
//   AMDGPU: llvm.amdgcn.readlane / llvm.amdgcn.ballot
//   SPIR-V: llvm.spv.wave.readlane / llvm.spv.wave.ballot (→ OpGroupNonUniform*)
//
// GPU-free: asserts the device IR carries the right per-backend intrinsics, and
// that the Vulkan module is strictly spirv-val-valid. On-device verification is
// XpuWaveDeviceTests.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// out[t] = readlane(t*10+5, 3) + (u32) ballot(t < 4).
// Per-wave (block <= wave width): every lane reads lane 3's value (35) and the
// same ballot mask (low 4 bits → 0xF=15), so the device result is 50.
const char* kWaveSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "import cajeta.xpu.core.Wave;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void wavetest(Buffer<uint32> out) {\n"
    "        uint32 t = Thread.x();\n"
    "        uint32 r = Wave.shuffleSync(t * 10 + 5, 3);\n"
    "        uint64 b = Wave.ballotSync(t < 4);\n"
    "        out[t] = r + (uint32) b;\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_wave_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_wave_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

cajeta::MethodPtr compileWaveKernel(Compiler& compiler) {
    auto module = compileForInspection(compiler, kWaveSource);
    return findMethod(module->getStructures()["test.M"], "wavetest");
}

} // namespace

// NVPTX: wave ops lower to the warp-shuffle + vote-ballot intrinsics.
TEST(XpuWaveEmitTests, nvptxLowersShuffleAndBallot) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::nvidia::createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_wave_nvptx", ctx);
    cajeta::xpu::nvidia::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::nvidia::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.nvvm.shfl.sync.idx.i32"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.nvvm.vote.ballot.sync"), std::string::npos) << ir;
}

// AMDGPU: wave ops lower to readlane + ballot.
TEST(XpuWaveEmitTests, amdgpuLowersReadlaneAndBallot) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::amd::createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("xpu_wave_amdgpu", ctx);
    cajeta::xpu::amd::configureDeviceModule(m, *tm);
    ASSERT_NE(cajeta::xpu::amd::lowerKernel(k, m), nullptr);
    std::string ir = printModule(m);
    EXPECT_NE(ir.find("llvm.amdgcn.readlane"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.amdgcn.ballot"), std::string::npos) << ir;
}

// SPIR-V: wave ops lower to the subgroup intrinsics, and the module is valid.
TEST(XpuWaveEmitTests, spirvLowersSubgroupOpsAndValidates) {
    Compiler compiler;
    auto k = compileWaveKernel(compiler);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_wave_spirv_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.wave.readlane"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.wave.ballot"), std::string::npos) << ir;

    // Fresh module for emission (it mutates), then spirv-val.
    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_wave_spirv_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());

    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_wave_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    // path::c_str() is const wchar_t* on Windows; go through string() so
    // the StringRef has a char buffer to bind to (held until after the wait).
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the wave-ops module";
}
