//
// CajetaXPU CPU backend — the grid→threads lowering on the seam (cajeta-cpu.md
// Increment 1).
//
// The SAME portable GPU-style kernel (Buffer<T> + Thread.globalIdX()) that
// targets NVPTX/AMDGPU/SPIR-V lowers through the shared AST walk for the CPU,
// with only the CpuTarget forking: the kernel gains 12 trailing i32 coordinate
// params (the grid→threads model), coordinate reads come from those args (no
// hardware intrinsics), buffers are flat addrspace(0) pointers. GPU-free.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Portable SAXPY: Buffer<T> params + Thread.globalIdX() — identical in spirit
// to the Vulkan/AMD device kernels. globalIdX exercises all three coordinate
// reads (ctaid*ntid + tid), so the grid→threads arg model is fully covered.
const char* kSaxpySource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
    "                             float32 a) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        y[i] = a * x[i] + y[i];\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_cpu_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_cpu_arch_" + std::to_string(rng()));
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

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

} // namespace

// The CpuTarget lowers the portable kernel to a host function: addrspace-0
// buffers, the 12 trailing coordinate params, coordinate reads from args, no
// device intrinsics.
TEST(XpuCpuEmitTests, lowersPortableKernelToGridThreadsHostFunction) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSaxpySource);
    auto k = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_cpu_saxpy", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);

    llvm::Function* fn = cajeta::xpu::cpu::lowerKernel(k, host);
    ASSERT_NE(fn, nullptr);

    // Signature: 2 buffers + 1 scalar + 12 coordinate params = 15 args.
    const unsigned kKernelParams = 3;
    ASSERT_EQ(fn->arg_size(),
              kKernelParams + cajeta::xpu::cpu::kNumCoordParams);
    // The trailing 12 are i32 coordinates.
    for (unsigned c = 0; c < cajeta::xpu::cpu::kNumCoordParams; ++c) {
        auto* arg = fn->getArg(kKernelParams + c);
        EXPECT_TRUE(arg->getType()->isIntegerTy(32))
            << "coord arg " << c << " is not i32";
    }
    // The two buffers are flat (addrspace 0) pointers.
    EXPECT_TRUE(fn->getArg(0)->getType()->isPointerTy());
    EXPECT_EQ(fn->getArg(0)->getType()->getPointerAddressSpace(), 0u);

    std::string ir = printModule(host);
    // No device address space, no hardware coordinate/wave intrinsics.
    EXPECT_EQ(ir.find("addrspace(1)"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("nvvm"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("amdgcn"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("llvm.spv"), std::string::npos) << ir;
    // A void host function that returns.
    EXPECT_NE(ir.find("ret void"), std::string::npos) << ir;
}
