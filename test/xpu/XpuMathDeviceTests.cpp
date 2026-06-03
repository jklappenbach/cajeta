//
// CajetaGPU device math intrinsics (Stage B2, increment 1) — the subset that
// lowers natively on every backend with no device math-library link:
// sqrt/floor/ceil/trunc/round, abs, min/max, fma. The SAME kernel lowers
// through the shared @Kernel walk and runs on the CPU oracle and (when present)
// on Vulkan and AMD GPUs. Transcendentals (sin/cos/exp/log/pow) are a later
// increment (device-lib linking) and are asserted to give a clean diagnostic.
//
//   x = (float) i
//   r = sqrt(x) + floor(x*0.5) + ceil(x*0.25) + |x-10|
//     + min(x,5) + max(x,3) + fma(x,2,1)
//   out[i] = r
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
#include "cajeta/error/Exception.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

const char* kMathSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mathk(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            float32 x = (float32) i;\n"
    "            float32 r = Math.sqrt(x)\n"
    "                      + Math.floor(x * 0.5f)\n"
    "                      + Math.ceil(x * 0.25f)\n"
    "                      + Math.abs(x - 10.0f)\n"
    "                      + Math.min(x, 5.0f)\n"
    "                      + Math.max(x, 3.0f)\n"
    "                      + Math.fma(x, 2.0f, 1.0f);\n"
    "            out[i] = r;\n"
    "        }\n"
    "    }\n"
    "}\n";

// A kernel using a transcendental — must lower to a clean diagnostic for now,
// never a miscompile.
const char* kTranscendentalSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class T {\n"
    "    @Kernel\n"
    "    public static void sink(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { out[i] = Math.sin((float32) i); }\n"
    "    }\n"
    "}\n";

float expectedAt(uint32_t i) {
    float x = (float) i;
    return std::sqrt(x) + std::floor(x * 0.5f) + std::ceil(x * 0.25f)
         + std::fabs(x - 10.0f) + std::min(x, 5.0f) + std::max(x, 3.0f)
         + std::fma(x, 2.0f, 1.0f);
}

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& cls) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_mathdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / (cls + ".cajeta")) << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_mathdev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / (cls + ".cajeta");
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

// CPU host signature: (out, n, then the 12 i32 grid coordinates).
using MathFn = void (*)(float*, uint32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t);

} // namespace

// The native-subset math ops lower to honest LLVM intrinsics in the kernel IR.
TEST(XpuMathDeviceTests, lowersToMathIntrinsics) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMathSource, "M");
    auto k = findMethod(module->getStructures()["test.M"], "mathk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_math_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    for (const char* tok : {"llvm.sqrt", "llvm.floor", "llvm.ceil", "llvm.fabs",
                            "llvm.minnum", "llvm.maxnum", "llvm.fma"})
        EXPECT_NE(ir.find(tok), std::string::npos) << tok << "\n" << ir;
}

// CPU oracle: JIT and run the kernel over a grid; every element must match the
// reference math.
TEST(XpuMathDeviceTests, runsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMathSource, "M");
    auto k = findMethod(module->getStructures()["test.M"], "mathk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_math_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("mathk");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto mathk = symOrErr->toPtr<MathFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            mathk(out.data(), N,
                  tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], expectedAt(i), 1e-3f) << "element " << i;
}

// A transcendental in a kernel is rejected cleanly (XPU-N01), not miscompiled.
TEST(XpuMathDeviceTests, transcendentalRejectedCleanly) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTranscendentalSource, "T");
    auto k = findMethod(module->getStructures()["test.T"], "sink");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_math_trans", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    try {
        cajeta::xpu::cpu::lowerKernel(k, host);
        FAIL() << "expected a clean XPU diagnostic for Math.sin on device";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-N01");
    }
}

// On a real GPU via Vulkan compute. Skips cleanly when no device is present.
TEST(XpuMathDeviceTests, runsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kMathSource, "M");
    auto k = findMethod(module->getStructures()["test.M"], "mathk");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_math_vkdevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    const uint32_t n = 1u << 12;
    std::vector<float> out(n, -1.0f);

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    const std::size_t bytes = std::size_t(n) * sizeof(float);
    VulkanDriver::Buffer dOut = vk.alloc(bytes);
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), bytes));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));

    const unsigned block = kVulkanLocalSizeX;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "mathk",
                          {dOut, dN}, grid));
    ASSERT_TRUE(vk.download(out.data(), dOut, bytes));
    for (uint32_t i = 0; i < n; ++i)
        EXPECT_NEAR(out[i], expectedAt(i), 1e-2f) << "element " << i;
}

// On a real GPU via AMD HIP. Skips cleanly when no device is present.
TEST(XpuMathDeviceTests, runsOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) {
        GTEST_SKIP() << "no AMD HIP device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kMathSource, "M");
    auto k = findMethod(module->getStructures()["test.M"], "mathk");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_math_amddevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    const uint32_t n = 1u << 12;
    std::vector<float> out(n, -1.0f);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "mathk");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(float);
    HipDevicePtr dOut = hip.alloc(bytes);
    ASSERT_NE(dOut, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), bytes));

    void* params[] = {&dOut, (void*) &n};
    const unsigned block = 64;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(hip.launch(fn, grid, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, bytes));
    hip.free(dOut);
    for (uint32_t i = 0; i < n; ++i)
        EXPECT_NEAR(result[i], expectedAt(i), 1e-2f) << "element " << i;
}
