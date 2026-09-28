//
// CajetaXPU step 8 (increment G, host launch wiring) — codegen for the
// `kernel.launch(stream, grid:, block:)(args)` form.
//
// CallExpression::generateCode lowers a launch site to a host runtime call
//   __cajeta_xpu_launch(name, gridX, blockX, argv)
// marshalling kernel args into a CUDA-style argv (KernelBuffer<T> -> deviceHandle,
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
#include "KernelLoweringProbe.h"

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

using cajeta::xpu::probe::compileForInspection;


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
// call to __cajeta_xpu_launch. The kernel takes a KernelBuffer and two scalars, so
// the marshalling exercises both the KernelBuffer-deviceHandle and scalar paths.
TEST(XpuLaunchCodegenTests, launchLowersToRuntimeCall) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelStream;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> y, float32 a, uint32 n) { }\n"
        "    public static void run(KernelBuffer<float32> y, float32 a, uint32 n) {\n"
        "        k.launch(KernelStream.current(),\n"
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
