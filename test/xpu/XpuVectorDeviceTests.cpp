//
// CajetaXPU Vector<T,N> — device codegen (Stage 5), end to end.
//
// The SAME `Vector<float32,N>` kernel lowers through the shared @Kernel walk
// for every backend and runs: constructed `<N x T>` registers, component
// (`.x`) and indexed (`[i]`) lane access + assignment, element-wise arithmetic
// with scalar broadcast, and the dot/length geometry helpers. The host-side
// counterpart is test/expression/VectorTests.cpp; here the very same operations
// execute on the CPU oracle and (when present) on Vulkan and AMD GPUs.
//
//   v = (1,2,3,4); w = v * 2 = (2,4,6,8); w.x += 10 -> (12,4,6,8)
//   p = (3,4)
//   s = w.x + w[1] + v.z + dot(v,v) + length(p)
//     = 12  + 4    + 3   + 30       + 5          = 54
//   out[i] = s + i  ==  54 + i
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
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
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

// One kernel, every backend. `n` guards the grid tail (needed on the GPUs,
// harmless on the CPU oracle). No `import` for Vector — it is intercepted by
// name in the type resolver, like a primitive.
const char* kVecSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void vecmath(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> v = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "            Vector<float32,4> w = v * 2.0f;\n"
    "            w.x = w.x + 10.0f;\n"
    "            Vector<float32,2> p = new Vector<float32,2>(3.0f, 4.0f);\n"
    "            float32 s = w.x + w[1] + v.z + v.dot(v) + p.length();\n"
    "            out[i] = s + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

// out[i] == 54 + i (see file header for the arithmetic).
float expectedAt(uint32_t i) { return 54.0f + (float) i; }

// S6 interop probe: a Buffer whose element type is itself a vector. Exercises
// buffer-of-vector marshalling (16-byte stride) and a whole-vector store
// `out[i] = <4 x float>` — distinct from the scalar-element buffer above.
//   out[i] = (i, 1, 2, 3) * 2 = (2i, 2, 4, 6)
const char* kVecBufSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class MB {\n"
    "    @Kernel\n"
    "    public static void vecbuf(Buffer<Vector<float32,4>> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> v = new Vector<float32,4>(\n"
    "                (float32) i, 1.0f, 2.0f, 3.0f);\n"
    "            out[i] = v * 2.0f;\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vecdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vecdev_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
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

// CPU lowering's host signature: (out, n, then the 12 i32 grid coordinates).
using VecFn = void (*)(float*, uint32_t,
                       int32_t, int32_t, int32_t,   // tid.{x,y,z}
                       int32_t, int32_t, int32_t,   // ctaid.{x,y,z}
                       int32_t, int32_t, int32_t,   // ntid.{x,y,z}
                       int32_t, int32_t, int32_t);  // nctaid.{x,y,z}

// Same coord ABI; the buffer pointer addresses 16-byte `<4 x float>` elements,
// so the caller backs it with 4 floats per element.
using VecBufFn = VecFn;

} // namespace

// The vector ops lower to honest-to-goodness LLVM vector IR: a `<4 x float>`
// type, element-wise `fmul <4 x float>` (the scalar broadcast `v * 2`), plus
// extractelement (component/index/dot) and insertelement (component assign).
TEST(XpuVectorDeviceTests, lowersToVectorIr) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSource);
    auto k = findMethod(module->getStructures()["test.M"], "vecmath");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_vec_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    EXPECT_NE(ir.find("<4 x float>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("fmul <4 x float>"), std::string::npos) << ir;  // broadcast
    EXPECT_NE(ir.find("extractelement"), std::string::npos) << ir;
    EXPECT_NE(ir.find("insertelement"), std::string::npos) << ir;    // .x assign
}

// The CPU oracle: JIT the lowered kernel and run it over a grid; every element
// must equal 54 + i.
TEST(XpuVectorDeviceTests, runsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSource);
    auto k = findMethod(module->getStructures()["test.M"], "vecmath");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_vec_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("vecmath");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto vecmath = symOrErr->toPtr<VecFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            vecmath(out.data(), N,
                    tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], expectedAt(i)) << "element " << i;
}

// S6: a Buffer<Vector<float32,4>> — buffer element type is a vector. JIT the
// kernel and run it; each 4-float element must equal (2i, 2, 4, 6). Proves the
// 16-byte element stride and a whole-vector store through `out[i] =`.
TEST(XpuVectorDeviceTests, bufferOfVectorRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecBufSource);
    auto k = findMethod(module->getStructures()["test.MB"], "vecbuf");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_vecbuf_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("vecbuf");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto vecbuf = symOrErr->toPtr<VecBufFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(std::size_t(N) * 4, -1.0f);  // 4 floats per element
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            vecbuf(out.data(), N,
                   tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_FLOAT_EQ(out[4 * i + 0], 2.0f * (float) i) << "elem " << i << " .x";
        EXPECT_FLOAT_EQ(out[4 * i + 1], 2.0f) << "elem " << i << " .y";
        EXPECT_FLOAT_EQ(out[4 * i + 2], 4.0f) << "elem " << i << " .z";
        EXPECT_FLOAT_EQ(out[4 * i + 3], 6.0f) << "elem " << i << " .w";
    }
}

// On a real GPU via Vulkan compute. Skips cleanly when no device is present.
TEST(XpuVectorDeviceTests, runsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSource);
    auto k = findMethod(module->getStructures()["test.M"], "vecmath");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vec_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "vecmath",
                          {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != expectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << expectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// On a real GPU via AMD HIP. Skips cleanly when no device / assembler present.
TEST(XpuVectorDeviceTests, runsOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no HIP device";

    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSource);
    auto k = findMethod(module->getStructures()["test.M"], "vecmath");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vec_amddevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> hsaco = assembleHsaco(deviceModule, *tm, "gfx1151");
    ASSERT_FALSE(hsaco.empty()) << "hsaco assembly failed (ld.lld present?)";

    const uint32_t n = 1u << 16;
    std::vector<float> out(n, -1.0f);

    HipDriver hip;
    ASSERT_TRUE(hip.init());
    HipModule mod = hip.loadModule(hsaco.data(), hsaco.size());
    ASSERT_NE(mod, nullptr);
    HipFunction fn = hip.getFunction(mod, "vecmath");
    ASSERT_NE(fn, nullptr);

    const std::size_t bytes = std::size_t(n) * sizeof(float);
    HipDevicePtr dOut = hip.alloc(bytes);
    ASSERT_NE(dOut, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), bytes));

    void* params[] = { &dOut, (void*) &n };
    const unsigned block = 256;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(hip.launch(fn, grid, block, params));
    ASSERT_TRUE(hip.synchronize());

    std::vector<float> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, bytes));
    hip.free(dOut);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != expectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << expectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}
