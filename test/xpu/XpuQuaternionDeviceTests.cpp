//
// Quaternion<T> device codegen — construction, `*` (Hamilton product + vector
// rotation), and the normalize/conjugate/length/dot methods, on the CPU oracle
// and (when present) a real Vulkan GPU. The same `<4 x T>` representation the
// host uses; the device walker tracks quaternion-ness by name (quaternionLocals).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
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
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// i*j = k; rotate (1,0,0) by 90 deg about z -> (0,1,0); q unit.
//   s = ij.dot(k) + r.y + q.length() = 1 + 1 + 1 = 3 ;  out[i] = 3 + i
const char* kQuatSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class Q {\n"
    "    @Kernel\n"
    "    public static void quatk(Buffer<float32> out, uint32 n) {\n"
    "        uint32 idx = Thread.globalIdX();\n"
    "        if (idx < n) {\n"
    "            Quaternion<float32> i = new Quaternion<float32>(0.0f, 1.0f, 0.0f, 0.0f);\n"
    "            Quaternion<float32> j = new Quaternion<float32>(0.0f, 0.0f, 1.0f, 0.0f);\n"
    "            Quaternion<float32> k = new Quaternion<float32>(0.0f, 0.0f, 0.0f, 1.0f);\n"
    "            Quaternion<float32> ij = i * j;\n"
    "            Quaternion<float32> q = new Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f);\n"
    "            Vector<float32,3> v = new Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
    "            Vector<float32,3> r = q * v;\n"
    "            float32 s = ij.dot(k) + r.y + q.length();\n"
    "            out[idx] = s + (float32) idx;\n"
    "        }\n"
    "    }\n"
    "}\n";

float quatExpectedAt(uint32_t i) { return 3.0f + (float) i; }

using QFn = void (*)(float*, uint32_t,
                     int32_t, int32_t, int32_t,
                     int32_t, int32_t, int32_t,
                     int32_t, int32_t, int32_t,
                     int32_t, int32_t, int32_t);

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_quatdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "Q.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_quatdev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "Q.cajeta";
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

// CPU oracle: i*j=k, 90-deg rotation, length — out[i] == 3 + i.
TEST(XpuQuaternionDeviceTests, runsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kQuatSource);
    auto k = findMethod(module->getStructures()["test.Q"], "quatk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_quat_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("quatk");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto quatk = sym->toPtr<QFn>();

    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            quatk(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], quatExpectedAt(i), 1e-4f) << "element " << i;
}

// On a real GPU via Vulkan. out[i] == 3 + i (within fp tolerance).
TEST(XpuQuaternionDeviceTests, runsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kQuatSource);
    auto k = findMethod(module->getStructures()["test.Q"], "quatk");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_quat_vkdevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    const uint32_t n = 1u << 16;
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "quatk", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (std::abs(result[i] - quatExpectedAt(i)) > 1e-3f) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << quatExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}
