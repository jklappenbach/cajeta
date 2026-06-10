//
// CajetaXPU AMD bring-up (cajeta-amd.md Increment 4) — SAXPY on a real AMD
// GPU (gfx1151 / Strix Halo), end to end.
//
// The AMD analog of XpuSaxpyDeviceTests. The whole AMDGPU pipeline on the GPU:
//   Cajeta @Kernel source
//     -> AmdgpuKernelLowering (shared AST walk + AMDGPU LoweringTarget)
//     -> AmdgpuBackend::emitIsa / assembleHsaco (LLVM -> object -> lld -> hsaco)
//     -> HipDriver (hipModuleLoadData / hipModuleLaunchKernel)
//     -> verify y[i] == a*x[i] + y[i].
//
// This is the variance-discipline payoff: the SAME SAXPY source that runs on
// NVIDIA runs here, with only LoweringTarget + the driver forking. Skips
// cleanly when no ROCm/HIP device is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Program.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace cajeta::xpu::amd;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_amddev_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_amddev_arch_" + std::to_string(rng()));
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

TEST(XpuSaxpyAmdDeviceTests, runsOnDevice) {
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no ROCm/HIP device available";
    }

    // 1. Compile the kernel source to an hsaco.
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto saxpy = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(saxpy, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_amddevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(saxpy, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    // 2. Host data. x[i] = i, y[i] = 1, a = 2  ->  y[i] = 2*i + 1.
    //    All integer-valued and < 2^24, so float-exact.
    const unsigned n = 1u << 20;             // 1,048,576
    const float a = 2.0f;
    std::vector<float> x(n), y(n);
    for (unsigned i = 0; i < n; ++i) { x[i] = (float) i; y[i] = 1.0f; }

    // 3. Launch on the device.
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "saxpy");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(float);
    HipDevicePtr dY = hip.alloc(bytes);
    HipDevicePtr dX = hip.alloc(bytes);
    ASSERT_NE(dY, nullptr);
    ASSERT_NE(dX, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dY, y.data(), bytes));
    ASSERT_TRUE(hip.memcpyHtoD(dX, x.data(), bytes));

    // Kernel argv: saxpy(y, x, a, n) — pointers to each argument value.
    void* params[] = { &dY, &dX, (void*) &a, (void*) &n };
    const unsigned block = 256;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(hip.launch(fn, grid, block, params));
    ASSERT_TRUE(hip.synchronize());

    // 4. Read back and verify.
    std::vector<float> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dY, bytes));
    hip.free(dY);
    hip.free(dX);

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

// Item 4: a MULTI-ARCH bundle (gfx1100 + gfx1151) built by assembleHsacoBundle
// loads via hipModuleLoadData — the HIP runtime selects the running device's arch
// — and the kernel runs correctly on-device. Proves AMD multi-arch fatbin end to
// end (bundle → load → select → launch). Skips without HIP / the bundler.
TEST(XpuSaxpyAmdDeviceTests, multiArchBundleRunsOnDevice) {
    if (!HipDriver::available()) GTEST_SKIP() << "no HIP device";
    if (!llvm::sys::findProgramByName("clang-offload-bundler"))
        GTEST_SKIP() << "clang-offload-bundler not found";

    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
        "                              float32 a, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { y[i] = a * x[i] + y[i]; }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.M");
    auto saxpy = findMethod(module->getStructures()["test.M"], "saxpy");
    ASSERT_NE(saxpy, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");   // config only; arch-neutral
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module deviceModule("xpu_saxpy_amd_multiarch", ctx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(saxpy, deviceModule);

    std::vector<uint8_t> bundle =
        assembleHsacoBundle(deviceModule, {"gfx1100", "gfx1151"});
    ASSERT_FALSE(bundle.empty()) << "multi-arch bundle assembly failed";

    const unsigned n = 4096;
    const float a = 2.0f;
    std::vector<float> x(n), y(n);
    for (unsigned i = 0; i < n; ++i) { x[i] = (float) i; y[i] = 1.0f; }

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(bundle.data(), bundle.size());
    ASSERT_NE(mod, nullptr) << "hipModuleLoadData rejected the multi-arch bundle";
    HipFunction fn = hip.getFunction(mod, "saxpy");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(float);
    HipDevicePtr dY = hip.alloc(bytes), dX = hip.alloc(bytes);
    ASSERT_NE(dY, nullptr); ASSERT_NE(dX, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dY, y.data(), bytes));
    ASSERT_TRUE(hip.memcpyHtoD(dX, x.data(), bytes));
    void* params[] = { &dY, &dX, (void*) &a, (void*) &n };
    const unsigned block = 256, grid = (n + block - 1) / block;
    ASSERT_TRUE(hip.launch(fn, grid, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dY, bytes));
    hip.free(dY); hip.free(dX);
    size_t mismatches = 0;
    for (unsigned i = 0; i < n; ++i)
        if (result[i] != a * x[i] + 1.0f) ++mismatches;
    EXPECT_EQ(mismatches, 0u);
}
