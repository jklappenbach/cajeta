//
// @Autotune registration — end-to-end GPU-free check (kernel-occupancy-autotune
// U3b.2): an @Autotune kernel emits a __cajeta_xpu_register_autotune call plus a
// candidate-block array into the host module. Needs ld.lld (host tool) to
// assemble the hsaco; skipped when absent. No GPU required.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuRegistration.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/LLVMContext.h"

#include <filesystem>
#include <fstream>
#include <random>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

const char* kAutotuneKernelSrc =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    @Autotune(blocks = {64, 128, 256})\n"
    "    public static void tunedAxpy(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {\n"
    "        uint32 i = KernelThread.x();\n"
    "        while (i < n) { y[i] = x[i] + y[i]; i = i + 256; }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler, const char* source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_atreg_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_atreg_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule((base / "test" / "M.cajeta").string(),
                                   base.string(), archive.string());
    compiler.compile(m);
    return m;
}

} // namespace

TEST(XpuAutotuneRegistrationTests, autotuneKernelEmitsRegistration) {
    if (cajeta::xpu::amd::findLld().empty())
        GTEST_SKIP() << "ld.lld not found (set ROCM_PATH) — needs the host linker";

    Compiler compiler;
    auto module = compileForInspection(compiler, kAutotuneKernelSrc);
    auto& cls = module->getStructures()["test.M"];
    std::vector<cajeta::MethodPtr> kernels;
    for (auto& [k, m] : cls->getMethods())
        if (m && m->getName() == "tunedAxpy") kernels.push_back(m);
    ASSERT_EQ(kernels.size(), 1u);

    llvm::LLVMContext ctx;
    llvm::Module host("atreg_host", ctx);
    int n = cajeta::xpu::amd::emitKernelRegistration(kernels, host, "gfx1151");
    ASSERT_GE(n, 1) << "kernel did not register (lld/codegen failed)";

    // The runtime hook + the candidate-block array must both be present.
    EXPECT_NE(host.getFunction("__cajeta_xpu_register_autotune"), nullptr)
        << "no autotune registration call emitted";
    EXPECT_NE(host.getNamedGlobal("xpu.atblk.tunedAxpy"), nullptr)
        << "no candidate-block array emitted";
}
