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
#include "XpuCpuJit.h"
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

// One kernel, every backend. `n` guards the grid tail (needed on the GPUs,
// harmless on the CPU oracle). No `import` for Vector — it is intercepted by
// name in the type resolver, like a primitive.
const char* kVecSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void vecmath(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "            Vector<float32,4> w = v * 2.0f;\n"
    "            w.x = w.x + 10.0f;\n"
    "            Vector<float32,2> p = heap Vector<float32,2>(3.0f, 4.0f);\n"
    "            float32 s = w.x + w[1] + v.z + v.dot(v) + p.length();\n"
    "            out[i] = s + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

// out[i] == 54 + i (see file header for the arithmetic).
float expectedAt(uint32_t i) { return 54.0f + (float) i; }

// B1 intrinsics A1 — element-wise min/max, scalar-bound clamp, and lerp on the
// device. a=(1,5,3,2) b=(4,2,6,8):
//   mn = min(a,b) = (1,2,3,2);  mx = max(a,b) = (4,5,6,8)
//   cl = clamp(b, 3, 7) = (4,3,6,7)
//   lp = lerp(a, b, 0.5) = (2.5, 3.5, 4.5, 5.0)
//   s = mn.x + mx.w + cl.y + lp.z = 1 + 8 + 3 + 4.5 = 16.5
//   out[i] = s + i  ==  16.5 + i
const char* kVecIntrinSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MI {\n"
    "    @Kernel\n"
    "    public static void vintrin(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> a = heap Vector<float32,4>(1.0f, 5.0f, 3.0f, 2.0f);\n"
    "            Vector<float32,4> b = heap Vector<float32,4>(4.0f, 2.0f, 6.0f, 8.0f);\n"
    "            Vector<float32,4> mn = a.min(b);\n"
    "            Vector<float32,4> mx = a.max(b);\n"
    "            Vector<float32,4> cl = b.clamp(3.0f, 7.0f);\n"
    "            Vector<float32,4> lp = a.lerp(b, 0.5f);\n"
    "            float32 s = mn.x + mx.w + cl.y + lp.z;\n"
    "            out[i] = s + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float intrinExpectedAt(uint32_t i) { return 16.5f + (float) i; }

// B1 intrinsics A2 — geometry on the device: cross, reflect, distance.
//   cr = cross((1,0,0),(0,1,0)) = (0,0,1)        -> cr.z = 1
//   rf = reflect((1,-1),(0,1))  = (1,1)          -> rf.x + rf.y = 2
//   d  = distance((0,0),(3,4))  = 5
//   s = cr.z + rf.x + rf.y + d = 1 + 2 + 5 = 8;  out[i] = 8 + i
const char* kVecGeomSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MG {\n"
    "    @Kernel\n"
    "    public static void vgeom(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 idx = KernelThread.globalIdX();\n"
    "        if (idx < n) {\n"
    "            Vector<float32,3> a = heap Vector<float32,3>(1.0f, 0.0f, 0.0f);\n"
    "            Vector<float32,3> b = heap Vector<float32,3>(0.0f, 1.0f, 0.0f);\n"
    "            Vector<float32,3> cr = a.cross(b);\n"
    "            Vector<float32,2> iv = heap Vector<float32,2>(1.0f, -1.0f);\n"
    "            Vector<float32,2> nv = heap Vector<float32,2>(0.0f, 1.0f);\n"
    "            Vector<float32,2> rf = iv.reflect(nv);\n"
    "            Vector<float32,2> p = heap Vector<float32,2>(0.0f, 0.0f);\n"
    "            Vector<float32,2> q = heap Vector<float32,2>(3.0f, 4.0f);\n"
    "            float32 d = p.distance(q);\n"
    "            float32 s = cr.z + rf.x + rf.y + d;\n"
    "            out[idx] = s + (float32) idx;\n"
    "        }\n"
    "    }\n"
    "}\n";

float geomExpectedAt(uint32_t i) { return 8.0f + (float) i; }

// B intrinsics — comparison masks + select/any/all on the device.
//   a=(1,5,3,2) b=(4,2,6,8):  (a < b) = (T,F,T,T)
//   mn = (a < b).select(a, b) = (1,2,3,2)  (element-wise min) -> sum 8
//   (a < b).any() = true  -> +1 ;  (a < b).all() = false -> +0
//   s = 8 + 1 = 9 ;  out[i] = 9 + i
const char* kVecMaskSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MM {\n"
    "    @Kernel\n"
    "    public static void vmask(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 idx = KernelThread.globalIdX();\n"
    "        if (idx < n) {\n"
    "            Vector<float32,4> a = heap Vector<float32,4>(1.0f, 5.0f, 3.0f, 2.0f);\n"
    "            Vector<float32,4> b = heap Vector<float32,4>(4.0f, 2.0f, 6.0f, 8.0f);\n"
    "            Vector<float32,4> mn = (a < b).select(a, b);\n"
    "            float32 extra = 0.0f;\n"
    "            if ((a < b).any()) { extra = extra + 1.0f; }\n"
    "            if ((a < b).all()) { extra = extra + 10.0f; }\n"
    "            float32 s = mn.x + mn.y + mn.z + mn.w + extra;\n"
    "            out[idx] = s + (float32) idx;\n"
    "        }\n"
    "    }\n"
    "}\n";

float maskExpectedAt(uint32_t i) { return 9.0f + (float) i; }

// C — multi-component swizzle reads on the device. v=(1,2,3,4):
//   zyx=(3,2,1) -> .x=3 ; xy=(1,2) -> .y=2 ; xxyy=(1,1,2,2) -> .z=2
//   s = 3 + 2 + 2 = 7 ;  out[i] = 7 + i
const char* kVecSwizSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MS {\n"
    "    @Kernel\n"
    "    public static void vswiz(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 idx = KernelThread.globalIdX();\n"
    "        if (idx < n) {\n"
    "            Vector<float32,4> v = heap Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "            Vector<float32,3> zyx = v.zyx;\n"
    "            Vector<float32,2> xy = v.xy;\n"
    "            Vector<float32,4> xxyy = v.xxyy;\n"
    "            float32 s = zyx.x + xy.y + xxyy.z;\n"
    "            out[idx] = s + (float32) idx;\n"
    "        }\n"
    "    }\n"
    "}\n";

float swizExpectedAt(uint32_t i) { return 7.0f + (float) i; }

// B2 — half / bfloat16 vector element types (ML/graphics dtypes). Each thread
// does <4 x half> / <4 x bfloat> arithmetic, casting the result back to f32.
//   a=(1,2,3,4) b=(10,20,30,40); c=a+b=(11,22,33,44); out = c.x + c.w = 55
const char* kF16Source =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class HF {\n"
    "    @Kernel\n"
    "    public static void f16k(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float16,4> a = heap Vector<float16,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "            Vector<float16,4> b = heap Vector<float16,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
    "            Vector<float16,4> c = a + b;\n"
    "            out[i] = (float32) c.x + (float32) c.w + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";
const char* kBf16Source =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class HB {\n"
    "    @Kernel\n"
    "    public static void bf16k(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<bfloat16,4> a = heap Vector<bfloat16,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
    "            Vector<bfloat16,4> b = heap Vector<bfloat16,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
    "            Vector<bfloat16,4> c = a + b;\n"
    "            out[i] = (float32) c.x + (float32) c.w + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float halfExpectedAt(uint32_t i) { return 55.0f + (float) i; }

// S6 interop probe: a KernelBuffer whose element type is itself a vector. Exercises
// buffer-of-vector marshalling (16-byte stride) and a whole-vector store
// `out[i] = <4 x float>` — distinct from the scalar-element buffer above.
//   out[i] = (i, 1, 2, 3) * 2 = (2i, 2, 4, 6)
const char* kVecBufSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MB {\n"
    "    @Kernel\n"
    "    public static void vecbuf(KernelBuffer<Vector<float32,4>> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<float32,4> v = heap Vector<float32,4>(\n"
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

    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
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

// S6: a KernelBuffer<Vector<float32,4>> — buffer element type is a vector. JIT the
// kernel and run it; each 4-float element must equal (2i, 2, 4, 6). Proves the
// 16-byte element stride and a whole-vector store through `out[i] =`.

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

// B1 intrinsics A1 — min/max/clamp/lerp on the CPU oracle. out[i] == 16.5 + i.

// B1 intrinsics A1 on a real GPU via Vulkan. out[i] == 16.5 + i.
TEST(XpuVectorDeviceTests, intrinsicsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecIntrinSource);
    auto k = findMethod(module->getStructures()["test.MI"], "vintrin");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vintrin_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "vintrin", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != intrinExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << intrinExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// B1 intrinsics A2 — cross/reflect/distance on the CPU oracle. out[i] == 8 + i.
TEST(XpuVectorDeviceTests, geometryRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecGeomSource);
    auto k = findMethod(module->getStructures()["test.MG"], "vgeom");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_vgeom_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("vgeom");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto vgeom = sym->toPtr<VecFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            vgeom(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], geomExpectedAt(i)) << "element " << i;
}

// B1 intrinsics A2 on a real GPU via Vulkan. out[i] == 8 + i.

// B intrinsics — comparison masks + select/any/all on the CPU oracle. 9 + i.

// B intrinsics — masks on a real GPU via Vulkan. out[i] == 9 + i.
TEST(XpuVectorDeviceTests, masksRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecMaskSource);
    auto k = findMethod(module->getStructures()["test.MM"], "vmask");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vmask_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "vmask", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != maskExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << maskExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// C — swizzle reads on the CPU oracle. out[i] == 7 + i.
TEST(XpuVectorDeviceTests, swizzleRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSwizSource);
    auto k = findMethod(module->getStructures()["test.MS"], "vswiz");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_vswiz_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("vswiz");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto vswiz = sym->toPtr<VecFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            vswiz(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], swizExpectedAt(i)) << "element " << i;
}

// C — swizzle reads on a real GPU via Vulkan. out[i] == 7 + i.
TEST(XpuVectorDeviceTests, swizzleRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kVecSwizSource);
    auto k = findMethod(module->getStructures()["test.MS"], "vswiz");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_vswiz_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "vswiz", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != swizExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << swizExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// half/bfloat16 vector arithmetic on the CPU oracle (cls = "HF" or "HB").
static void runHalfOnCpu(const char* src, const char* cls, const char* kern) {
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()[std::string("test.") + cls], kern);
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_half_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);
    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup(kern);
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto fn = sym->toPtr<VecFn>();
    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            fn(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);
    for (uint32_t i = 0; i < N; ++i)
        EXPECT_NEAR(out[i], halfExpectedAt(i), 0.5f) << "element " << i;
}


// half/bfloat16 vector arithmetic on a real GPU via Vulkan (Float16 capability).
static void runHalfOnVulkan(const char* src, const char* cls, const char* kern) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) GTEST_SKIP() << "no Vulkan compute device";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()[std::string("test.") + cls], kern);
    ASSERT_NE(k, nullptr);
    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_half_vk", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";
    const uint32_t n = 1u << 14;
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), kern, {dOut, dN}, grid));
    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut); vk.free(dN);
    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (std::abs(result[i] - halfExpectedAt(i)) > 0.5f) {
            if (mismatches < 5) ADD_FAILURE() << "i=" << i << " got " << result[i];
            ++mismatches;
        }
    EXPECT_EQ(mismatches, 0u);
}

// bfloat16 is portable via the storage+f32-compute model: SPV_KHR_bfloat16 gives
// the type+conversions; the device computes in f32 (no native bfloat arithmetic,
// which is Intel-only) — so it runs on RADV too.

// half/bfloat16 vector arithmetic on AMD via HIP.
static void runHalfOnAmd(const char* src, const char* cls, const char* kern) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no AMD HIP device available";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()[std::string("test.") + cls], kern);
    ASSERT_NE(k, nullptr);
    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_half_amd", deviceCtx);
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
    HipFunction fn = hip.getFunction(mod, kern);
    ASSERT_NE(fn, nullptr);
    const std::size_t bytes = std::size_t(n) * sizeof(float);
    HipDevicePtr dOut = hip.alloc(bytes);
    ASSERT_NE(dOut, nullptr);
    ASSERT_TRUE(hip.memcpyHtoD(dOut, out.data(), bytes));
    void* params[] = {&dOut, (void*) &n};
    ASSERT_TRUE(hip.launch(fn, (n + 63) / 64, 64, params));
    ASSERT_TRUE(hip.synchronize());
    std::vector<float> result(n);
    ASSERT_TRUE(hip.memcpyDtoH(result.data(), dOut, bytes));
    hip.free(dOut);
    for (uint32_t i = 0; i < n; ++i)
        EXPECT_NEAR(result[i], halfExpectedAt(i), 0.5f) << "element " << i;
}


// --- integer dot product (DP4a, SPV_KHR_integer_dot_product) --------------
// `Vector<int8,4>` / `Vector<uint8,4>` `.dot()` -> int32, with the optional
// int32 accumulator (fused dot-add). Signed and unsigned in one kernel:
//   a=(1,2,3,4) b=(5,6,7,8): a.dot(b)      = 5+12+21+32      =  70 (signed)
//                            a.dot(b, 100) = 100 + 70        = 170 (fused)
//   u=(200,100,50,25) w=(2,3,4,5): u.dot(w) = 400+300+200+125 = 1025 (UNsigned:
//     the signed reading of 200/100 would be negative — proves OpUDot vs OpSDot)
//   out[i] = 70 + 170 + 1025 = 1265, for every i.
// On Vulkan this lowers to OpSDot/OpUDot PackedVectorFormat4x8Bit (the hardware
// dot-product unit); on CPU/AMD to the portable widening reduce. Both bit-exact.
const char* kDp4aSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class DP {\n"
    "    @Kernel\n"
    "    public static void dp4a(KernelBuffer<int32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,4> a = heap Vector<int8,4>(1, 2, 3, 4);\n"
    "            Vector<int8,4> b = heap Vector<int8,4>(5, 6, 7, 8);\n"
    "            int32 d = a.dot(b);\n"
    "            int32 e = a.dot(b, 100);\n"
    "            Vector<uint8,4> u = heap Vector<uint8,4>(200, 100, 50, 25);\n"
    "            Vector<uint8,4> w = heap Vector<uint8,4>(2, 3, 4, 5);\n"
    "            int32 f = u.dot(w);\n"
    "            out[i] = d + e + f;\n"
    "        }\n"
    "    }\n"
    "}\n";

const int32_t kDp4aExpected = 1265;

using IDotFn = void (*)(int32_t*, uint32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t);

TEST(XpuVectorDeviceTests, integerDotRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kDp4aSource);
    auto k = findMethod(module->getStructures()["test.DP"], "dp4a");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_dp4a_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("dp4a");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto dp4a = symOrErr->toPtr<IDotFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<int32_t> out(N, -1);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            dp4a(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_EQ(out[i], kDp4aExpected) << "element " << i;
}


