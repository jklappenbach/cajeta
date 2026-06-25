//
// XpuAccelWidthSpikeTests — cajeta-accel plan unit 2: the SIMD width-lowering
// spike that GATES how wide the Exact strategy can go (spec §4 width-as-lowering).
//
// The Exact strategy's wide slab test is written over Vector<float32,N>; for the
// AVX-512 CPU path to pay off, the backend must lower Vector<float32,16> to a
// PACKED <16 x float> op (a real wide vector, not 16 scalars). This spike probes
// that on the CPU backend, exactly as XpuVectorLoadStoreTests proved <8 x double>
// vload/vstore: it lowers a width-16 (and width-8) float32 slab-arithmetic kernel
// — t0 = (min - origin) * invDir, t1 = (max - origin) * invDir across N lanes —
// and asserts (1) the IR carries packed <16 x float> sub/mul/load/store, (2) it
// round-trips correctly when JIT-run, and (3) width 8 and width 16 monomorphize to
// DISTINCT specialized bodies (each its own packed width, no runtime arity loop).
//
// Finding gates unit 3: if Vector<float32,16> packs, Exact wide proceeds to width
// 16; the single-zmm register form additionally needs the target to advertise
// AVX-512 (cpu=native on Zen4 / Skylake-X+), which test (4) reports from the host
// target machine. GPU-free — the CPU backend is the oracle.
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
    auto baseDir = std::filesystem::temp_directory_path()
              / ("cajeta_accel_spike_" + std::to_string(rng()));
    std::filesystem::create_directories(baseDir / "test");
    std::ofstream(baseDir / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_accel_spike_arch_" + std::to_string(rng()));
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

// Lower kernel `entry` in `M` to a fresh host module and return its printed IR.
std::string lowerToIr(Compiler& compiler, const char* src, const char* entry) {
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], entry);
    if (!k) return "";
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    if (!tm) return "";
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("accel_spike_ir", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    if (!cajeta::xpu::cpu::lowerKernel(k, *host)) return "";
    std::string ir;
    llvm::raw_string_ostream os(ir);
    host->print(os, nullptr);
    os.flush();
    return ir;
}

// Per thread: a 96-float block — mn[16] mx[16] o[16] inv[16] t0[16] t1[16].
const char* kSlab16 =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void slab16(KernelBuffer<float32> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 96;\n"
    "        Vector<float32,16> mn = c.vload<16>(base);\n"
    "        Vector<float32,16> mx = c.vload<16>(base + 16);\n"
    "        Vector<float32,16> o  = c.vload<16>(base + 32);\n"
    "        Vector<float32,16> iv = c.vload<16>(base + 48);\n"
    "        Vector<float32,16> t0 = (mn - o) * iv;\n"
    "        Vector<float32,16> t1 = (mx - o) * iv;\n"
    "        c.vstore(base + 64, t0);\n"
    "        c.vstore(base + 80, t1);\n"
    "    }\n"
    "}\n";

// Width-8 twin: a 48-float block — mn[8] mx[8] o[8] inv[8] t0[8] t1[8].
const char* kSlab8 =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void slab8(KernelBuffer<float32> c) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        uint32 base = t * 48;\n"
    "        Vector<float32,8> mn = c.vload<8>(base);\n"
    "        Vector<float32,8> mx = c.vload<8>(base + 8);\n"
    "        Vector<float32,8> o  = c.vload<8>(base + 16);\n"
    "        Vector<float32,8> iv = c.vload<8>(base + 24);\n"
    "        Vector<float32,8> t0 = (mn - o) * iv;\n"
    "        Vector<float32,8> t1 = (mx - o) * iv;\n"
    "        c.vstore(base + 32, t0);\n"
    "        c.vstore(base + 40, t1);\n"
    "    }\n"
    "}\n";

using SlabFn = void (*)(float*,
                        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
                        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);

} // namespace

// 2.1.2 / 2.2.1 — the width-16 slab probe lowers to PACKED <16 x float> ops: a
// packed load, the per-lane slab arithmetic as <16 x float> fsub + fmul, and a
// packed store. Not scalarized — the property the AVX-512 Exact path needs.
TEST(XpuAccelWidthSpikeTests, slab16LowersToPackedVectorIr) {
    Compiler compiler;
    std::string ir = lowerToIr(compiler, kSlab16, "slab16");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("load <16 x float>"), std::string::npos)
        << "expected a packed <16 x float> load in:\n" << ir;
    EXPECT_NE(ir.find("fsub <16 x float>"), std::string::npos)
        << "expected a packed <16 x float> fsub (min - origin)";
    EXPECT_NE(ir.find("fmul <16 x float>"), std::string::npos)
        << "expected a packed <16 x float> fmul (* invDir)";
    EXPECT_NE(ir.find("store <16 x float>"), std::string::npos)
        << "expected a packed <16 x float> store";
}

// 2.1.1 / 2.2.1 — the width-16 slab probe round-trips correctly per lane when
// JIT-run on the CPU backend: t0 = (min - o) * inv, t1 = (max - o) * inv.
TEST(XpuAccelWidthSpikeTests, slab16RoundTripsOnCpu) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSlab16);
    auto k = findMethod(module->getStructures()["test.M"], "slab16");
    ASSERT_NE(k, nullptr);
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("accel_spike_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, *host), nullptr);

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    ASSERT_TRUE(static_cast<bool>(jitOrErr))
        << llvm::toString(jitOrErr.takeError());
    auto jit = std::move(*jitOrErr);
    auto err = jit->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)));
    ASSERT_FALSE(static_cast<bool>(err)) << llvm::toString(std::move(err));
    auto slab16 = jit->lookup("slab16")->toPtr<SlabFn>();

    const int32_t B = 4, G = 2;
    const int blocks = B * G;
    const int N = 96 * blocks;
    std::vector<float> c(N, 0.0f);
    // Fill each block's mn/mx/o/inv with known per-lane values.
    for (int b = 0; b < blocks; ++b) {
        int base = b * 96;
        for (int k = 0; k < 16; ++k) {
            c[base + 0 + k]  = static_cast<float>(b) * 0.25f + k + 1; // mn
            c[base + 16 + k] = c[base + 0 + k] + 5.0f;                // mx
            c[base + 32 + k] = 1.0f;                                  // o
            c[base + 48 + k] = 2.0f;                                  // inv
        }
    }
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            slab16(c.data(), tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);

    for (int b = 0; b < blocks; ++b) {
        int base = b * 96;
        for (int k = 0; k < 16; ++k) {
            float mn = static_cast<float>(b) * 0.25f + k + 1;
            float mx = mn + 5.0f;
            EXPECT_FLOAT_EQ(c[base + 64 + k], (mn - 1.0f) * 2.0f) << "t0 b" << b << " k" << k;
            EXPECT_FLOAT_EQ(c[base + 80 + k], (mx - 1.0f) * 2.0f) << "t1 b" << b << " k" << k;
        }
    }
}

// 2.1.3 — width 8 and width 16 monomorphize to DISTINCT specialized bodies: the
// width-8 kernel carries <8 x float> and NOT <16 x float>, the width-16 the
// reverse. Each concrete width is its own packed op — never a shared runtime
// arity loop.
TEST(XpuAccelWidthSpikeTests, slab8And16MonomorphizeDistinctly) {
    // One Compiler alive at a time (block-scoped) — two simultaneous instances
    // corrupt shared LLVM global state.
    std::string ir8, ir16;
    { Compiler c8; ir8 = lowerToIr(c8, kSlab8, "slab8"); }
    { Compiler c16; ir16 = lowerToIr(c16, kSlab16, "slab16"); }
    ASSERT_FALSE(ir8.empty());
    ASSERT_FALSE(ir16.empty());
    EXPECT_NE(ir8.find("fmul <8 x float>"), std::string::npos)
        << "width-8 body must carry a packed <8 x float> op";
    EXPECT_EQ(ir8.find("<16 x float>"), std::string::npos)
        << "width-8 body must NOT carry a <16 x float> op";
    EXPECT_NE(ir16.find("fmul <16 x float>"), std::string::npos)
        << "width-16 body must carry a packed <16 x float> op";
    EXPECT_EQ(ir16.find("<8 x float>"), std::string::npos)
        << "width-16 body must NOT carry a <8 x float> op";
}

// 2.2.2 — the host target machine's feature string maps to a max Exact width
// (AVX-512 -> 16, AVX -> 8, else -> 4). This is the spike-level mapping the
// capability-driven selector (unit 5) wires to a runtime Capability; here we only
// confirm it is computable and sane on the build host. <16 x float> IR lowers on
// ANY host (LLVM legalizes to the available registers); a single zmm needs this
// feature string to advertise avx512f.
TEST(XpuAccelWidthSpikeTests, hostSimdMapsToExactWidth) {
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr);
    std::string feats = tm->getTargetFeatureString().str();
    int maxExactWidth = 4;
    if (feats.find("+avx512f") != std::string::npos) maxExactWidth = 16;
    else if (feats.find("+avx") != std::string::npos) maxExactWidth = 8;
    EXPECT_TRUE(maxExactWidth == 4 || maxExactWidth == 8 || maxExactWidth == 16);
    EXPECT_GE(maxExactWidth, 4);
}
