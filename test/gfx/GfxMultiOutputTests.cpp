//
// GfxMultiOutputTests — cajeta-gfx plan §4.b-rest (increment B-2): a graphics
// shader emits MORE THAN ONE output by returning a @ValueType struct.
//
// A pass-through shader returns one value → one output (GfxGraphicsLoweringTests).
// A real shader emits several: a vertex stage writes gl_Position AND inter-stage
// varyings; a fragment stage may write several color render targets (MRT). The
// cajeta surface for that is a @ValueType struct return — and storeShaderOutput
// decomposes the returned aggregate (extractvalue per field) into one Output
// interface variable per field. Positional mapping: vertex field 0 → BuiltIn
// Position, fields 1.. → sequential Location varyings; fragment fields → sequential
// Location color targets.
//
// (Enabled by extending deviceStructInfo to accept Vector/Matrix struct FIELDS —
// the canonical output struct is {vec4 position, vec2 uv, ...}.)
//
// Same harness as GfxGraphicsLoweringTests; spirv-val runs on the device machine,
// so these assert the SPIR-V text structurally (here: counting the OpVariable
// Output declarations). GPU-free.
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
              / ("cajeta_gfx_mo_" + std::to_string(rng()));
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
                 / ("cajeta_gfx_mo_arch_" + std::to_string(rng()));
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

// Count the OpVariable declarations in the given storage class. A disassembled
// interface variable line is `%name = OpVariable %_ptr_<SC>_<ty> <SC>`, so the
// storage-class token appears on the line for both its pointer type and the
// variable's storage class — and an Input line never contains "Output" (its type
// is _ptr_Input_…), so matching the SC word per OpVariable line counts cleanly.
size_t countOpVariables(const std::string& text, const std::string& storageClass) {
    size_t n = 0, pos = 0;
    while ((pos = text.find("OpVariable", pos)) != std::string::npos) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(
            pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (line.find(storageClass) != std::string::npos) ++n;
        pos = (eol == std::string::npos) ? text.size() : eol + 1;
    }
    return n;
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
    llvm::Module m("gfx_mo_text", ctx);
    configureDeviceModuleForStage(m, *tm, stage);
    lowerGraphicsShader(method, m, stage);
    return emitSpirvText(m, *tm);
}

} // namespace

// 4b-rest B-2 — a @Vertex shader returns a {vec4 position, vec2 uv} struct: field 0
// drives BuiltIn Position, field 1 a Location-0 Output varying. The module carries
// TWO Output interface variables (the single-return path emits one).
TEST(GfxMultiOutputTests, vertexStructReturnSplitsPositionAndVarying) {
    const std::string src =
        "package test;\n"
        "@ValueType\n"
        "public class VsOut {\n"
        "    Vector<float32,4> position;\n"
        "    Vector<float32,2> uv;\n"
        "    public VsOut(Vector<float32,4> position, Vector<float32,2> uv) {\n"
        "        this.position = position;\n"
        "        this.uv = uv;\n"
        "    }\n"
        "}\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static VsOut main(Vector<float32,4> inPos, Vector<float32,2> inUv) {\n"
        "        VsOut o = stack VsOut(inPos, inUv);\n"
        "        return o;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Vertex);
    ASSERT_FALSE(text.empty()) << "vertex struct-return lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Vertex"), std::string::npos) << text;
    EXPECT_NE(text.find("BuiltIn Position"), std::string::npos) << text;
    // Two outputs: BuiltIn Position (field 0) + a Location-0 varying (field 1).
    EXPECT_EQ(countOpVariables(text, "Output"), 2u) << text;
    // Two vertex attributes in (inPos @ Location 0, inUv @ Location 1).
    EXPECT_EQ(countOpVariables(text, "Input"), 2u) << text;
}

// 4b-rest B-2 — a @Fragment shader returns a {vec4 color0, vec4 color1} struct:
// two color render targets at Location 0 and Location 1 (MRT). With a single
// varying input, the two Location-decorated Outputs are the only color targets,
// so two Output OpVariables is the MRT proof.
TEST(GfxMultiOutputTests, fragmentStructReturnIsMultipleRenderTargets) {
    const std::string src =
        "package test;\n"
        "@ValueType\n"
        "public class FsOut {\n"
        "    Vector<float32,4> color0;\n"
        "    Vector<float32,4> color1;\n"
        "    public FsOut(Vector<float32,4> color0, Vector<float32,4> color1) {\n"
        "        this.color0 = color0;\n"
        "        this.color1 = color1;\n"
        "    }\n"
        "}\n"
        "public class S {\n"
        "    @Fragment\n"
        "    public static FsOut main(Vector<float32,4> varying) {\n"
        "        FsOut o = stack FsOut(varying, varying);\n"
        "        return o;\n"
        "    }\n"
        "}\n";
    std::string text = lowerToText(src, "test.S", "main", ShaderStage::Fragment);
    ASSERT_FALSE(text.empty()) << "fragment struct-return lowering produced no SPIR-V";
    EXPECT_NE(text.find("OpEntryPoint Fragment"), std::string::npos) << text;
    // Two color targets: Output Location 0 and Location 1.
    EXPECT_EQ(countOpVariables(text, "Output"), 2u) << text;
    EXPECT_NE(text.find("Location 1"), std::string::npos) << text;
    // A fragment has no Position builtin — its outputs are Location color targets.
    EXPECT_EQ(text.find("BuiltIn Position"), std::string::npos) << text;
}
