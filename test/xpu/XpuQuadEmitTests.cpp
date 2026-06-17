//
// CajetaXPU quad (2x2) ops — emit + spirv-val (GPU-free).
//
// Quad.broadcast/swap{Horizontal,Vertical,Diagonal}/all/any lower, on the
// Vulkan/Shader flavor, to the native quad opcodes via the fork llvm.spv.quad.*
// intrinsics: OpGroupNonUniformQuadBroadcast / QuadSwap are core SPIR-V
// (GroupNonUniformQuad); OpGroupNonUniformQuad{All,Any}KHR are
// SPV_KHR_quad_control (a quad-wide vote, no Scope operand). NVPTX/AMDGPU/CPU
// inherit the base defaults (laneId/ballot arithmetic over the wave seams).
//
// This asserts the device IR carries the intrinsics, the SPIR-V text carries the
// ops + capability + extension, and the binary is strictly spirv-val-valid.
// On-device verification is XpuQuadDeviceTests.
//

#include "gtest/gtest.h"

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

// Exercise every quad op: broadcast lane 0's value across the quad, swap it the
// three ways, then quad-vote a predicate. All four distinct opcodes (broadcast,
// swap, all, any) appear in one spirv-val-clean module.
const char* kQuadSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.GpuThread;\n"
    "import cajeta.gpu.Quad;\n"
    "public class Q {\n"
    "    @Kernel\n"
    "    public static void quadops(GpuBuffer<uint32> out) {\n"
    "        uint32 t = GpuThread.x();\n"
    "        uint32 b = Quad.broadcast(t, 0);\n"
    "        uint32 h = Quad.swapHorizontal(b);\n"
    "        uint32 v = Quad.swapVertical(h);\n"
    "        uint32 d = Quad.swapDiagonal(v);\n"
    "        uint32 r = d;\n"
    "        boolean p = t > 0;\n"
    "        if (Quad.all(p)) {\n"
    "            r = r + 1;\n"
    "        }\n"
    "        if (Quad.any(p)) {\n"
    "            r = r + 2;\n"
    "        }\n"
    "        out[t] = r;\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_quad_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "Q.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_quad_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "Q.cajeta";
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

} // namespace

TEST(XpuQuadEmitTests, spirvLowersQuadOpsAndValidates) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kQuadSource);
    auto k = findMethod(module->getStructures()["test.Q"], "quadops");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    // (1) Device IR carries the four fork intrinsics.
    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_quad_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("llvm.spv.quad.broadcast"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.quad.swap"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.quad.all"), std::string::npos) << ir;
    EXPECT_NE(ir.find("llvm.spv.quad.any"), std::string::npos) << ir;

    // (2) SPIR-V text carries the ops + the capability + the extension.
    llvm::LLVMContext txtCtx;
    llvm::Module txtMod("xpu_quad_txt", txtCtx);
    cajeta::xpu::vulkan::configureDeviceModule(txtMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, txtMod);
    std::string text = cajeta::xpu::vulkan::emitSpirvText(txtMod, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpCapability GroupNonUniformQuad"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpCapability QuadControlKHR"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpExtension \"SPV_KHR_quad_control\""),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformQuadBroadcast"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformQuadSwap"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformQuadAllKHR"),
              std::string::npos) << text;
    EXPECT_NE(text.find("OpGroupNonUniformQuadAnyKHR"),
              std::string::npos) << text;

    // (3) The binary is strictly spirv-val-valid.
    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_quad_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_quad_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the quad-ops module";
}
