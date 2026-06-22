//
// GfxPushConstantTests — cajeta-gfx plan §4.b-rest (increment B-1): a graphics
// @Vertex/@Fragment shader's @PushConstant params lower to a single SPIR-V
// PushConstant block (the small per-draw uniforms).
//
// GfxGraphicsResourceTests covered the two existing param flavors: a plain value
// param → a per-vertex Location interface variable, a GpuBuffer<T> → a descriptor.
// A @PushConstant param is the third flavor: a uniform that is the SAME across
// every invocation, supplied by the host at draw time. Vulkan permits exactly ONE
// push-constant block per stage, so SpirvGraphicsTarget collects every
// @PushConstant param into one struct global in the PushConstant address space
// (AS 13); the fork's SPIRVPushConstantAccess pass + backend lay it out as a Block
// with member Offsets and rewrite each access into an OpAccessChain.
//
// These tests pin two properties on the emitted SPIR-V text:
//   1. a scalar @PushConstant rides a PushConstant block alongside a Location-0
//      varying input and a Location-0 color output;
//   2. TWO @PushConstant params share ONE block — both members are laid out in the
//      same struct (Offsets 0 and 4), not two separate blocks (which Vulkan forbids).
//
// Same harness as GfxGraphicsLoweringTests. spirv-val is a host tool (absent here —
// it runs on the device machine), so these assert the disassembly structurally and
// the device pass confirms full validity. GPU-free.
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
              / ("cajeta_gfx_pc_" + std::to_string(rng()));
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
                 / ("cajeta_gfx_pc_arch_" + std::to_string(rng()));
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

std::string lowerToText(const std::string& src, const std::string& fq,
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
    llvm::Module m("gfx_pc_text", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    lowerGraphicsShader(method, m, stage);
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
    llvm::Module m("gfx_pc_bin", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    lowerGraphicsShader(method, m, stage);
    return emitSpirv(m, *tm);
}

} // namespace

// 4b-rest B-1 — a scalar @PushConstant rides a PushConstant block while an
// ordinary value param stays a Location-0 varying input. The emitted fragment
// module carries OpEntryPoint Fragment, a Location-0 input + output, and a
// PushConstant block laid out from member 0 (Offset 0).
TEST(GfxPushConstantTests, fragmentScalarPushConstant) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Fragment\n"
        "    public static Vector<float32,4> main(Vector<float32,4> varying,\n"
        "                                         @PushConstant float32 alpha) {\n"
        "        float32 a = alpha;\n"
        "        float32 vx = varying.x;\n"
        "        Vector<float32,4> out = stack Vector<float32,4>(vx, a, a, 1.0f);\n"
        "        return out;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_FALSE(text.empty()) << "fragment+push-constant lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;
    // The uniform rode in as a PushConstant block (storage class + member 0 layout).
    EXPECT_NE(text.find("PushConstant"), std::string::npos) << text;
    EXPECT_NE(text.find("Offset 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
}

// 4b-rest B-1 — TWO @PushConstant params share ONE block: SpirvGraphicsTarget
// packs them as members 0 and 1 of a single struct (Offsets 0 and 4), not two
// separate PushConstant blocks (Vulkan permits exactly one per stage). Offset 4 on
// the second member is the evidence the block is shared.
TEST(GfxPushConstantTests, multiplePushConstantsShareOneBlock) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Fragment\n"
        "    public static Vector<float32,4> main(@PushConstant float32 r,\n"
        "                                         @PushConstant float32 g) {\n"
        "        Vector<float32,4> out = stack Vector<float32,4>(r, g, 0.0f, 1.0f);\n"
        "        return out;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_FALSE(text.empty()) << "two-push-constant lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("PushConstant"), std::string::npos) << text;
    // Two members packed in one block: f32 at offset 0, f32 at offset 4.
    EXPECT_NE(text.find("Offset 0"), std::string::npos) << text;
    EXPECT_NE(text.find("Offset 4"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
}
