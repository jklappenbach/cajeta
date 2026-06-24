//
// GfxSpirvInterfaceTests — cajeta-gfx plan §4.b, the SPIR-V shader interface
// variable primitive (cajeta.xpu.vulkan.SpirvInterface).
//
// GfxSpirvEmitProbeTests proved (with LOCAL lambdas) that interface variables —
// Input/Output globals in addrspace 7/8 with !spirv.Decorations Location/BuiltIn
// — emit spirv-val-clean. §4.b productizes that into SpirvInterface
// (createLocationVar / createBuiltInVar with typed InterfaceStorage / SpirvBuiltIn
// enums), the API the graphics body-lowering will call. These tests build vertex
// and fragment modules entirely through that production API + the §4.a per-stage
// knob, and round-trip them through spirv-val.
//
// GPU-free (SPIR-V is bytes; spirv-val is a host tool, skipped if absent).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvInterface.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

using namespace cajeta::xpu::vulkan;

namespace {

std::optional<bool> validateSpirv(const std::vector<uint8_t>& spirv) {
    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) return std::nullopt;
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_iface_" + std::to_string(rng()) + ".spv");
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

// A void main() entry for `stage`, with the hlsl.shader execution-model attr.
llvm::Function* makeEntry(llvm::Module& m, ShaderStage stage) {
    llvm::LLVMContext& ctx = m.getContext();
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(ctx), false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage,
                                      "main", &m);
    fn->addFnAttr("hlsl.shader", hlslShaderAttr(stage));
    return fn;
}

} // namespace

// 4b — a vertex stage's interface: a Location-0 Input attribute and the BuiltIn
// Position Output, both built through the production SpirvInterface API, emit a
// spirv-val-clean OpEntryPoint Vertex module carrying those decorations.
TEST(GfxSpirvInterfaceTests, vertexInterfaceVarsRoundTrip) {
    auto tm = createSpirvTargetMachineForStage(ShaderStage::Vertex);
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_iface_vertex", ctx);
    configureDeviceModuleForStage(m, *tm, ShaderStage::Vertex);

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* inPos = createLocationVar(m, v4f, InterfaceStorage::Input, 0, "in_position");
    auto* outPos = createBuiltInVar(m, v4f, InterfaceStorage::Output,
                                    SpirvBuiltIn::Position, "gl_Position");

    auto* fn = makeEntry(m, ShaderStage::Vertex);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* pos = b.CreateLoad(v4f, inPos, "pos");
    b.CreateStore(pos, outPos);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the vertex interface module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// 4b — a fragment stage's interface: the BuiltIn FragCoord Input and a Location-0
// color Output, built through the production API, emit a spirv-val-clean
// OpEntryPoint Fragment module.
TEST(GfxSpirvInterfaceTests, fragmentInterfaceVarsRoundTrip) {
    auto tm = createSpirvTargetMachineForStage(ShaderStage::Fragment);
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module m("gfx_iface_fragment", ctx);
    configureDeviceModuleForStage(m, *tm, ShaderStage::Fragment);

    auto* v4f = llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 4);
    auto* fragCoord = createBuiltInVar(m, v4f, InterfaceStorage::Input,
                                       SpirvBuiltIn::FragCoord, "gl_FragCoord");
    auto* outColor = createLocationVar(m, v4f, InterfaceStorage::Output, 0, "out_color");

    auto* fn = makeEntry(m, ShaderStage::Fragment);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* c = b.CreateLoad(v4f, fragCoord, "c");
    b.CreateStore(c, outColor);
    b.CreateRetVoid();

    std::string text = emitSpirvText(m, *tm);
    ASSERT_FALSE(text.empty());
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn FragCoord"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;

    std::vector<uint8_t> spirv = emitSpirv(m, *tm);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the fragment interface module";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}
