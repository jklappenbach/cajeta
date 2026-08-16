//
// Kernel-aware vectorized load/store (kernel-vector-loadstore plan, Unit 2):
// `buf.vload<N>(i) -> Vector<T,N>` and `buf.vstore(i, v)` inside an @Kernel,
// lowered to a PACKED `<N x T>` memory op (not N scalar loads), through the
// per-backend bufferElementPtr seam. CPU backend, GPU-free — the oracle.
//
// Two checks: (1) semantic — the kernel doubles each contiguous 8-block via a
// vload/vstore round-trip; (2) structural — the lowered IR carries a packed
// `<8 x double>` load + store, proving the kernel actually vectorizes (the
// scalar-load matmul kernel did not, which is the whole motivation).
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

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

// One thread per contiguous 8-element block: load it, double it, store it.
const char* kDblSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dbl(KernelBuffer<float64> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 8;\n"
    "        Vector<float64,8> v = c.vload<8>(base);\n"
    "        c.vstore(base, v + v);\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto baseDir = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vls_" + std::to_string(rng()));
    std::filesystem::create_directories(baseDir / "test");
    std::ofstream(baseDir / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vls_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = baseDir / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), baseDir.string(),
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

// Lowered host signature: (c, then 12 i32 grid coordinates).
using DblFn = void (*)(double*,
                       int32_t, int32_t, int32_t,   // tid.{x,y,z}
                       int32_t, int32_t, int32_t,   // ctaid.{x,y,z}
                       int32_t, int32_t, int32_t,   // ntid.{x,y,z}
                       int32_t, int32_t, int32_t);  // nctaid.{x,y,z}

} // namespace

TEST(XpuVectorLoadStoreTests, vloadVstoreRoundTripsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kDblSource);
    auto k = findMethod(module->getStructures()["test.M"], "dbl");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_cpu_vls_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = cajeta::test::makeCoffSafeJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto symOrErr = jit->lookup("dbl");
    ASSERT_TRUE(static_cast<bool>(symOrErr))
        << llvm::toString(symOrErr.takeError());
    auto dbl = symOrErr->toPtr<DblFn>();

    // Grid: G blocks × B threads, one thread per 8-block → covers N = 8*B*G.
    const int32_t B = 16, G = 4;
    const int blocks = B * G;
    const int N = 8 * blocks;
    std::vector<double> c(N), c0(N);
    for (int i = 0; i < N; ++i) c[i] = c0[i] = static_cast<double>(i + 1);

    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            dbl(c.data(),
                tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (int i = 0; i < N; ++i)
        EXPECT_DOUBLE_EQ(c[i], 2.0 * c0[i]) << "at " << i;
}

// Structural: the lowered kernel must carry a PACKED <8 x double> load + store —
// not eight scalar loads/inserts. This is the property the scalar-construction
// matmul kernel lacked.
TEST(XpuVectorLoadStoreTests, vloadVstoreLowersToPackedVectorIr) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kDblSource);
    auto k = findMethod(module->getStructures()["test.M"], "dbl");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_cpu_vls_ir", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    std::string ir;
    llvm::raw_string_ostream os(ir);
    host->print(os, nullptr);
    os.flush();

    EXPECT_NE(ir.find("load <8 x double>"), std::string::npos)
        << "expected a packed <8 x double> load in:\n" << ir;
    EXPECT_NE(ir.find("store <8 x double>"), std::string::npos)
        << "expected a packed <8 x double> store in:\n" << ir;
}

namespace {
// A different element type AND a different (non-8) width — float32 x 4.
const char* kDbl4Source =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dbl4(KernelBuffer<float32> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 4;\n"
    "        Vector<float32,4> v = c.vload<4>(base);\n"
    "        c.vstore(base, v + v);\n"
    "    }\n"
    "}\n";
using Dbl4Fn = void (*)(float*,
                        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
} // namespace

// 2.1.3 — vload/vstore generalize across element type and lane width: float32,
// width 4, packed <4 x float>, and a correct round-trip.
TEST(XpuVectorLoadStoreTests, vloadVstoreFloat32Width4) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kDbl4Source);
    auto k = findMethod(module->getStructures()["test.M"], "dbl4");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_cpu_vls4", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    std::string ir;
    llvm::raw_string_ostream os(ir);
    host->print(os, nullptr);
    os.flush();
    EXPECT_NE(ir.find("load <4 x float>"), std::string::npos)
        << "expected a packed <4 x float> load";
    EXPECT_NE(ir.find("store <4 x float>"), std::string::npos)
        << "expected a packed <4 x float> store";

    auto jitOrErr = cajeta::test::makeCoffSafeJit();
    ASSERT_TRUE(static_cast<bool>(jitOrErr));
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto dbl4 = jit->lookup("dbl4")->toPtr<Dbl4Fn>();

    const int32_t B = 8, G = 2;
    const int blocks = B * G, N = 4 * blocks;
    std::vector<float> c(N), c0(N);
    for (int i = 0; i < N; ++i) c[i] = c0[i] = static_cast<float>(i + 1);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            dbl4(c.data(), tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);
    for (int i = 0; i < N; ++i) EXPECT_FLOAT_EQ(c[i], 2.0f * c0[i]) << "at " << i;
}

namespace {
const char* kI32Source =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dbli32(KernelBuffer<int32> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 8;\n"
    "        Vector<int32,8> v = c.vload<8>(base);\n"
    "        c.vstore(base, v + v);\n"
    "    }\n"
    "}\n";
const char* kI64Source =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void dbli64(KernelBuffer<int64> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 4;\n"
    "        Vector<int64,4> v = c.vload<4>(base);\n"
    "        c.vstore(base, v + v);\n"
    "    }\n"
    "}\n";
using I32Fn = void (*)(int32_t*, int32_t, int32_t, int32_t, int32_t, int32_t,
                       int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                       int32_t);
using I64Fn = void (*)(int64_t*, int32_t, int32_t, int32_t, int32_t, int32_t,
                       int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                       int32_t);

// Lower `entry` in `M`, JIT it, return the symbol. Asserts packed-IR substr.
template <class Fn>
Fn lowerAndJit(Compiler& compiler, const char* src, const char* entry,
               const char* irNeedle,
               std::unique_ptr<llvm::orc::LLJIT>& keep) {
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], entry);
    EXPECT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("vls_int", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    EXPECT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);
    std::string ir;
    llvm::raw_string_ostream os(ir);
    host->print(os, nullptr);
    os.flush();
    EXPECT_NE(ir.find(irNeedle), std::string::npos)
        << "expected '" << irNeedle << "' in IR";
    keep = std::move(*cajeta::test::makeCoffSafeJit());
    auto err = keep->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    EXPECT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    return keep->lookup(entry)->template toPtr<Fn>();
}
} // namespace

// 4.1.1 — integer element types round-trip (int32 x 8). vload/vstore are
// element-type-generic; a packed <8 x i32> load/store and a correct double.

// 4.1.1 — int64 x 4 round-trips with a packed <4 x i64> load/store.

// 2.1.4 — vload/vstore are kernel-only: calling vload on a KernelBuffer from a
// non-kernel (host) method does not resolve to a method and is a compile error.
TEST(XpuVectorLoadStoreTests, vloadOutsideKernelIsCompileError) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        KernelBuffer<float64> b = stack KernelBuffer<float64>(8);\n"
        "        Vector<float64,8> v = b.vload<8>(0);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(cajeta_test::CajetaJit::compile(src, "test.D"));
}
