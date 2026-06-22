//
// CajetaXPU — workgroup shared memory on a real NVIDIA device.
//
// Companion to XpuNvptxSharedEmitTests (PTX text): runs a 256-wide single-block
// tree reduction that stages its inputs through `shared int32[256]`, barriers,
// and reduces in shared memory. Proves the addrspace(3) global + bar.sync path
// is correct end to end on the GPU.
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
              / ("cajeta_xpu_shareddev_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_shareddev_arch_" + std::to_string(rng()));
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

TEST(XpuSharedDeviceTests, sharedTreeReductionRunsOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    // One block of 256 threads: stage in[0..256) into a shared tile, barrier,
    // tree-reduce, thread 0 writes tile[0] to out[blockIdx].
    const char* src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "import cajeta.gpu.KernelThread;\n"
        "import cajeta.gpu.Workgroup;\n"
        "import cajeta.gpu.Barrier;\n"
        "import cajeta.gpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void reduce(KernelBuffer<int32> out, KernelBuffer<int32> in, uint32 n) {\n"
        "        Shared<int32> tile = shared int32[256];\n"
        "        uint32 t = KernelThread.x();\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < n) {\n"
        "            tile[t] = in[g];\n"
        "        } else {\n"
        "            tile[t] = 0;\n"
        "        }\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
        "            if (t < s) {\n"
        "                tile[t] += tile[t + s];\n"
        "            }\n"
        "            Barrier.workgroup();\n"
        "        }\n"
        "        if (t == 0) {\n"
        "            out[Workgroup.x()] = tile[0];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto reduce = findMethod(module->getStructures()["test.M"], "reduce");
    ASSERT_NE(reduce, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_shared_reduce_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(reduce, deviceModule), nullptr);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty());

    // in[i] = i  ->  expected sum over the 256-wide block = 256*255/2.
    const uint32_t n = 256;
    std::vector<int32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = (int32_t) i;
    const int32_t expected = (int32_t) ((uint64_t) n * (n - 1) / 2);

    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_NE(mod, nullptr);
    CudaFunction fn = cuda.getFunction(mod, "reduce");
    ASSERT_NE(fn, nullptr);

    CudaDevicePtr dOut = cuda.alloc(sizeof(int32_t));
    CudaDevicePtr dIn = cuda.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dIn, 0u);
    ASSERT_TRUE(cuda.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(int32_t)));

    void* params[] = { &dOut, &dIn, (void*) &n };
    ASSERT_TRUE(cuda.launch(fn, /*grid=*/1, /*block=*/256, params));
    ASSERT_TRUE(cuda.synchronize());

    int32_t result = -1;
    ASSERT_TRUE(cuda.memcpyDtoH(&result, dOut, sizeof(int32_t)));
    cuda.free(dOut);
    cuda.free(dIn);

    EXPECT_EQ(result, expected);
}

// Same reduction, but the tile is DYNAMIC shared memory: its size is a runtime
// kernel arg, so the device global is external/unsized and the byte count is
// supplied at launch (cuLaunchKernel sharedMemBytes). Proves the dynamic path
// end to end on the GPU.
TEST(XpuSharedDeviceTests, dynamicSharedReductionRunsOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    const char* src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "import cajeta.gpu.KernelThread;\n"
        "import cajeta.gpu.Workgroup;\n"
        "import cajeta.gpu.Barrier;\n"
        "import cajeta.gpu.Shared;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void dynreduce(KernelBuffer<int32> out, KernelBuffer<int32> in,\n"
        "                                 uint32 n, uint32 tileLen) {\n"
        "        Shared<int32> tile = shared int32[tileLen];\n"
        "        uint32 t = KernelThread.x();\n"
        "        uint32 g = KernelThread.globalIdX();\n"
        "        if (g < n) {\n"
        "            tile[t] = in[g];\n"
        "        } else {\n"
        "            tile[t] = 0;\n"
        "        }\n"
        "        Barrier.workgroup();\n"
        "        for (uint32 s = 128; s > 0; s >>= 1) {\n"
        "            if (t < s) {\n"
        "                tile[t] += tile[t + s];\n"
        "            }\n"
        "            Barrier.workgroup();\n"
        "        }\n"
        "        if (t == 0) {\n"
        "            out[Workgroup.x()] = tile[0];\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto reduce = findMethod(module->getStructures()["test.M"], "dynreduce");
    ASSERT_NE(reduce, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_dynshared_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(reduce, deviceModule), nullptr);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());
    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty());

    const uint32_t n = 256;
    const uint32_t tileLen = 256;
    std::vector<int32_t> in(n);
    for (uint32_t i = 0; i < n; ++i) in[i] = (int32_t) i;
    const int32_t expected = (int32_t) ((uint64_t) n * (n - 1) / 2);

    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_NE(mod, nullptr);
    CudaFunction fn = cuda.getFunction(mod, "dynreduce");
    ASSERT_NE(fn, nullptr);

    CudaDevicePtr dOut = cuda.alloc(sizeof(int32_t));
    CudaDevicePtr dIn = cuda.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dIn, 0u);
    ASSERT_TRUE(cuda.memcpyHtoD(dIn, in.data(), std::size_t(n) * sizeof(int32_t)));

    // The launch supplies the dynamic shared byte count: tileLen int32s.
    void* params[] = { &dOut, &dIn, (void*) &n, (void*) &tileLen };
    ASSERT_TRUE(cuda.launch(fn, /*grid=*/1, /*block=*/256, params,
                            /*sharedMemBytes=*/tileLen * sizeof(int32_t)));
    ASSERT_TRUE(cuda.synchronize());

    int32_t result = -1;
    ASSERT_TRUE(cuda.memcpyDtoH(&result, dOut, sizeof(int32_t)));
    cuda.free(dOut);
    cuda.free(dIn);

    EXPECT_EQ(result, expected);
}
