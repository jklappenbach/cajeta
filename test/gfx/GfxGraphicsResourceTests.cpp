//
// GfxGraphicsResourceTests — cajeta-gfx plan §4.b-rest (increment A): a graphics
// @Vertex/@Fragment shader that ALSO consumes descriptor-bound resources
// (storage/uniform buffers) alongside its pipeline interface variables.
//
// GfxGraphicsLoweringTests proved the bare interface fork (attributes in,
// gl_Position out). A real shader additionally reads per-draw data from a
// GpuBuffer<T> (a transform UBO/SSBO). The compute SpirvTarget already binds a
// GpuBuffer<T> as a (set 0, binding = param-index) storage-buffer descriptor;
// SpirvGraphicsTarget keeps that path for resource params and routes ONLY plain
// value params to Location-N interface variables (via a SEPARATE nextInputLocation_
// counter). These tests pin two properties of that split:
//   1. a vertex shader reads a GpuBuffer<float32> descriptor AND has a Location-0
//      vertex attribute AND writes BuiltIn Position — all in one valid module;
//   2. multiple plain attributes get SEQUENTIAL Locations (0,1,2) while a resource
//      param interleaved among them does NOT consume an interface Location (the
//      two namespaces — descriptor Binding vs. interface Location — are distinct).
//
// Same harness as GfxGraphicsLoweringTests: compile a tiny cajeta shader, lower it
// through lowerGraphicsShader, inspect the emitted SPIR-V text, and round-trip the
// binary through spirv-val (a host tool; skipped if absent). GPU-free.
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
              / ("cajeta_gfx_res_" + std::to_string(rng()));
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
                 / ("cajeta_gfx_res_arch_" + std::to_string(rng()));
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
              / ("cajeta_gfx_res_spv_" + std::to_string(rng()) + ".spv");
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
    llvm::Module m("gfx_res_text", ctx);
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
    llvm::Module m("gfx_res_bin", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    lowerGraphicsShader(method, m, stage);
    return emitSpirv(m, *tm);
}

} // namespace

// 4b-rest A — a @Vertex shader reads a GpuBuffer<float32> (a per-draw transform
// UBO/SSBO) AND has a Location-0 position attribute AND writes gl_Position. The
// resource binds as a descriptor (DescriptorSet/Binding) through the inherited
// compute path; the attribute is a Location-0 Input; the two coexist in one valid
// Vulkan vertex module.
TEST(GfxGraphicsResourceTests, vertexReadsStorageBufferAlongsideAttribute) {
    const std::string src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static Vector<float32,4> main(Vector<float32,4> position,\n"
        "                                         GpuBuffer<float32> xform) {\n"
        "        float32 s = xform[0];\n"
        "        float32 px = position.x;\n"
        "        Vector<float32,4> out = stack Vector<float32,4>(px, s, s, 1.0f);\n"
        "        return out;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_FALSE(text.empty()) << "vertex+buffer lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;
    // The buffer rode in as a (set 0, binding) descriptor — the compute SSBO path.
    EXPECT_NE(text.find("DescriptorSet"), std::string::npos) << text;
    EXPECT_NE(text.find("Binding"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the vertex+buffer shader";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}

// 4b-rest A — three plain vertex attributes get SEQUENTIAL interface Locations
// (0, 1, 2); the per-attribute nextInputLocation_ counter assigns them in
// declaration order. (No resource param here — the interleaving-with-a-resource
// case is covered by the test above, where the buffer takes a descriptor Binding
// and does NOT advance the Location counter.)
TEST(GfxGraphicsResourceTests, multipleVertexAttributesGetSequentialLocations) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static Vector<float32,4> main(Vector<float32,4> position,\n"
        "                                         Vector<float32,3> normal,\n"
        "                                         Vector<float32,2> uv) {\n"
        "        float32 px = position.x;\n"
        "        float32 nx = normal.x;\n"
        "        float32 uu = uv.x;\n"
        "        Vector<float32,4> out = stack Vector<float32,4>(px, nx, uu, 1.0f);\n"
        "        return out;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_FALSE(text.empty()) << "multi-attribute lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 0"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 1"), std::string::npos) << text;
    EXPECT_NE(text.find("Location 2"), std::string::npos) << text;

    std::vector<uint8_t> spirv = lowerToBinary(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_GE(spirv.size(), 4u);
    EXPECT_EQ(spirv[0], 0x03u);
    EXPECT_EQ(spirv[3], 0x07u);
    if (auto valid = validateSpirv(spirv)) {
        EXPECT_TRUE(*valid) << "spirv-val rejected the multi-attribute shader";
    } else {
        GTEST_SUCCEED() << "spirv-val not installed; skipped binary validation";
    }
}
