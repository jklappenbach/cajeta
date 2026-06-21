//
// GfxGraphicsRegistrationTests — cajeta-gfx plan §4 (registration): auto-emit
// and register @Vertex/@Fragment shaders during compilation, the rasterization
// parallel of VulkanRegistration's @Kernel pass.
//
// emitKernelRegistration embeds each @Kernel's SPIR-V binary as a private host
// constant and appends an llvm.global_ctors entry calling the backend-neutral
// __cajeta_xpu_register_module(name, bytes, len). emitGraphicsRegistration is
// the graphics twin: it walks the graphics-shader methods (isGraphicsShader),
// lowers each through lowerGraphicsShader on its per-stage TargetMachine, and
// embeds + registers the resulting module the same way — so a @Vertex/@Fragment
// shader rides the normal `cajeta build` exactly like a @Kernel does.
//
// This test compiles a tiny cajeta source carrying one @Vertex and one
// @Fragment method, collects them the way Compiler::emitXpuKernels does
// (getAllMethods + isGraphicsShader), runs emitGraphicsRegistration into a fresh
// host module, and asserts the embedded SPIR-V constants + registration ctors
// landed (keyed by entry name) and are wired into llvm.global_ctors.
//
// GPU-free (SPIR-V is bytes; no device, no spirv-val needed here — the binary's
// validity is already covered by GfxGraphicsLoweringTests).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/core/XpuAttributes.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::MethodPtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler, const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_gfx_reg_" + std::to_string(rng()));
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
                 / ("cajeta_gfx_reg_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// Collect graphics-shader methods exactly as Compiler::emitXpuKernels does.
std::vector<MethodPtr> collectShaders(const CajetaModulePtr& module) {
    std::vector<MethodPtr> shaders;
    for (auto& method : module->getAllMethods()) {
        if (method && cajeta::xpu::isGraphicsShader(*method)) {
            shaders.push_back(method);
        }
    }
    return shaders;
}

} // namespace

// §4 registration — a @Vertex + a @Fragment method, collected and run through
// emitGraphicsRegistration, embed their SPIR-V into the host module and append
// an llvm.global_ctors registration ctor each (keyed by entry name) — exactly
// like @Kernel registration.
TEST(GfxGraphicsRegistrationTests, vertexAndFragmentShadersAutoRegister) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static Vector<float32,4> vsMain(Vector<float32,4> position) {\n"
        "        return position;\n"
        "    }\n"
        "    @Fragment\n"
        "    public static Vector<float32,4> fsMain(Vector<float32,4> varying) {\n"
        "        return varying;\n"
        "    }\n"
        "}\n";

    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.S");
    std::vector<MethodPtr> shaders = collectShaders(module);
    ASSERT_EQ(shaders.size(), 2u) << "expected to collect both graphics shaders";

    llvm::LLVMContext ctx;
    llvm::Module host("gfx_reg_host", ctx);
    int emitted = cajeta::xpu::emitGraphicsRegistration(
        cajeta::xpu::Backend::Spirv, shaders, host, "vulkan1.3");
    ASSERT_EQ(emitted, 2) << "both graphics shaders should be embedded + registered";

    // The backend-neutral runtime hook must be declared/used.
    EXPECT_NE(host.getFunction("__cajeta_xpu_register_module"), nullptr);

    // Each shader's SPIR-V is embedded as a private constant and a registration
    // ctor is emitted, keyed by entry (method) name.
    for (const char* entry : {"vsMain", "fsMain"}) {
        EXPECT_NE(host.getNamedGlobal(std::string("xpu.spirv.") + entry), nullptr)
            << "missing embedded SPIR-V for " << entry;
        EXPECT_NE(host.getFunction(std::string("__cajeta_xpu_reg_ctor.") + entry),
                  nullptr)
            << "missing registration ctor for " << entry;
    }

    // The ctors are wired into module-init.
    EXPECT_NE(host.getNamedGlobal("llvm.global_ctors"), nullptr)
        << "registration ctors must be appended to llvm.global_ctors";
}

// §4 registration — graphics registration is Vulkan/SPIR-V-only: the neutral
// seam no-ops (returns 0, touches nothing) for non-Spirv backends, since the
// other device targets have no rasterization pipeline.
TEST(GfxGraphicsRegistrationTests, nonSpirvBackendsNoOp) {
    const std::string src =
        "package test;\n"
        "public class S {\n"
        "    @Vertex\n"
        "    public static Vector<float32,4> vsMain(Vector<float32,4> position) {\n"
        "        return position;\n"
        "    }\n"
        "}\n";

    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.S");
    std::vector<MethodPtr> shaders = collectShaders(module);
    ASSERT_EQ(shaders.size(), 1u);

    for (auto backend : {cajeta::xpu::Backend::Nvptx, cajeta::xpu::Backend::Amdgpu,
                         cajeta::xpu::Backend::Cpu}) {
        llvm::LLVMContext ctx;
        llvm::Module host("gfx_reg_noop", ctx);
        int emitted = cajeta::xpu::emitGraphicsRegistration(
            backend, shaders, host, "vulkan1.3");
        EXPECT_EQ(emitted, 0) << "graphics registration must no-op for "
                              << cajeta::xpu::backendName(backend);
        EXPECT_EQ(host.getNamedGlobal("xpu.spirv.vsMain"), nullptr);
    }
}
