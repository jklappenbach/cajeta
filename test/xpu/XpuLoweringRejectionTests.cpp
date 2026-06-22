//
// CajetaXPU — kernel-lowering guardrail rejections (XPU-N01).
//
// The shared kernel-lowering walk rejects a handful of constructs the substrate
// deliberately doesn't support yet, each with a clear XPU-N01 diagnostic. The
// behavior was already enforced in the lowering; these are the missing
// *emit-time rejection tests* that pin it (Part I follow-ups in
// plans/gpu/xpu/cajeta-xpu-plan.md — Stage 3 recursion, Stage 5 dynamic-shared
// limits). All GPU-free: the rejection fires during device body lowering, well
// before any driver/runtime touch.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"
#include "cajeta/error/Exception.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_reject_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_reject_arch_" + std::to_string(rng()));
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

// Lower `kernel` through the CPU device path (the rejection fires here).
void lowerOnCpu(const cajeta::MethodPtr& kernel) {
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_reject_cpu", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    cajeta::xpu::cpu::lowerKernel(kernel, host);
}

// Lower `kernel` through the Vulkan/SPIR-V device path (GPU-free host emit).
void lowerOnVulkan(const cajeta::MethodPtr& kernel) {
    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module dev("xpu_reject_vk", ctx);
    cajeta::xpu::vulkan::configureDeviceModule(dev, *tm);
    cajeta::xpu::vulkan::lowerKernel(kernel, dev);
}

} // namespace

// Stage 3 follow-up — a recursive @Device helper is rejected (XPU-N01): the
// device-function lowering marks each helper in-progress before lowering its
// body, so a call back into a helper still being lowered is a recursion the
// always-inline model can't fold.
TEST(XpuLoweringRejectionTests, recursiveDeviceCallRejected) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "import cajeta.gpu.KernelThread;\n"
        "public class M {\n"
        "    @Device\n"
        "    public static int32 fib(int32 n) {\n"
        "        if (n < 2) { return n; }\n"
        "        return fib(n - 1) + fib(n - 2);\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        out[i] = fib(8);\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);
    try {
        lowerOnCpu(k);
        FAIL() << "expected XPU-N01 for a recursive @Device call";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N01");
        EXPECT_NE(e.getMessage().find("recursive"), std::string::npos)
            << e.getMessage();
    }
}

// Stage 5 follow-up — at most one dynamic (runtime-sized) shared array per
// kernel; a second is rejected (XPU-N01). Fires on every backend.
TEST(XpuLoweringRejectionTests, secondDynamicSharedArrayRejected) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "import cajeta.gpu.KernelThread;\n"
        "import cajeta.gpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, uint32 lenA, uint32 lenB) {\n"
        "        Shared<int32> a = shared int32[lenA];\n"
        "        Shared<int32> b = shared int32[lenB];\n"
        "        uint32 t = KernelThread.x();\n"
        "        a[t] = 0; b[t] = 0;\n"
        "        out[t] = a[t] + b[t];\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);
    try {
        lowerOnCpu(k);
        FAIL() << "expected XPU-N01 for a second dynamic shared array";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N01");
        EXPECT_NE(e.getMessage().find("at most one dynamic"), std::string::npos)
            << e.getMessage();
    }
}

// Stage 5 follow-up — a dynamic Shared<T> currently requires a 4-byte element
// (the Vulkan launch's shared-length spec constant assumes 4 bytes); a non-4-byte
// element is rejected (XPU-N01) rather than silently mis-sized. Vulkan-gated
// (dynamicSharedNeedsConcreteSize()), so driven through the SPIR-V path.
TEST(XpuLoweringRejectionTests, nonFourByteDynamicSharedRejectedOnVulkan) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "import cajeta.gpu.KernelThread;\n"
        "import cajeta.gpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<int32> out, uint32 len) {\n"
        "        Shared<int64> a = shared int64[len];\n"
        "        uint32 t = KernelThread.x();\n"
        "        a[t] = 0;\n"
        "        out[t] = (int32) a[t];\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);
    try {
        lowerOnVulkan(k);
        FAIL() << "expected XPU-N01 for a non-4-byte dynamic shared element";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N01");
        EXPECT_NE(e.getMessage().find("4-byte"), std::string::npos)
            << e.getMessage();
    }
}
