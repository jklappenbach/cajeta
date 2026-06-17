//
// CajetaXPU — loop-bearing kernel on a real NVIDIA device.
//
// Companion to XpuSaxpyDeviceTests: where SAXPY is straight-line, this
// exercises the generalized lowering (a for-loop with a mutable counter +
// accumulator and compound assignment) end to end on the GPU. A single
// thread reduces `in[0..n)` and writes the sum to `result[0]`.
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

#include <cstdint>
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
              / ("cajeta_xpu_loopdev_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_loopdev_arch_" + std::to_string(rng()));
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

TEST(XpuLoopDeviceTests, singleThreadReductionRunsOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    // GpuThread 0 sums in[0..n) into result[0]; every other thread is idle.
    const char* src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "import cajeta.gpu.GpuThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void reduceSum(GpuBuffer<int32> result, GpuBuffer<int32> in,\n"
        "                                 uint32 n) {\n"
        "        uint32 t = GpuThread.globalIdX();\n"
        "        if (t == 0) {\n"
        "            int32 sum = 0;\n"
        "            for (uint32 j = 0; j < n; j += 1) {\n"
        "                sum += in[j];\n"
        "            }\n"
        "            result[0] = sum;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto reduce = findMethod(module->getStructures()["test.M"], "reduceSum");
    ASSERT_NE(reduce, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_reduce_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(reduce, deviceModule), nullptr);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty());

    // in[i] = i  ->  expected sum = n*(n-1)/2 (fits int32 for n=1000).
    const uint32_t n = 1000;
    std::vector<int32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = (int32_t) i;
    const int32_t expected = (int32_t) ((uint64_t) n * (n - 1) / 2);

    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_NE(mod, nullptr);
    CudaFunction fn = cuda.getFunction(mod, "reduceSum");
    ASSERT_NE(fn, nullptr);

    CudaDevicePtr dResult = cuda.alloc(sizeof(int32_t));
    CudaDevicePtr dIn = cuda.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dResult, 0u);
    ASSERT_NE(dIn, 0u);
    ASSERT_TRUE(cuda.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(int32_t)));

    void* params[] = { &dResult, &dIn, (void*) &n };
    ASSERT_TRUE(cuda.launch(fn, /*grid=*/1, /*block=*/1, params));
    ASSERT_TRUE(cuda.synchronize());

    int32_t result = -1;
    ASSERT_TRUE(cuda.memcpyDtoH(&result, dResult, sizeof(int32_t)));
    cuda.free(dResult);
    cuda.free(dIn);

    EXPECT_EQ(result, expected);
}
