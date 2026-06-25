//
// CajetaXPU step 11 — SAXPY on a real NVIDIA device, end to end.
//
// The whole NVPTX pipeline, exercised on the GPU:
//   Cajeta @Kernel source
//     -> NvptxKernelLowering (AST -> device LLVM IR)
//     -> NvptxBackend::emitPtx (LLVM -> PTX)
//     -> NvptxBackend::assembleCubin (ptxas -> .cubin)
//     -> CudaDriver (cuModuleLoadData / cuLaunchKernel)
//     -> verify y[i] == a*x[i] + y[i].
//
// This is the variance-discipline payoff in miniature: the SAME kernel
// source that runs on the CPU-emulation path compiles into a real device
// launch. Host-side launch via Cajeta `saxpy.launch(...)` syntax (compiled
// through LLJIT) is the follow-on; here the host orchestration is C++ via
// CudaDriver, which is what the eventual @Native launch runtime calls into.
//
// Skips cleanly when no CUDA device/driver is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/nvidia/NvptxBackend.h"
#include "cajeta/xpu/nvidia/NvptxKernelLowering.h"
#include "cajeta/xpu/nvidia/CudaDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::nvidia;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_dev_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_dev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
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

} // namespace

TEST(XpuSaxpyDeviceTests, runsOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    // 1. Compile the kernel source to a cubin.
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto saxpy = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(saxpy, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(saxpy, deviceModule);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty());

    // 2. Host data. x[i] = i, y[i] = 1, a = 2  ->  y[i] = 2*i + 1.
    //    All integer-valued and < 2^24, so float-exact.
    const unsigned n = 1u << 20;             // 1,048,576
    const float a = 2.0f;
    std::vector<float> x(n), y(n);
    for (unsigned i = 0; i < n; ++i) { x[i] = (float) i; y[i] = 1.0f; }

    // 3. Launch on the device.
    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_NE(mod, nullptr);
    CudaFunction fn = cuda.getFunction(mod, "saxpy");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(float);
    CudaDevicePtr dY = cuda.alloc(bytes);
    CudaDevicePtr dX = cuda.alloc(bytes);
    ASSERT_NE(dY, 0u);
    ASSERT_NE(dX, 0u);
    ASSERT_TRUE(cuda.memcpyHtoD(dY, y.data(), bytes));
    ASSERT_TRUE(cuda.memcpyHtoD(dX, x.data(), bytes));

    // Kernel argv: saxpy(y, x, a, n) — pointers to each argument value.
    void* params[] = { &dY, &dX, (void*) &a, (void*) &n };
    const unsigned block = 256;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(cuda.launch(fn, grid, block, params));
    ASSERT_TRUE(cuda.synchronize());

    // 4. Read back and verify.
    std::vector<float> result(n);
    ASSERT_TRUE(cuda.memcpyDtoH(result.data(), dY, bytes));
    cuda.free(dY);
    cuda.free(dX);

    size_t mismatches = 0;
    for (unsigned i = 0; i < n; ++i) {
        float expected = a * x[i] + 1.0f;     // == 2*i + 1
        if (result[i] != expected) {
            if (mismatches < 5) {
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << expected;
            }
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}
