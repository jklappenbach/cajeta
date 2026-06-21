//
// GfxGraphicsLoweringTests — cajeta-gfx plan §4.b (slice 2): lowering a cajeta
// @Vertex / @Fragment shader METHOD to a SPIR-V graphics module.
//
// XpuVulkanEmitTests lowers a @Kernel through the shared AST walk + the compute
// SpirvTarget; this is the rasterization twin — lowerGraphicsShader runs the SAME
// body walk through SpirvGraphicsTarget, which forks only the pipeline interface:
// a `void main()` entry with the stage execution model, params materialized as
// Input interface variables, and the `return` value stored into an Output
// interface variable (BuiltIn Position for a vertex stage, a Location color for a
// fragment stage). Each test compiles a tiny cajeta shader, lowers it, and
// asserts the emitted SPIR-V carries the stage + interface decorations, then
// round-trips the binary through spirv-val.
//
// GPU-free (SPIR-V is bytes; spirv-val is a host tool, skipped if absent).
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
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace cajeta::xpu::vulkan;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_lower_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_gfx_lower_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == name) return m;
    }
    return nullptr;
}

std::optional<bool> validateSpirv(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_lower_spv_" + std::to_string(rng()) + ".spv");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spirv.data()),
                  (std::streamsize) spirv.size());
    }
    std::string fileStr = path.string();
    llvm::SmallVector<llvm::StringRef, 4> args = {
        *tool, "--target-env", "vulkan1.3", fileStr};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    return rc == 0;
}

// Compile `src`, find class `fq` method `mname`, lower it for `stage`, and return
// the emitted SPIR-V text. Each emit gets its own fresh module + TargetMachine
// (emission mutates the module and accumulates TM state — see XpuVulkanEmitTests).
std::string lowerToText(const std::string& src, const std::string& fq,
                        const std::string& mname, ShaderStage stage,
                        bool* argFreeOut) {
    Compiler compiler;
    auto module = compileForInspection(compiler, src, fq);
    auto klass = module->getStructures()[fq];
    if (!klass) return {};
    auto method = findMethod(klass, mname);
    if (!method) return {};
    auto tm = createSpirvTargetMachineForStage(stage);
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_lower_text", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    llvm::Function* fn = lowerGraphicsShader(method, m, stage);
    if (argFreeOut) *argFreeOut = fn && fn->arg_size() == 0u;
    return emitSpirvText(m, *tm);
}

std::vector<uint8_t> lowerToBinary(const std::string& src, const std::string& fq,
                                   const std::string& mname, ShaderStage stage) {
    Compiler compiler;
    auto module = compileForInspection(compiler, src, fq);
    auto klass = module->getStructures()[fq];
    if (!klass) return {};
    auto method = findMethod(klass, mname);
    if (!method) return {};
    auto tm = createSpirvTargetMachineForStage(stage);
    if (!tm) return {};
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_lower_bin", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    lowerGraphicsShader(method, m, stage);
    return emitSpirv(m, *tm);
}

} // namespace

// 4b — a pass-through @Vertex shader (one position attribute → gl_Position)
// lowers from the cajeta front end to a valid Vulkan vertex SPIR-V module:
// a void main() entry (inputs are interface vars, not fn args), OpEntryPoint
// Vertex, a Location-0 Input attribute, and a BuiltIn Position Output.
TEST(GfxGraphicsLoweringTests, vertexPassThroughLowersToSpirv) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static Vector<float32,4> main(Vector<float32,4> position) {\n"
        "        return position;\n"
        "    }\n"
        "}\n";
    bool argFree = false;
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Vertex, &argFree);
    ASSERT_FALSE(text.empty()) << "vertex lowering produced no SPIR-V";
    EXPECT_TRUE(argFree) << "graphics entry must be void main() (interface vars, no args)";
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the lowered vertex shader";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// 4b — a pass-through @Fragment shader (one Location-0 varying → Location-0
// color output) lowers to a valid Vulkan fragment SPIR-V module: OpEntryPoint
// Fragment with Location-decorated Input and Output interface variables.
TEST(GfxGraphicsLoweringTests, fragmentPassThroughLowersToSpirv) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Fragment\n"
        "    public static Vector<float32,4> main(Vector<float32,4> varying) {\n"
        "        return varying;\n"
        "    }\n"
        "}\n";
    bool argFree = false;
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Fragment, &argFree);
    ASSERT_FALSE(text.empty()) << "fragment lowering produced no SPIR-V";
    EXPECT_TRUE(argFree);
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the lowered fragment shader";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}
