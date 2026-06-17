//
// CajetaXPU bit instructions — device codegen, end to end.
//
// `Bits.reverse/count/rotateLeft/rotateRight(value)` lower to GENERIC LLVM
// intrinsics (llvm.bitreverse / llvm.ctpop) and an inline shift/or rotate — no
// SPIR-V extension, no fork. The same @Kernel runs on every backend and the
// result is bit-exact: on Vulkan these are core OpBitReverse / OpBitCount under
// the plain Shader capability; on CPU/AMD the native brev/popcnt/rotate. The
// host counterpart is the spirv-val emit check in
// XpuVulkanEmitTests.lowersBitInstructionsToSpirv.
//
// in[i] = Knuth hash of i (varied bit pattern). The kernel writes four regions:
//   out[      i] = reverse(in[i])
//   out[  n + i] = popcount(in[i])
//   out[2*n + i] = rotateLeft(in[i], 5)
//   out[3*n + i] = rotateRight(in[i], 5)
// checked against a C++ reference, exactly.
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

const char* kBitsSource =
    "package test;\n"
    "import cajeta.gpu.GpuBuffer;\n"
    "import cajeta.gpu.GpuThread;\n"
    "import cajeta.gpu.Bits;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bitops(GpuBuffer<uint32> out, GpuBuffer<uint32> in,\n"
    "                              uint32 n) {\n"
    "        uint32 i = GpuThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            uint32 v = in[i];\n"
    "            out[i] = Bits.reverse(v);\n"
    "            out[n + i] = Bits.count(v);\n"
    "            out[2 * n + i] = Bits.rotateLeft(v, 5);\n"
    "            out[3 * n + i] = Bits.rotateRight(v, 5);\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_bits_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_bits_arch_" + std::to_string(rng()));
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
using BitsFn = void (*)(uint32_t*, uint32_t*, uint32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t);

constexpr uint32_t kN = 1024;

uint32_t refReverse(uint32_t v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    v = (v << 16) | (v >> 16);
    return v;
}
uint32_t refRotl(uint32_t v, uint32_t s) {
    s &= 31u;
    return s == 0 ? v : ((v << s) | (v >> (32u - s)));
}
uint32_t refRotr(uint32_t v, uint32_t s) {
    s &= 31u;
    return s == 0 ? v : ((v >> s) | (v << (32u - s)));
}

std::vector<uint32_t> makeInput() {
    std::vector<uint32_t> in(kN);
    for (uint32_t i = 0; i < kN; ++i) in[i] = i * 2654435761u;  // Knuth hash
    return in;
}

void checkResult(const std::vector<uint32_t>& in,
                 const std::vector<uint32_t>& out) {
    for (uint32_t i = 0; i < kN; ++i) {
        uint32_t v = in[i];
        EXPECT_EQ(out[i], refReverse(v)) << "reverse @ " << i;
        EXPECT_EQ(out[kN + i], (uint32_t) __builtin_popcount(v))
            << "count @ " << i;
        EXPECT_EQ(out[2 * kN + i], refRotl(v, 5)) << "rotl @ " << i;
        EXPECT_EQ(out[3 * kN + i], refRotr(v, 5)) << "rotr @ " << i;
    }
}

} // namespace

TEST(XpuBitsDeviceTests, bitInstructionsRunOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kBitsSource);
    auto k = findMethod(module->getStructures()["test.M"], "bitops");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_bits_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("bitops");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto bitops = symOrErr->toPtr<BitsFn>();

    std::vector<uint32_t> in = makeInput();
    std::vector<uint32_t> out(4 * kN, 0);
    const int32_t B = 64;
    const int32_t G = (int32_t) (kN / 64);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            bitops(out.data(), in.data(), kN,
                   tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    checkResult(in, out);
}

TEST(XpuBitsDeviceTests, bitInstructionsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, kBitsSource);
    auto k = findMethod(module->getStructures()["test.M"], "bitops");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_bits_vk", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    std::vector<uint32_t> in = makeInput();
    std::vector<uint32_t> out(4 * kN, 0);
    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    VulkanDriver::Buffer dOut = vk.alloc(out.size() * sizeof(uint32_t));
    VulkanDriver::Buffer dIn = vk.alloc(in.size() * sizeof(uint32_t));
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dOut, 0u); ASSERT_NE(dIn, 0u); ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dOut, out.data(), out.size() * sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dIn, in.data(), in.size() * sizeof(uint32_t)));
    ASSERT_TRUE(vk.upload(dN, &kN, sizeof(uint32_t)));

    const unsigned block = kVulkanLocalSizeX;
    const unsigned grid = (kN + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "bitops",
                          {dOut, dIn, dN}, grid));

    std::vector<uint32_t> result(out.size());
    ASSERT_TRUE(vk.download(result.data(), dOut,
                            result.size() * sizeof(uint32_t)));
    vk.free(dOut); vk.free(dIn); vk.free(dN);

    checkResult(in, result);
}

TEST(XpuBitsDeviceTests, bitInstructionsRunOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";
    Compiler compiler;
    auto module = compileForInspection(compiler, kBitsSource);
    auto k = findMethod(module->getStructures()["test.M"], "bitops");
    ASSERT_NE(k, nullptr);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_bits_amd", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    std::vector<uint32_t> in = makeInput();
    std::vector<uint32_t> out(4 * kN, 0);
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "bitops");
    ASSERT_NE(fn, nullptr);
    HipDevicePtr dOut = hip.alloc(out.size() * sizeof(uint32_t));
    HipDevicePtr dIn = hip.alloc(in.size() * sizeof(uint32_t));
    ASSERT_NE(dOut, nullptr); ASSERT_NE(dIn, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), out.size() * sizeof(uint32_t)));
    ASSERT_TRUE(hip.memcpyHtoD(dIn, in.data(), in.size() * sizeof(uint32_t)));
    void* params[] = {&dOut, &dIn, (void*) &kN};
    ASSERT_TRUE(hip.launch(fn, (kN + 63) / 64, 64, params));
    ASSERT_TRUE(hip.synchronize());
    std::vector<uint32_t> result(out.size());
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut,
                               result.size() * sizeof(uint32_t)));
    hip.free(dOut); hip.free(dIn);

    checkResult(in, result);
}
