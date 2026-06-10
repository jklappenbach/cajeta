//
// CajetaXPU shader clock — device codegen, end to end.
//
// `Thread.clock()` reads a free-running hardware counter (uint64) for in-kernel
// timing. On Vulkan it is OpReadClockKHR at Subgroup scope (SPV_KHR_shader_clock,
// reached via the fork's llvm.spv.read.clock intrinsic); on AMD s_memrealtime, on
// NVPTX clock64, on CPU rdtsc. The tick value is non-deterministic, so the
// verifiable property on-device is that the clock is LIVE: every thread observes
// a non-zero tick (a stuck/zero clock means the op never executed). The host
// spirv-val emit check is XpuVulkanEmitTests.lowersShaderClockToSpirv.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/HipDriver.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

const char* kClockSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void stamp(Buffer<uint64> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i] = Thread.clock();\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_clock_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_clock_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

using StampFn = void (*)(uint64_t*, uint32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t);

constexpr uint32_t kN = 4096;

size_t countZero(const std::vector<uint64_t>& v) {
    size_t z = 0;
    for (uint64_t x : v) z += (x == 0);
    return z;
}

} // namespace

TEST(XpuClockDeviceTests, shaderClockRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kClockSource);
    auto k = findMethod(module->getStructures()["test.M"], "stamp");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_clock_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("stamp");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto stamp = symOrErr->toPtr<StampFn>();

    std::vector<uint64_t> out(kN, 0);
    const int32_t B = 64;
    const int32_t G = (int32_t) (kN / 64);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            stamp(out.data(), kN, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    EXPECT_EQ(countZero(out), 0u) << "some threads observed a zero clock tick";
}

TEST(XpuClockDeviceTests, shaderClockRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, kClockSource);
    auto k = findMethod(module->getStructures()["test.M"], "stamp");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_clock_vk", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    std::vector<uint64_t> out(kN, 0);
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    const std::size_t bytes = std::size_t(kN) * sizeof(uint64_t);
    VulkanDriver::Buffer dOut = vk.alloc(bytes);
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u); ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), bytes));
    ASSERT_TRUE(vk.upload(dN, &kN, sizeof(uint32_t)));

    const unsigned block = kVulkanLocalSizeX;
    const unsigned grid = (kN + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "stamp", {dOut, dN}, grid));

    std::vector<uint64_t> result(kN);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut); vk.free(dN);

    EXPECT_EQ(countZero(result), 0u) << "some invocations observed a zero clock";
}

TEST(XpuClockDeviceTests, shaderClockRunsOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";
    Compiler compiler;
    auto module = compileForInspection(compiler, kClockSource);
    auto k = findMethod(module->getStructures()["test.M"], "stamp");
    ASSERT_NE(k, nullptr);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_clock_amd", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<uint64_t> out(kN, 0);
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "stamp");
    ASSERT_NE(fn, nullptr);
    const std::size_t bytes = std::size_t(kN) * sizeof(uint64_t);
    HipDevicePtr dOut = hip.alloc(bytes);
    ASSERT_NE(dOut, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), bytes));
    void* params[] = {&dOut, (void*) &kN};
    ASSERT_TRUE(hip.launch(fn, (kN + 63) / 64, 64, params));
    ASSERT_TRUE(hip.synchronize());
    std::vector<uint64_t> result(kN);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, bytes));
    hip.free(dOut);

    EXPECT_EQ(countZero(result), 0u) << "some invocations observed a zero clock";
}
