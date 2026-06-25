//
// CajetaXPU CPU backend — on-CPU execution of the lowered kernel (cajeta-cpu.md
// Increment 1, the oracle stretch goal).
//
// The portable SAXPY kernel lowered by CpuTarget is JIT-compiled and run over a
// grid by a host driver that supplies each work-item's coordinates through the
// 12 trailing kernel args (the grid→threads model). This proves the lowering is
// not just structurally right (XpuCpuEmitTests) but *semantically* correct —
// and it is the CPU oracle the GPU backends can be diffed against. GPU-free.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

const char* kSaxpySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                             float32 a) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        y[i] = a * x[i] + y[i];\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_cpuexec_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_cpuexec_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// The lowered kernel's host signature: (y, x, a, then 12 i32 coordinates).
using SaxpyFn = void (*)(float*, float*, float,
                         int32_t, int32_t, int32_t,   // tid.{x,y,z}
                         int32_t, int32_t, int32_t,   // ctaid.{x,y,z}
                         int32_t, int32_t, int32_t,   // ntid.{x,y,z}
                         int32_t, int32_t, int32_t);  // nctaid.{x,y,z}

} // namespace

TEST(XpuCpuExecTests, saxpyRunsOnCpuOverAGrid) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSaxpySource);
    auto k = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_cpu_saxpy_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);

    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));

    auto symOrErr = jit->lookup("saxpy");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto saxpy = symOrErr->toPtr<SaxpyFn>();

    // Grid: G blocks × B threads covering N elements.
    const int32_t B = 64, G = 4;
    const int N = B * G;
    const float a = 3.0f;
    std::vector<float> x(N), y(N), y0(N);
    for (int i = 0; i < N; ++i) {
        x[i] = static_cast<float>(i);
        y[i] = y0[i] = static_cast<float>(2 * i + 1);
    }

    // Host driver — the grid→threads loop. globalId.x = ctaid*ntid + tid = i.
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            saxpy(y.data(), x.data(), a,
                  tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (int i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(y[i], a * x[i] + y0[i]) << "element " << i;
}
