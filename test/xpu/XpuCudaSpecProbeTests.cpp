//
// Diagnostic probe: the NVPTX spec-constant constant-memory read on a real
// NVIDIA device, driven from C++ (CudaDriver) so stderr is live (the JIT'd
// runtime's stderr is unreliable — embedded-bitcode CRT resolution).
//
// Isolates why the host-source spec-override dispatch tests fail: prints the
// emitted PTX (so the __cajeta_xpu_spec_count/_values const globals + the
// ld.const read are visible), assembles the cubin (catching any ptxas error),
// loads it, and runs the kernel with the const globals left zero-initialized —
// which MUST read the baked default (the no-override contract).
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
#include <cstdio>
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
              / ("cajeta_cuda_specprobe_" + std::to_string(rng()));
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
                 / ("cajeta_cuda_specprobe_arch_" + std::to_string(rng()));
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

// No-override spec read on the device: count is left 0 (cubin zero-init), so the
// kernel MUST return the baked default. Prints the PTX so the const globals and
// the ld.const read are visible, and assembles + loads + runs to verify the
// cubin is well-formed and the device read works.
TEST(XpuCudaSpecProbeTests, noOverrideReadsBakedDefaultOnDevice) {
    if (!CudaDriver::available()) {
        GTEST_SKIP() << "no CUDA device/driver available";
    }

    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class SC {\n"
        "    @Kernel\n"
        "    public static void fill(KernelBuffer<int32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) { out[i] = Spec.geti(0, 99); }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.SC");
    auto fill = findMethod(module->getStructures()["test.SC"], "fill");
    ASSERT_NE(fill, nullptr);

    auto tm = createNvptxTargetMachine("sm_89");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_specprobe_device", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    ASSERT_NE(lowerKernel(fill, deviceModule), nullptr);
    std::string ptx = emitPtx(deviceModule, *tm);
    ASSERT_FALSE(ptx.empty());

    if (std::getenv("CAJETA_XPU_DEBUG"))
        std::fprintf(stderr, "\n===== SPEC KERNEL PTX =====\n%s\n===== END PTX =====\n",
                     ptx.c_str());

    std::vector<uint8_t> cubin = assembleCubin(ptx, "sm_89");
    ASSERT_FALSE(cubin.empty()) << "ptxas rejected the spec kernel PTX";

    const uint32_t n = 64;
    std::vector<int32_t> out(n, -1);

    CudaDriver cuda;
    ASSERT_TRUE(cuda.init());
    CudaModule mod = cuda.loadModule(cubin.data(), cubin.size());
    ASSERT_NE(mod, nullptr) << "cuModuleLoadData failed for the spec kernel";
    CudaFunction fn = cuda.getFunction(mod, "fill");
    ASSERT_NE(fn, nullptr);

    // Replicate the runtime's pre-launch constant-memory write (the spec override
    // path: cuModuleGetGlobal + cuMemcpyHtoD of count=0) to prove it does not
    // poison the context — the device-side half of the spec contract.
    ASSERT_TRUE(cuda.setModuleGlobalI32(mod, "__cajeta_xpu_spec_count", 0))
        << "cuModuleGetGlobal/cuMemcpyHtoD to the spec const global failed";

    CudaDevicePtr dOut = cuda.alloc(std::size_t(n) * sizeof(int32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_TRUE(cuda.memcpyHtoD(dOut, out.data(), std::size_t(n) * sizeof(int32_t)));

    void* params[] = { &dOut, (void*) &n };
    ASSERT_TRUE(cuda.launch(fn, /*grid=*/1, /*block=*/64, params));
    ASSERT_TRUE(cuda.synchronize());

    ASSERT_TRUE(cuda.memcpyDtoH(out.data(), dOut, std::size_t(n) * sizeof(int32_t)));
    cuda.free(dOut);

    for (uint32_t i = 0; i < n; ++i) {
        ASSERT_EQ(out[i], 99) << "at i=" << i
            << " — spec no-override did not read the baked default 99 on device";
    }
}
