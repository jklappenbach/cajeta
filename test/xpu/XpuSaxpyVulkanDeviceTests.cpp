//
// CajetaXPU Vulkan bring-up (Increment 4) — SAXPY on a real GPU via Vulkan
// compute, end to end.
//
// The Vulkan analog of XpuSaxpy{,Amd}DeviceTests. The whole SPIR-V pipeline on
// the GPU:
//   Cajeta @Kernel source
//     -> SpirvKernelLowering (shared AST walk + SPIR-V LoweringTarget)
//     -> SpirvBackend::emitSpirv (LLVM -> Khronos SPIR-V binary, no assembler)
//     -> VulkanDriver (descriptor-set bind + compute pipeline + vkCmdDispatch)
//     -> verify y[i] == a*x[i] + y[i].
//
// This is the variance-discipline payoff for the THIRD backend: the SAME SAXPY
// source that runs on NVIDIA and AMD runs here — but Vulkan forks more (the
// kernel signature + buffer access + the whole launch path), which is exactly
// the measured finding. Scalars cross as single-element storage buffers
// (descriptor-set ABI; bindings 0..3 = y, x, a, n). Skips cleanly when no
// Vulkan compute device is present.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

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

using namespace cajeta::xpu::vulkan;
using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vkdev_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_vkdev_arch_" + std::to_string(rng()));
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

TEST(XpuSaxpyVulkanDeviceTests, runsOnDevice) {
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }

    // 1. Compile the kernel source to a SPIR-V binary.
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

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_saxpy_vkdevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(saxpy, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    // 2. Host data. x[i] = i, y[i] = 1, a = 2  ->  y[i] = 2*i + 1.
    //    All integer-valued and < 2^24, so float-exact.
    const uint32_t n = 1u << 20;             // 1,048,576
    const float a = 2.0f;
    std::vector<float> x(n), y(n);
    for (uint32_t i = 0; i < n; ++i) { x[i] = (float) i; y[i] = 1.0f; }

    // 3. Launch on the device. Scalars cross as single-element SSBOs; the
    //    descriptor bindings are y, x, a, n in kernel-parameter order.
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    const std::size_t bytes = std::size_t(n) * sizeof(float);
    VulkanDriver::Buffer dY = vk.alloc(bytes);
    VulkanDriver::Buffer dX = vk.alloc(bytes);
    VulkanDriver::Buffer dA = vk.alloc(sizeof(float));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dY, 0u);
    ASSERT_NE(dX, 0u);
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dY, y.data(), bytes));
    ASSERT_TRUE(vk.upload(dX, x.data(), bytes));
    ASSERT_TRUE(vk.upload(dA, &a, sizeof(float)));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));

    // Vulkan bakes the workgroup size into the SPIR-V (LocalSize); the grid is
    // the number of workgroups covering n threads.
    const unsigned block = cajeta::xpu::vulkan::kVulkanLocalSizeX;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "saxpy",
                          {dY, dX, dA, dN}, grid));

    // 4. Read back and verify.
    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dY, bytes));
    vk.free(dY);
    vk.free(dX);
    vk.free(dA);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
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
