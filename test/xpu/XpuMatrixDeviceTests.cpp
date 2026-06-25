//
// B1 — Matrix<T,R,C> device codegen (S6), via the CPU lowering path.
//
// The SAME flat row-major `<R*C x T>` representation the host uses lowers in the
// @Kernel walk: construction (insertelement chain), m[r][c] read/write (flat
// lane r*C+c), element-wise + - / (flat vector arithmetic), and `*` = matrix
// multiply / matrix-vector. The device walker has no resolved types, so a matrix
// local's (R,C) shape is tracked by name (matrixShapes) — that is how m[r][c]
// and matmul recover the dimensions a Vector<R*C> slot type can't carry.
//
// Host counterpart: test/expression/MatrixTests.cpp. VK/AMD device EXECUTION of
// matrices is a follow-on (hardware-gated, like XpuVectorDeviceTests); these run
// the very same ops on the CPU oracle with numeric assertions.
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

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_matdev_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_matdev_arch_" + std::to_string(rng()));
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
using MatFn = void (*)(float*, uint32_t,
                       int32_t, int32_t, int32_t,
                       int32_t, int32_t, int32_t,
                       int32_t, int32_t, int32_t,
                       int32_t, int32_t, int32_t);

// A kernel exercising construct + matmul + element-wise add + m[r][c] write.
//   a = [1 2; 3 4]  b = [5 6; 7 8]
//   c = a * b = [19 22; 43 50]   (matmul, not element-wise)
//   s = a + b = [6 8; 10 12]; then s[0][0] = 100
//   out[i] = c[0][0] + c[1][1] + s[0][0] + i = 19 + 50 + 100 + i = 169 + i
const char* kMatSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void matk(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f,2.0f,3.0f,4.0f);\n"
    "            Matrix<float32,2,2> b = stack Matrix<float32,2,2>(5.0f,6.0f,7.0f,8.0f);\n"
    "            Matrix<float32,2,2> c = a * b;\n"
    "            Matrix<float32,2,2> s = a + b;\n"
    "            s[0][0] = 100.0f;\n"
    "            out[i] = c[0][0] + c[1][1] + s[0][0] + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float expectedAt(uint32_t i) { return 169.0f + (float) i; }

// determinant + inverse on the device. a = [4 7; 2 6]:
//   d = det(a) = 4*6 - 7*2 = 10 ; inv = a^-1 ; p = a*inv = I
//   out[i] = d + p[0][0] + p[1][1] + i = 10 + 1 + 1 + i = 12 + i
const char* kDetInvSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void detinv(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 idx = KernelThread.globalIdX();\n"
    "        if (idx < n) {\n"
    "            Matrix<float32,2,2> a = stack Matrix<float32,2,2>(4.0f, 7.0f, 2.0f, 6.0f);\n"
    "            float32 d = a.determinant();\n"
    "            Matrix<float32,2,2> inv = a.inverse();\n"
    "            Matrix<float32,2,2> p = a * inv;\n"
    "            out[idx] = d + p[0][0] + p[1][1] + (float32) idx;\n"
    "        }\n"
    "    }\n"
    "}\n";

float detInvExpectedAt(uint32_t i) { return 12.0f + (float) i; }

// A kernel exercising the matrix methods on the device path: transpose,
// identity, row, col, hadamard. All lower to flat-vector IR via the shared
// `matops` helpers, identical to the host.
//   a = [1 2 3; 4 5 6]                 (2x3)
//   t = a.transpose() = [1 4; 2 5; 3 6]  -> t[1][0] = 2
//   e = id.identity() = [1 0; 0 1]       -> e[0][0] = 1
//   r0 = a.row(0) = [1 2 3]              -> r0[2] = 3
//   c1 = a.col(1) = [2 5]               -> c1[1] = 5
//   h = a.hadamard(a) = [1 4 9; 16 25 36] -> h[1][2] = 36
//   out[i] = 2 + 1 + 3 + 5 + 36 + i = 47 + i
const char* kMatMethodSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void methk(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
    "            Matrix<float32,3,2> t = a.transpose();\n"
    "            Matrix<float32,2,2> id = stack Matrix<float32,2,2>(9.0f,9.0f,9.0f,9.0f);\n"
    "            Matrix<float32,2,2> e = id.identity();\n"
    "            Vector<float32,3> r0 = a.row(0);\n"
    "            Vector<float32,2> c1 = a.col(1);\n"
    "            Matrix<float32,2,3> h = a.hadamard(a);\n"
    "            out[i] = t[1][0] + e[0][0] + r0[2] + c1[1] + h[1][2] + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float methodExpectedAt(uint32_t i) { return 47.0f + (float) i; }

// A Matrix<T,R,C> passed BY VALUE as a kernel param (B1 follow-on). The flat
// <R*C x T> aggregate marshals like a POD struct; the body reads a[r][c].
//   a = [10 20; 30 40]   out[i] = a[0][0] + a[1][1] + i = 10 + 40 + i = 50 + i
const char* kMatParamSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void mpk(Matrix<float32,2,2> a, KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            out[i] = a[0][0] + a[1][1] + (float32) i;\n"
    "        }\n"
    "    }\n"
    "}\n";

float paramExpectedAt(uint32_t i) { return 50.0f + (float) i; }

// Matches the LLVM `<4 x float>` by-value argument ABI (XMM on x86-64 SysV), so
// the 2x2 matrix param can be supplied directly to the JIT'd CPU kernel.
typedef float v4f __attribute__((vector_size(16)));

// CPU kernel signature for mpk: (a:<4 x float> by value, out, n, 12 coords).
using MatParamFn = void (*)(v4f, float*, uint32_t,
                            int32_t, int32_t, int32_t,
                            int32_t, int32_t, int32_t,
                            int32_t, int32_t, int32_t,
                            int32_t, int32_t, int32_t);

} // namespace

// The matrix ops lower to flat LLVM vector IR: a `<4 x float>` value, plus
// insertelement (construction + m[r][c] write) and extractelement (m[r][c] read
// + the matmul lane gathers).
TEST(XpuMatrixDeviceTests, lowersToMatrixIr) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatSource);
    auto k = findMethod(module->getStructures()["test.M"], "matk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_mat_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    EXPECT_NE(ir.find("<4 x float>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("insertelement"), std::string::npos) << ir;
    EXPECT_NE(ir.find("extractelement"), std::string::npos) << ir;
}

// CPU oracle: JIT the lowered kernel and run it over a grid; every element must
// equal 169 + i — proving matmul (not element-wise) for `*`, element-wise `+`,
// and m[r][c] read/write all lower correctly on the device path.
TEST(XpuMatrixDeviceTests, runsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatSource);
    auto k = findMethod(module->getStructures()["test.M"], "matk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_mat_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("matk");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto matk = symOrErr->toPtr<MatFn>();

    const int32_t B = 64, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            matk(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], expectedAt(i)) << "element " << i;
}

// Matrix * Vector on the device path: A(2x3) * v(3) -> Vector<2>, read back.
//   A = [1 2 3; 4 5 6]  v = [1 2 3]  Av = [14 32]; out[i] = 14 + 32 + i = 46 + i
TEST(XpuMatrixDeviceTests, matrixVectorRunsOnCpu) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void mvk(KernelBuffer<float32> out, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "            Vector<float32,3> v = stack Vector<float32,3>(1.0f,2.0f,3.0f);\n"
        "            Vector<float32,2> r = a * v;\n"
        "            out[i] = r[0] + r[1] + (float32) i;\n"
        "        }\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "mvk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_matvec_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("mvk");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto mvk = sym->toPtr<MatFn>();

    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            mvk(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], 46.0f + (float) i) << "element " << i;
}

// On a real GPU via Vulkan compute. The SAME matrix kernel (construct + matmul
// + element-wise add + m[r][c] write) lowers through the shared DeviceLowerer
// — the matrix ops are pure flat-vector IR with no backend seam — and runs on
// the device. out[i] must equal 169 + i. Skips cleanly when no device.
TEST(XpuMatrixDeviceTests, runsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatSource);
    auto k = findMethod(module->getStructures()["test.M"], "matk");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_mat_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "matk",
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

// On a real GPU via AMD HIP. Same matrix kernel, hsaco path. Skips cleanly when
// no device / assembler present.
TEST(XpuMatrixDeviceTests, runsOnAmdDevice) {
    using namespace cajeta::xpu::amd;
    if (!HipDriver::available()) GTEST_SKIP() << "no HIP device";

    Compiler compiler;
    auto module = compileForInspection(compiler, kMatSource);
    auto k = findMethod(module->getStructures()["test.M"], "matk");
    ASSERT_NE(k, nullptr);

    auto tm = createAmdgpuTargetMachine("gfx1151");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_mat_amddevice", deviceCtx);
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
    HipFunction fn = hip.getFunction(mod, "matk");
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

// CPU oracle for the device matrix methods (transpose/identity/row/col/
// hadamard): JIT the kernel, run it over a grid, every element must equal
// 47 + i. Proves the methods lower correctly on the device path.
TEST(XpuMatrixDeviceTests, methodsRunOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatMethodSource);
    auto k = findMethod(module->getStructures()["test.M"], "methk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_matmeth_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("methk");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto methk = sym->toPtr<MatFn>();

    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            methk(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], methodExpectedAt(i)) << "element " << i;
}

// The matrix methods on a real GPU via Vulkan compute. out[i] must equal 47 + i.
TEST(XpuMatrixDeviceTests, methodsRunOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatMethodSource);
    auto k = findMethod(module->getStructures()["test.M"], "methk");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_matmeth_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "methk",
                          {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != methodExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << methodExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// A Matrix<T,R,C> by-value kernel PARAM lowers: the body reads a[r][c] off the
// flat <R*C x T> param slot (the marshalling admitted it). Successful lowering
// + extract/insertelement IR is the proof (mirrors the @ValueType-arg emit
// tests). The param is a `<4 x float>` kernel argument.
TEST(XpuMatrixDeviceTests, matrixParamLowers) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatParamSource);
    auto k = findMethod(module->getStructures()["test.M"], "mpk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_matparam_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    EXPECT_NE(ir.find("<4 x float>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("extractelement"), std::string::npos) << ir;
}

// CPU oracle: a 2x2 matrix supplied BY VALUE to the JIT'd kernel (the v4f ABI
// matches the <4 x float> param). Every element must equal 50 + i.
TEST(XpuMatrixDeviceTests, matrixParamRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatParamSource);
    auto k = findMethod(module->getStructures()["test.M"], "mpk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_matparam_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("mpk");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto mpk = sym->toPtr<MatParamFn>();

    v4f a = {10.0f, 20.0f, 30.0f, 40.0f};  // [10 20; 30 40] row-major
    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            mpk(a, out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], paramExpectedAt(i)) << "element " << i;
}

// A by-value matrix param on a real GPU via Vulkan: the 2x2 (vec4-conformant)
// matrix is bound as a single-element read-only SSBO at binding 0, just like a
// scalar param. out[i] must equal 50 + i.
TEST(XpuMatrixDeviceTests, matrixParamRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kMatParamSource);
    auto k = findMethod(module->getStructures()["test.M"], "mpk");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_matparam_vkdevice", deviceCtx);
    configureDeviceModule(deviceModule, *tm);
    lowerKernel(k, deviceModule);
    std::vector<uint8_t> spirv = emitSpirv(deviceModule, *tm);
    ASSERT_FALSE(spirv.empty()) << "SPIR-V emission failed";

    const uint32_t n = 1u << 16;
    std::vector<float> out(n, -1.0f);
    const float matrix[4] = {10.0f, 20.0f, 30.0f, 40.0f};

    VulkanDriver vk;
    ASSERT_TRUE(vk.init());
    const std::size_t bytes = std::size_t(n) * sizeof(float);
    VulkanDriver::Buffer dA = vk.alloc(sizeof(matrix));
    VulkanDriver::Buffer dOut = vk.alloc(bytes);
    VulkanDriver::Buffer dN = vk.alloc(sizeof(uint32_t));
    ASSERT_NE(dA, 0u);
    ASSERT_NE(dOut, 0u);
    ASSERT_NE(dN, 0u);
    ASSERT_TRUE(vk.upload(dA, matrix, sizeof(matrix)));
    ASSERT_TRUE(vk.upload(dOut, out.data(), bytes));
    ASSERT_TRUE(vk.upload(dN, &n, sizeof(uint32_t)));

    const unsigned block = kVulkanLocalSizeX;
    const unsigned grid = (n + block - 1) / block;
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "mpk",
                          {dA, dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dA);
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != paramExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << paramExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}

// determinant + inverse on the CPU oracle. out[i] == 12 + i.
TEST(XpuMatrixDeviceTests, detInverseRunsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kDetInvSource);
    auto k = findMethod(module->getStructures()["test.M"], "detinv");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_detinv_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr)) << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto sym = jit->lookup("detinv");
    ASSERT_TRUE(static_cast<bool>(sym)) << llvm::toString(sym.takeError());
    auto detinv = sym->toPtr<MatFn>();

    const int32_t B = 32, G = 2;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<float> out(N, -1.0f);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            detinv(out.data(), N, tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (uint32_t i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(out[i], detInvExpectedAt(i)) << "element " << i;
}

// determinant + inverse on a real GPU via Vulkan. out[i] == 12 + i.
TEST(XpuMatrixDeviceTests, detInverseRunsOnVulkanDevice) {
    using namespace cajeta::xpu::vulkan;
    if (!VulkanDriver::available()) {
        GTEST_SKIP() << "no Vulkan compute device available";
    }
    Compiler compiler;
    auto module = compileForInspection(compiler, kDetInvSource);
    auto k = findMethod(module->getStructures()["test.M"], "detinv");
    ASSERT_NE(k, nullptr);

    auto tm = createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);
    llvm::LLVMContext deviceCtx;
    llvm::Module deviceModule("xpu_detinv_vkdevice", deviceCtx);
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
    ASSERT_TRUE(vk.launch(spirv.data(), spirv.size(), "detinv", {dOut, dN}, grid));

    std::vector<float> result(n);
    ASSERT_TRUE(vk.download(result.data(), dOut, bytes));
    vk.free(dOut);
    vk.free(dN);

    size_t mismatches = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (result[i] != detInvExpectedAt(i)) {
            if (mismatches < 5)
                ADD_FAILURE() << "i=" << i << " got " << result[i]
                              << " expected " << detInvExpectedAt(i);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
}
