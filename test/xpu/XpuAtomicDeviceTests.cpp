//
// CajetaXPU float atomics — device codegen, end to end.
//
// `Buffer<float32>.atomicAdd/atomicMin/atomicMax(index, value)` lower to an
// atomic read-modify-write on the buffer element pointer. The same @Kernel
// source runs a concurrent reduction on every backend: a parallel sum (atomicAdd
// into out[0]) plus running max/min (out[1]/out[2]). On Vulkan this is
// OpAtomicFAddEXT/FMinEXT/FMaxEXT (SPV_EXT_shader_atomic_float_*); on CPU/AMD the
// native atomicrmw. The host counterpart is the spirv-val emit check in
// XpuVulkanEmitTests.lowersFloatAtomicsToSpirv.
//
// in[i] = i, i in [0, n).  out starts {0, 0, BIG}.
//   out[0] = sum(0..n-1) = n*(n-1)/2   (exact in f32: every partial sum is an
//                                       integer < 2^24, so order-independent)
//   out[1] = max = n-1
//   out[2] = min = 0
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

const char* kAtomicSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void reduce(Buffer<float32> out, Buffer<float32> in,\n"
    "                              uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out.atomicAdd(0, in[i]);\n"
    "            out.atomicMax(1, in[i]);\n"
    "            out.atomicMin(2, in[i]);\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_atomic_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_atomic_arch_" + std::to_string(rng()));
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

// (out, in, n, then the 12 i32 grid coordinates).
using ReduceFn = void (*)(float*, float*, uint32_t,
                          int32_t, int32_t, int32_t,
                          int32_t, int32_t, int32_t,
                          int32_t, int32_t, int32_t,
                          int32_t, int32_t, int32_t);

constexpr uint32_t kN = 4096;
float expectedSum() { return (float) ((uint64_t) kN * (kN - 1) / 2); }  // 8386560
float expectedMax() { return (float) (kN - 1); }                        // 4095
float expectedMin() { return 0.0f; }

std::vector<float> makeInput() {
    std::vector<float> in(kN);
    for (uint32_t i = 0; i < kN; ++i) in[i] = (float) i;
    return in;
}
std::vector<float> initOut() { return {0.0f, 0.0f, 1.0e9f}; }

} // namespace

TEST(XpuAtomicDeviceTests, floatAtomicsRunOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kAtomicSource);
    auto k = findMethod(module->getStructures()["test.M"], "reduce");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_atomic_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("reduce");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto reduce = symOrErr->toPtr<ReduceFn>();

    std::vector<float> in = makeInput();
    std::vector<float> out = initOut();
    const int32_t B = 64;
    const int32_t G = (int32_t) (kN / 64);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            reduce(out.data(), in.data(), kN,
                   tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    EXPECT_FLOAT_EQ(out[0], expectedSum());
    EXPECT_FLOAT_EQ(out[1], expectedMax());
    EXPECT_FLOAT_EQ(out[2], expectedMin());
}

TEST(XpuAtomicDeviceTests, floatAtomicsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, kAtomicSource);
    auto k = findMethod(module->getStructures()["test.M"], "reduce");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_atomic_vk", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    std::vector<float> in = makeInput();
    std::vector<float> out = initOut();
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(out.size() * sizeof(float));
    VulkanDriver::Buffer dIn = vk.alloc(in.size() * sizeof(float));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u); ASSERT_NE(dIn, 0u); ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), out.size() * sizeof(float)));
    ASSERT_TRUE(vk.upload(dIn, in.data(), in.size() * sizeof(float)));
    ASSERT_TRUE(vk.upload(dN, &kN, sizeof(uint32_t)));

    const unsigned block = kVulkanLocalSizeX;
    const unsigned grid = (kN + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "reduce",
                          {dOut, dIn, dN}, grid));

    std::vector<float> result(out.size());
    ASSERT_TRUE(vk.download(result.data(), dOut, result.size() * sizeof(float)));
    vk.free(dOut); vk.free(dIn); vk.free(dN);

    EXPECT_FLOAT_EQ(result[0], expectedSum());
    EXPECT_FLOAT_EQ(result[1], expectedMax());
    EXPECT_FLOAT_EQ(result[2], expectedMin());
}

TEST(XpuAtomicDeviceTests, floatAtomicsRunOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";
    Compiler compiler;
    auto module = compileForInspection(compiler, kAtomicSource);
    auto k = findMethod(module->getStructures()["test.M"], "reduce");
    ASSERT_NE(k, nullptr);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_atomic_amd", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<float> in = makeInput();
    std::vector<float> out = initOut();
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "reduce");
    ASSERT_NE(fn, nullptr);
    HipDevicePtr dOut = hip.alloc(out.size() * sizeof(float));
    HipDevicePtr dIn = hip.alloc(in.size() * sizeof(float));
    ASSERT_NE(dOut, nullptr); ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), out.size() * sizeof(float)));
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), in.size() * sizeof(float)));
    void* params[] = {&dOut, &dIn, (void*) &kN};
    ASSERT_TRUE(hip.launch(fn, (kN + 63) / 64, 64, params));
    ASSERT_TRUE(hip.synchronize());
    std::vector<float> result(out.size());
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, result.size() * sizeof(float)));
    hip.free(dOut); hip.free(dIn);

    EXPECT_FLOAT_EQ(result[0], expectedSum());
    EXPECT_FLOAT_EQ(result[1], expectedMax());
    EXPECT_FLOAT_EQ(result[2], expectedMin());
}
