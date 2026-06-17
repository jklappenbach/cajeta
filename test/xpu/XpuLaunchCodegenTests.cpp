//
// CajetaXPU step 8 (increment G, host launch wiring) — codegen for the
// `kernel.launch(stream, grid:, block:)(args)` form.
//
// CallExpression::generateCode lowers a launch site to a host runtime call
//   __cajeta_xpu_launch(name, gridX, blockX, argv)
// marshalling kernel args into a CUDA-style argv (GpuBuffer<T> -> deviceHandle,
// scalars by value). This test drives Phase-2 codegen on a host method that
// launches a kernel and confirms the lowering emits that call (rather than the
// old NOT_IMPLEMENTED throw). The kernel body is empty so host codegen of the
// kernel itself doesn't depend on device-only semantics.
//
// (Running the launch on a GPU through the JIT — cubin registration + the
// real CUDA-backed runtime — is the next step; this isolates the compiler
// lowering.)
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_lc_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_lc_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// Drive Phase-1/2 codegen the way the JIT loop does, so generateCode (and the
// launch lowering) actually fires.
void codegenAll(Compiler& compiler) {
    for (auto& m : compiler.getModules())
        for (auto& method : m->getAllMethods())
            method->getLlvmFunctionType();
    for (auto& m : compiler.getModules())
        for (auto& method : m->getAllMethods())
            method->generateCode();
}

std::string moduleIR(const CajetaModulePtr& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m->getLlvmModule()->print(os, nullptr);
    return s;
}

} // namespace

// A host method that launches a kernel codegens without error and emits a
// call to __cajeta_xpu_launch. The kernel takes a GpuBuffer and two scalars, so
// the marshalling exercises both the GpuBuffer-deviceHandle and scalar paths.
TEST(XpuLaunchCodegenTests, launchLowersToRuntimeCall) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuStream;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(GpuBuffer<float32> y, float32 a, uint32 n) { }\n"
        "    public static void run(GpuBuffer<float32> y, float32 a, uint32 n) {\n"
        "        k.launch(GpuStream.current(),\n"
        "                 grid: [(n + 255) / 256], block: [256])(y, a, n);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    ASSERT_NO_THROW(codegenAll(compiler));

    std::string ir = moduleIR(module);
    EXPECT_NE(ir.find("__cajeta_xpu_launch"), std::string::npos) << ir;
}

// A launch with a `sharedBytes:` config arg lowers to the 5-arg
// __cajeta_xpu_launch(name, gridX, blockX, sharedBytes, argv), carrying the
// dynamic-shared byte count. (The label is `sharedBytes:`, not `shared:` —
// `shared` is the placement keyword and won't lex as a parameterLabel.)
TEST(XpuLaunchCodegenTests, sharedConfigLowersByteCount) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuStream;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(GpuBuffer<int32> out, uint32 n) { }\n"
        "    public static void run(GpuBuffer<int32> out, uint32 n) {\n"
        "        k.launch(GpuStream.current(),\n"
        "                 grid: [1], block: [256], sharedBytes: [2048])(out, n);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    ASSERT_NO_THROW(codegenAll(compiler));

    std::string ir = moduleIR(module);
    // 10-arg runtime signature: name, gridX/Y/Z, blockX/Y/Z, sharedBytes, argv,
    // streamHandle (the GpuStream's i64 handle, threaded for stream-ordered launch).
    EXPECT_NE(ir.find(
                  "@__cajeta_xpu_launch(ptr, i32, i32, i32, i32, i32, i32, i32, ptr, i64)"),
              std::string::npos) << ir;
    // The requested dynamic-shared byte count reaches the call.
    EXPECT_NE(ir.find("i32 2048"), std::string::npos) << ir;
}
