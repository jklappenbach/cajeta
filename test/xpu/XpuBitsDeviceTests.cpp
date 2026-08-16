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
#include "jit/CoffSafeJit.h"
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
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Bits;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void bitops(KernelBuffer<uint32> out, KernelBuffer<uint32> in,\n"
    "                              uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
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
