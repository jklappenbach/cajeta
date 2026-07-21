//
// cajeta-gfx bring-up — SPIR-V GRAPHICS emitter probe (G1.0, increment 0).
//
// Gating fact for the entire graphics plan: today the SPIR-V backend is
// compute-only (every entry is `OpEntryPoint GLCompute`, stamped by
// hlsl.shader="compute"). Before building a vertex/fragment front-end we must
// know the SAME in-tree LLVM SPIR-V backend — no glslang, no SPIRV-Tools
// assembler — can emit a valid graphics shader pair. This probe proves it.
//
// It hand-builds the two minimal graphics modules directly in LLVM IR (there is
// no Cajeta graphics front-end yet — that is the next increment), mirroring
// LLVM's own authoritative patterns (llvm/test/CodeGen/SPIRV/semantics/
// position.{vs,ps}.ll and ExecutionMode_Vertex.ll). The graphics seam vs the
// compute path is three things, all confirmed here:
//
//   1. Execution model: hlsl.shader="vertex"/"pixel" + a per-STAGE triple
//      environment (spirv-unknown-vulkan1.3-{vertex,pixel}) → OpEntryPoint
//      Vertex / Fragment. The stage rides the triple, so each stage is its own
//      module / .spv — exactly how Vulkan binds shader stages.
//   2. Shader I/O: interface variables are module globals in addrspace 7
//      (Input) / addrspace 8 (Output), carrying !spirv.Decorations metadata —
//      {i32 11, i32 0} = BuiltIn Position, {i32 11, i32 15} = BuiltIn FragCoord,
//      {i32 30, i32 N} = Location N. (Compute has no interface vars; args arrive
//      via descriptors.)
//   3. The existing emitSpirvText / emitSpirv path carries graphics modules
//      unchanged — emitSpirvText runs no compute-specific post-passes, and the
//      binary's LocalSize/barrier fixups are documented no-ops when absent.
//
// GPU-free (SPIR-V is just bytes; spirv-val is a host tool). When the graphics
// front-end lands, these raw-IR builders get replaced by lowering a Cajeta
// @Vertex / @Fragment shader — but the emitter contract proven here is the same.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/TargetRegistry.h"
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

// Build a SPIR-V TargetMachine for an arbitrary per-stage graphics triple
// (e.g. spirv-unknown-vulkan1.3-vertex). createSpirvTargetMachine hardcodes the
// compute triple, so we can't reuse it directly — but calling it first forces
// the project's own spirv target-registry initialization, after which a plain
// lookupTarget on the graphics triple succeeds. Mirrors createSpirvTargetMachine.
std::unique_ptr<llvm::TargetMachine> makeStageTargetMachine(const char* tripleStr) {
    auto warm = createSpirvTargetMachine("vulkan1.3");  // ensures targets registered
    if (!warm) return nullptr;
    llvm::Triple triple{llvm::StringRef(tripleStr)};
    std::string error;
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;
    llvm::TargetOptions opt;
    llvm::TargetMachine* tm =
        target->createTargetMachine(triple, /*CPU=*/"", /*Features=*/"", opt,
                                    /*RM=*/std::nullopt);
    return std::unique_ptr<llvm::TargetMachine>(tm);
}

// One !spirv.Decorations entry: {i32 decoration, i32 operand}. The global's
// metadata is the outer node wrapping one-or-more of these.
llvm::MDNode* decoration(llvm::LLVMContext& ctx, uint32_t decor, uint32_t operand) {
    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::Metadata* ops[2] = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, decor)),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(i32, operand)),
    };
    return llvm::MDNode::get(ctx, ops);
}

// An interface variable — a module global in the Input (7) or Output (8)
// storage class, decorated as a BuiltIn or Location. Matches position.{vs,ps}.ll
// exactly: external hidden thread_local externally_initialized; Input vars are
// `constant` (read-only), Output vars are `global` (writable).
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

// Run spirv-val on a binary if installed; nullopt if the tool is absent so the
// caller can skip. Mirrors the helper in XpuVulkanEmitTests.
std::optional<bool> validateSpirv(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_spv_" + std::to_string(rng()) + ".spv");
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

constexpr uint32_t kBuiltIn = 11;   // SPIR-V Decoration::BuiltIn
constexpr uint32_t kLocation = 30;  // SPIR-V Decoration::Location
constexpr uint32_t kPosition = 0;   // SPIR-V BuiltIn::Position
constexpr uint32_t kFragCoord = 15; // SPIR-V BuiltIn::FragCoord

constexpr unsigned kInputSC = 7;    // addrspace → StorageClass::Input
constexpr unsigned kOutputSC = 8;   // addrspace → StorageClass::Output

} // namespace

// A minimal VERTEX shader — read a clip-space position from input location 0,
// write it to the Position builtin — emits a valid Vulkan vertex SPIR-V module:
// OpEntryPoint Vertex, a Location-0 Input var, and a BuiltIn Position Output var.
// (Pass-through positions: no Matrix dependency, so this probe does not block on
// gpu B1.) Proves the execution-model + interface-variable seam in one shot.
TEST(GfxSpirvEmitProbe, vertexShaderEmitsPositionOutput) {
    auto tm = makeStageTargetMachine("spirv-unknown-vulkan1.3-vertex");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_probe_vertex", ctx);
    m.setTargetTriple(llvm::Triple("spirv-unknown-vulkan1.3-vertex"));
    m.setDataLayout(tm->createDataLayout());

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* inPos = interfaceVar(m, v4f, kInputSC, /*isInput=*/true, "in_position",
                               decoration(ctx, kLocation, 0));
    auto* outPos = interfaceVar(m, v4f, kOutputSC, /*isInput=*/false, "gl_Position",
                                decoration(ctx, kBuiltIn, kPosition));

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      "main", &m);
    fn->addFnAttr("hlsl.shader", "vertex");
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* pos = b.CreateLoad(v4f, inPos, "pos");
    b.CreateStore(pos, outPos);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty()) << "vertex SPIR-V text emission produced nothing";
    EXPECT_NE(text.find("OpCapability Shader"), std::string::npos) << text;
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u) << "vertex SPIR-V binary emission produced nothing";
    EXPECT_EQ(spirv[0], 0x03u);  // 0x07230203 magic, little-endian
    EXPECT_EQ(spirv[1], 0x02u);
    EXPECT_EQ(spirv[2], 0x23u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the vertex module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// A minimal FRAGMENT shader — read FragCoord, write it to color output
// location 0 — emits a valid Vulkan fragment SPIR-V module: OpEntryPoint
// Fragment, a BuiltIn FragCoord Input var, and a Location-0 Output var.
TEST(GfxSpirvEmitProbe, fragmentShaderEmitsColorOutput) {
    auto tm = makeStageTargetMachine("spirv-unknown-vulkan1.3-pixel");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_probe_fragment", ctx);
    m.setTargetTriple(llvm::Triple("spirv-unknown-vulkan1.3-pixel"));
    m.setDataLayout(tm->createDataLayout());

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* fragCoord = interfaceVar(m, v4f, kInputSC, /*isInput=*/true, "gl_FragCoord",
                                   decoration(ctx, kBuiltIn, kFragCoord));
    auto* outColor = interfaceVar(m, v4f, kOutputSC, /*isInput=*/false, "out_color",
                                  decoration(ctx, kLocation, 0));

    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      "main", &m);
    fn->addFnAttr("hlsl.shader", "pixel");
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* c = b.CreateLoad(v4f, fragCoord, "c");
    b.CreateStore(c, outColor);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty()) << "fragment SPIR-V text emission produced nothing";
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn FragCoord"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u) << "fragment SPIR-V binary emission produced nothing";
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the fragment module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}
