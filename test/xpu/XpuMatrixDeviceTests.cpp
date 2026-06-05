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
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void matk(Buffer<float32> out, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
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
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void mvk(Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
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
