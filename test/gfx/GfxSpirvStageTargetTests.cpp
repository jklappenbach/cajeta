//
// GfxSpirvStageTargetTests — cajeta-gfx plan §4.a, the per-stage SPIR-V
// TargetMachine knob in cajeta.xpu.vulkan.SpirvBackend.
//
// GfxSpirvEmitProbeTests proved the in-tree LLVM SPIR-V backend can emit valid
// vertex/fragment modules, but via a LOCAL makeStageTargetMachine helper because
// createSpirvTargetMachine hardcodes the compute triple. §4.a productizes that:
// SpirvBackend now exposes ShaderStage + spirvStageTriple / hlslShaderAttr /
// createSpirvTargetMachineForStage / configureDeviceModuleForStage. These tests
// exercise the OFFICIAL knob — the stage→triple/attr mapping for every stage, TM
// resolution for the §4.a stages, and a spirv-val-clean vertex + fragment module
// emitted entirely through the new API.
//
// GPU-free (SPIR-V is bytes; spirv-val is a host tool, skipped if absent).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

using namespace cajeta::xpu::vulkan;

namespace {

// One !spirv.Decorations entry: {i32 decoration, i32 operand}.
llvm::MDNode* decoration(llvm::LLVMContext& ctx, uint32_t decor, uint32_t operand) {
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Metadata* ops[2] = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, decor)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, operand)),
    };
    return llvm::MDNode::get(ctx, ops);
}

// An interface variable — an Input(7)/Output(8) module global decorated BuiltIn
// or Location (matches LLVM's position.{vs,ps}.ll, as in the emit probe).
llvm::GlobalVariable* interfaceVar(llvm::Module& m, llvm::Type* ty,
                                   unsigned addrSpace, bool isInput,
                                   const char* name, llvm::MDNode* decor) {
    auto* gv = new llvm::GlobalVariable(
        m, ty, /*isConstant=*/isInput, llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr, name, /*InsertBefore=*/nullptr,
        llvm::GlobalValue::GeneralDynamicTLSModel, addrSpace,
        /*isExternallyInitialized=*/true);
    gv->setVisibility(llvm::GlobalValue::HiddenVisibility);
    gv->setMetadata("spirv.Decorations",
                    llvm::MDNode::get(m.getContext(),
                                      llvm::ArrayRef<llvm::Metadata*>{decor}));
    return gv;
}

// Run spirv-val if installed; nullopt when absent so the caller can skip.
std::optional<bool> validateSpirv(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_stage_" + std::to_string(rng()) + ".spv");
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

constexpr uint32_t kBuiltIn = 11;
constexpr uint32_t kLocation = 30;
constexpr uint32_t kPosition = 0;
constexpr uint32_t kFragCoord = 15;
constexpr unsigned kInputSC = 7;
constexpr unsigned kOutputSC = 8;

} // namespace

// 4a — the per-stage triple + hlsl.shader mapping is the knob's core. Every
// stage maps to "spirv-unknown-vulkan1.3-<env>" with the LLVM/HLSL stage env,
// and hlslShaderAttr returns the matching execution-model token. (Compute equals
// the legacy kSpirvTriple.)
TEST(GfxSpirvStageTargetTests, stageTriplesAndAttrsAreCorrect) {
    EXPECT_EQ(spirvStageTriple(ShaderStage::Compute), "spirv-unknown-vulkan1.3-compute");
    EXPECT_EQ(spirvStageTriple(ShaderStage::Vertex), "spirv-unknown-vulkan1.3-vertex");
    EXPECT_EQ(spirvStageTriple(ShaderStage::Fragment), "spirv-unknown-vulkan1.3-pixel");
    EXPECT_EQ(spirvStageTriple(ShaderStage::Geometry), "spirv-unknown-vulkan1.3-geometry");
    EXPECT_EQ(spirvStageTriple(ShaderStage::TessControl), "spirv-unknown-vulkan1.3-hull");
    EXPECT_EQ(spirvStageTriple(ShaderStage::TessEval), "spirv-unknown-vulkan1.3-domain");
    EXPECT_EQ(spirvStageTriple(ShaderStage::Mesh), "spirv-unknown-vulkan1.3-mesh");
    EXPECT_EQ(spirvStageTriple(ShaderStage::Task), "spirv-unknown-vulkan1.3-amplification");

    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Compute), "compute");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Vertex), "vertex");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Fragment), "pixel");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Geometry), "geometry");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::TessControl), "hull");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::TessEval), "domain");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Mesh), "mesh");
    EXPECT_STREQ(hlslShaderAttr(ShaderStage::Task), "amplification");

    // The arch knob flows into the triple.
    EXPECT_EQ(spirvStageTriple(ShaderStage::Vertex, "vulkan1.2"),
              "spirv-unknown-vulkan1.2-vertex");
}

// 4a — the §4.a stages resolve to a usable SPIR-V TargetMachine through the
// official knob (the precondition for emitting their modules).
TEST(GfxSpirvStageTargetTests, computeVertexFragmentTargetMachinesResolve) {
    EXPECT_NE(createSpirvTargetMachineForStage(ShaderStage::Compute), nullptr);
    EXPECT_NE(createSpirvTargetMachineForStage(ShaderStage::Vertex), nullptr);
    EXPECT_NE(createSpirvTargetMachineForStage(ShaderStage::Fragment), nullptr);
}

// 4a — a vertex module built entirely through the new knob
// (createSpirvTargetMachineForStage + configureDeviceModuleForStage +
// hlslShaderAttr) emits a spirv-val-clean OpEntryPoint Vertex module.
TEST(GfxSpirvStageTargetTests, vertexModuleEmitsThroughKnob) {
    auto tm = createSpirvTargetMachineForStage(ShaderStage::Vertex);
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_stage_vertex", ctx);
    configureDeviceModuleForStage(m, *tm, ShaderStage::Vertex);

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* inPos = interfaceVar(m, v4f, kInputSC, true, "in_position",
                               decoration(ctx, kLocation, 0));
    auto* outPos = interfaceVar(m, v4f, kOutputSC, false, "gl_Position",
                                decoration(ctx, kBuiltIn, kPosition));

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "main", &m);
    fn->addFnAttr("hlsl.shader", hlslShaderAttr(ShaderStage::Vertex));
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* pos = b.CreateLoad(v4f, inPos, "pos");
    b.CreateStore(pos, outPos);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the knob-emitted vertex module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// 4a — a fragment module built through the knob emits a spirv-val-clean
// OpEntryPoint Fragment module.
TEST(GfxSpirvStageTargetTests, fragmentModuleEmitsThroughKnob) {
    auto tm = createSpirvTargetMachineForStage(ShaderStage::Fragment);
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_stage_fragment", ctx);
    configureDeviceModuleForStage(m, *tm, ShaderStage::Fragment);

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* fragCoord = interfaceVar(m, v4f, kInputSC, true, "gl_FragCoord",
                                   decoration(ctx, kBuiltIn, kFragCoord));
    auto* outColor = interfaceVar(m, v4f, kOutputSC, false, "out_color",
                                  decoration(ctx, kLocation, 0));

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, "main", &m);
    fn->addFnAttr("hlsl.shader", hlslShaderAttr(ShaderStage::Fragment));
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* c = b.CreateLoad(v4f, fragCoord, "c");
    b.CreateStore(c, outColor);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn FragCoord"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the knob-emitted fragment module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}
