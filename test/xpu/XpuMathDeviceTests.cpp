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
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
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
// B2 increment 2 — transcendentals, chosen at exact-valued inputs:
//   sin0=0 cos0=1 tan0=0 exp0=1 log1=0 pow(2,3)=8 sqrt16=4
//   rsqrt(0.25)=2 acos1=0  -> sum 16 ;  out[i] = 16 + i
const char* kTranscendentalSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class T {\n"
    "    @Kernel\n"
    "    public static void trans(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i] = Math.sin(0.0f) + Math.cos(0.0f) + Math.tan(0.0f)\n"
    "                   + Math.exp(0.0f) + Math.log(1.0f) + Math.pow(2.0f, 3.0f)\n"
    "                   + Math.sqrt(16.0f) + Math.rsqrt(0.25f) + Math.acos(1.0f)\n"
    "                   + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float transExpectedAt(uint32_t i) { return 16.0f + (float) i; }

// Vectorized math: Math.<fn> applied to a `Vector<float32,N>` -> elementwise.
// sin(0)=0 cos(0)=1 exp(0)=1 (per lane); sqrt((4,9,16,25))=(2,3,4,5).
//   out[i] = s.x + c.x + c.y + e.z + r.x+r.y+r.z+r.w
//          = 0 + 1 + 1 + 1 + 2+3+4+5 = 17 ;  out[i] = 17 + i
const char* kVectorMathSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "import cajeta.lang.Math;\n"
    "public class V {\n"
    "    @Kernel\n"
    "    public static void vmath(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> z = heap Vector<float32,4>(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "            Vector<float32,4> s = Math.sin(z);\n"
    "            Vector<float32,4> c = Math.cos(z);\n"
    "            Vector<float32,4> e = Math.exp(z);\n"
    "            Vector<float32,4> q = heap Vector<float32,4>(4.0f, 9.0f, 16.0f, 25.0f);\n"
    "            Vector<float32,4> r = Math.sqrt(q);\n"
    "            out[i] = s.x + c.x + c.y + e.z + r.x + r.y + r.z + r.w + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float vectorMathExpectedAt(uint32_t i) { return 17.0f + (float) i; }

// @FastMath relaxes IEEE FP: the body's ops carry the LLVM fast-math flags
// (contract/reassoc/arcp/afn/...) so the backend may fuse to FMA, use
// reciprocals, and approximate transcendentals. out[i] = 2*i + 1 (exact).
const char* kFastMathSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class F {\n"
    "    @Kernel\n"
    "    @FastMath\n"
    "    public static void fastk(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { out[i] = (float32) i * 2.0f + 1.0f; }\n"
    "    }\n"
    "}\n";
// Same kernel without @FastMath — the FP ops are emitted IEEE-strict (no flags).
const char* kPreciseSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class P {\n"
    "    @Kernel\n"
    "    public static void precisek(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) { out[i] = (float32) i * 2.0f + 1.0f; }\n"
    "    }\n"
    "}\n";

float fastMathExpectedAt(uint32_t i) { return 2.0f * (float) i + 1.0f; }

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

// Wave-3 bugfix (H13-H16): device-kernel numeric literals must honor the radix
// (hex/bin/oct), digit-group underscores, and the declared width — the old path
// used std::stoll (decimal-only, stops at '_', i32-truncating, throws on overflow).
TEST(XpuMathDeviceTests, lowersNumericLiteralsCorrectly) {
    const char* src =
        "package test;\n"
        "import cajeta.gpu.core.Buffer;\n"
        "import cajeta.gpu.core.Thread;\n"
        "public class L {\n"
        "    @Kernel\n"
        "    public static void litk(Buffer<int32> out, Buffer<int64> out64,\n"
        "                            uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i == 0) {\n"
        "            out[0] = 0xDEAD;\n"          // hex     -> 57005 (bug: 0)
        "            out[1] = 9_999_999;\n"       // _ group -> 9999999 (bug: 9)
        "            out[2] = 0b1111000011110000;\n" // binary -> 61680 (bug: 0)
        "            out64[0] = 5000000000L;\n"   // 64-bit  -> not i32-truncated
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "L");
    auto k = findMethod(module->getStructures()["test.L"], "litk");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_lit_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);
    std::string ir = printModule(host);
    for (const char* tok : {"57005", "9999999", "61680", "5000000000"})
        EXPECT_NE(ir.find(tok), std::string::npos) << tok << "\n" << ir;
    EXPECT_EQ(ir.find("705032704"), std::string::npos)  // i32-truncated 5e9
        << "64-bit literal was truncated to i32\n" << ir;
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

// B2 increment 2 — transcendentals lower to the llvm.* intrinsics (CPU -> libm,
// Vulkan -> OpExtInst GLSL.std.450, AMD/NV -> device math library).
TEST(XpuMathDeviceTests, transcendentalsLowerToIntrinsics) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTranscendentalSource, "T");
    auto k = findMethod(module->getStructures()["test.T"], "trans");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_math_trans_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    for (const char* tok : {"llvm.sin", "llvm.cos", "llvm.tan", "llvm.exp",
                            "llvm.log", "llvm.pow", "llvm.acos"})
        EXPECT_NE(ir.find(tok), std::string::npos) << tok << " missing\n" << ir;
}

// CPU oracle: the transcendental kernel runs via libm. out[i] == 16 + i.
TEST(XpuMathDeviceTests, transcendentalsRunOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kTranscendentalSource, "T");
    auto k = findMethod(module->getStructures()["test.T"], "trans");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_math_trans_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("trans");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto trans = sym->toPtr<MathFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            trans(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], transExpectedAt(i), 1e-3f) << "element " << i;
}

// On a real GPU via Vulkan: the SPIR-V backend maps the intrinsics to
// OpExtInst GLSL.std.450. out[i] == 16 + i.
TEST(XpuMathDeviceTests, transcendentalsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kTranscendentalSource, "T");
    auto k = findMethod(module->getStructures()["test.T"], "trans");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_math_trans_vk", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "trans", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (std::abs(result[i] - transExpectedAt(i)) > 1e-2f) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << transExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
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

// B2 increment 2 — transcendentals on a real AMD GPU via HIP. Probes whether the
// AMDGPU backend needs ocml linking for sin/cos/etc. out[i] == 16 + i.
TEST(XpuMathDeviceTests, transcendentalsRunOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";

    Compiler compiler;
    auto module = compileForInspection(compiler, kTranscendentalSource, "T");
    auto k = findMethod(module->getStructures()["test.T"], "trans");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_math_trans_amd", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed — transcendentals may "
                                   "need ocml linking on AMDGPU";

    const uint32_t n = 1u << 12;
    std::vector<float> out(n, -1.0f);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr) << "module load failed — undefined device math symbol?";
    HipFunction fn = hip.getFunction(mod, "trans");
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
        EXPECT_NEAR(result[i], transExpectedAt(i), 1e-2f) << "element " << i;
}

// Vectorized transcendentals over <N x float> — CPU oracle. out[i] == 17 + i.
TEST(XpuMathDeviceTests, vectorMathRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVectorMathSource, "V");
    auto k = findMethod(module->getStructures()["test.V"], "vmath");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_vecmath_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("vmath");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto vmath = sym->toPtr<MathFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            vmath(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], vectorMathExpectedAt(i), 1e-3f) << "element " << i;
}

// Vectorized transcendentals on a real GPU via Vulkan (vector OpExtInst).
TEST(XpuMathDeviceTests, vectorMathRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, kVectorMathSource, "V");
    auto k = findMethod(module->getStructures()["test.V"], "vmath");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vecmath_vk", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "vmath", {dOut, dN}, grid));
    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut); vk.free(dN);
    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (std::abs(result[i] - vectorMathExpectedAt(i)) > 1e-2f) {
            if (mismatches < 5) ADD_FAILURE() << "i=" << i << " got " << result[i];
            ++mismatches;
        }
    EXPECT_EQ(mismatches, 0u);
}

// Vectorized transcendentals on AMD via HIP — ocml scalarized per lane.
TEST(XpuMathDeviceTests, vectorMathRunsOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";
    Compiler compiler;
    auto module = compileForInspection(compiler, kVectorMathSource, "V");
    auto k = findMethod(module->getStructures()["test.V"], "vmath");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vecmath_amd", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed";

    const uint32_t n = 1u << 12;
    std::vector<float> out(n, -1.0f);
    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "vmath");
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
        EXPECT_NEAR(result[i], vectorMathExpectedAt(i), 1e-2f) << "element " << i;
}

// @FastMath stamps the LLVM fast-math flags on the body's FP ops; the plain
// kernel does not. (The flags appear as "fast" / "contract" on fmul/fadd.)
TEST(XpuMathDeviceTests, fastMathFlagsEmitted) {
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);

    Compiler c1;
    auto mf = compileForInspection(c1, kFastMathSource, "F");
    auto kf = findMethod(mf->getStructures()["test.F"], "fastk");
    ASSERT_NE(kf, nullptr);
    llvm::LLVMContext ctxF;
    llvm::Module hostF("xpu_fast_emit", ctxF);
    cajeta::xpu::cpu::configureHostModule(hostF, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(kf, hostF), nullptr);
    std::string irFast = printModule(hostF);

    Compiler c2;
    auto mp = compileForInspection(c2, kPreciseSource, "P");
    auto kp = findMethod(mp->getStructures()["test.P"], "precisek");
    ASSERT_NE(kp, nullptr);
    llvm::LLVMContext ctxP;
    llvm::Module hostP("xpu_precise_emit", ctxP);
    cajeta::xpu::cpu::configureHostModule(hostP, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(kp, hostP), nullptr);
    std::string irPrecise = printModule(hostP);

    // The @FastMath body carries fast-math flags on its float ops; the plain one
    // emits a strict `fmul float` / `fadd float` with no flags.
    EXPECT_NE(irFast.find("fmul fast"), std::string::npos) << irFast;
    EXPECT_NE(irFast.find("fadd fast"), std::string::npos) << irFast;
    EXPECT_EQ(irPrecise.find("fmul fast"), std::string::npos) << irPrecise;
    EXPECT_NE(irPrecise.find("fmul float"), std::string::npos) << irPrecise;
}

// A @FastMath kernel still computes correctly — out[i] = 2*i + 1 (CPU oracle).
TEST(XpuMathDeviceTests, fastMathRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kFastMathSource, "F");
    auto k = findMethod(module->getStructures()["test.F"], "fastk");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_fast_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("fastk");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto fastk = sym->toPtr<MathFn>();
    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            fastk(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);
    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], fastMathExpectedAt(i), 1e-3f) << "element " << i;
}

// @FastMath on a real GPU via Vulkan — the flags ride into the SPIR-V and the
// result is still correct.
TEST(XpuMathDeviceTests, fastMathRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, kFastMathSource, "F");
    auto k = findMethod(module->getStructures()["test.F"], "fastk");
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_fast_vk", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "fastk", {dOut, dN}, grid));
    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut); vk.free(dN);
    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (std::abs(result[i] - fastMathExpectedAt(i)) > 1e-2f) {
            if (mismatches < 5) ADD_FAILURE() << "i=" << i << " got " << result[i];
            ++mismatches;
        }
    EXPECT_EQ(mismatches, 0u);
}
