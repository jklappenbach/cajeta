//
// Byte-vector ops in a kernel lower in WORD form (kernel-byte-vector-lowering
// plan, Units 1 and 2).
//
// `Vector<int8,N>` is the working type of every quantized kernel, and LLVM's
// amdgpu backend legalizes a `<16 x i8>` and/or/shift one byte at a time —
// sixteen `v_and_b16` for one `and`, and a runtime-lane `hv[j]` as a
// fifteen-deep compare/select chain per byte. cajeta-llm unit 60 measured
// the Q4_K mat-vec ALU-bound on exactly this (30 us from cache = from DRAM)
// and halved eight kernels' instruction counts by hand-rewriting them on
// `asWords()`. These tests pin the same rewrite in the shared lowering:
//
//   - bitwise and/or/xor on `<N x i8>` emit on `<N/4 x i32>`;
//   - `<< c` and unsigned `>> c` by a constant emit a word shift + a
//     replicated byte mask;
//   - the signed `(v >> c) & k` shape, where k discards every sign-extended
//     bit, emits the logical word form (no `ashr <N x i8>` at all);
//   - a bare signed `v >> c` STAYS `ashr <N x i8>` (a word-form sign
//     extension costs more than it saves) — the check that the peephole does
//     not fire;
//   - `v[j]` with a runtime j extracts word j>>2 and shifts.
//
// Two kinds of assertion, both needed: the IR SHAPE (what was emitted) and a
// CPU-JIT oracle against a byte-at-a-time host reference (that the bytes are
// identical, negatives included). A shape test alone would pass a rewrite
// that computed the wrong bytes; an oracle alone would pass no rewrite.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "XpuCpuJit.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

// Six regions of n*16 bytes in `out`, one per shape under test:
//   0: the Q6_K assembly  ((ql >> 4) & 15) | (((qh >> 2) & 3) << 4)  (signed ql/qh)
//   1: ql ^ qh
//   2: ql << 3
//   3: ql.asUnsigned() >> 3
//   4: ql >> 3            (bare arithmetic shift: stays per-byte, must sign-extend)
//   5: (ql & 15) | (qh & 240)
//   6: ((ql >> 4) & 15) - 8      (the q4_0 dequant: a byte SUB after the peephole)
const char* kByteOpsSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void byteops(KernelBuffer<int8> out, KernelBuffer<int8> a,\n"
    "            KernelBuffer<int8> b, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 o = (int64) i * 16L;\n"
    "            int64 span = (int64) n * 16L;\n"
    "            Vector<int8,16> ql = a.vload<16>(o);\n"
    "            Vector<int8,16> qh = b.vload<16>(o);\n"
    "            Vector<int8,16> q = ((ql >> 4) & 15) | (((qh >> 2) & 3) << 4);\n"
    "            out.vstore(o, q);\n"
    "            Vector<int8,16> x = ql ^ qh;\n"
    "            out.vstore(span + o, x);\n"
    "            Vector<int8,16> s = ql << 3;\n"
    "            out.vstore(2L * span + o, s);\n"
    "            Vector<uint8,16> u = ql.asUnsigned() >> 3;\n"
    "            out.vstore(3L * span + o, u.asSigned());\n"
    "            Vector<int8,16> r = ql >> 3;\n"
    "            out.vstore(4L * span + o, r);\n"
    "            Vector<int8,16> m = (ql & 15) | (qh & 240);\n"
    "            out.vstore(5L * span + o, m);\n"
    "            Vector<int8,16> e = ((ql >> 4) & 15) - 8;\n"
    "            out.vstore(6L * span + o, e);\n"
    "        }\n"
    "    }\n"
    "}\n";

// Region 0: v[j] for runtime j = i % 16 of a signed byte vector;
// region 1: u[j % 8] of an 8-lane unsigned one; region 2: constant v[3];
// out16[i]: w[j % 8] of an int16 x 8.
const char* kLaneSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void lanes(KernelBuffer<int8> out, KernelBuffer<int16> out16,\n"
    "            KernelBuffer<int8> a, KernelBuffer<int16> h, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            Vector<int8,16> v = a.vload<16>(0L);\n"
    "            Vector<uint8,8> u = a.vload<8>(16L).asUnsigned();\n"
    "            Vector<int16,8> w = h.vload<8>(0L);\n"
    "            int32 j = (int32) (i % 16);\n"
    "            int32 k = j % 8;\n"
    "            out[(int64) i] = v[j];\n"
    "            out[(int64) n + (int64) i] = (int8) u[k];\n"
    "            out[2L * (int64) n + (int64) i] = v[3];\n"
    "            out16[(int64) i] = w[k];\n"
    "        }\n"
    "    }\n"
    "}\n";

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_bytevec_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_bytevec_arch_" + std::to_string(rng()));
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

// Lower `kernel` from `source` for the CPU backend and return its IR text.
std::string cpuIr(const char* source, const char* kernel) {
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], kernel);
    if (!k) return "<no kernel " + std::string(kernel) + ">";
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    if (!tm) return "<no host target>";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_bytevec_emit", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    if (!cajeta::xpu::cpu::lowerKernel(k, host)) return "<lowering failed>";
    return printModule(host);
}

size_t countOf(const std::string& hay, const std::string& needle) {
    size_t c = 0, at = hay.find(needle);
    while (at != std::string::npos) { ++c; at = hay.find(needle, at + needle.size()); }
    return c;
}

// CPU lowering's host signature: buffers as raw pointers, scalars as-is,
// then the 12 i32 grid coordinates.
using ByteOpsFn = void (*)(int8_t*, int8_t*, int8_t*, uint32_t,
                           int32_t, int32_t, int32_t,
                           int32_t, int32_t, int32_t,
                           int32_t, int32_t, int32_t,
                           int32_t, int32_t, int32_t);
using LanesFn = void (*)(int8_t*, int16_t*, int8_t*, int16_t*, uint32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t,
                         int32_t, int32_t, int32_t);

template <typename Fn>
struct Jitted {
    std::unique_ptr<llvm::orc::LLJIT> jit;
    Fn fn = nullptr;
};

template <typename Fn>
Jitted<Fn> jitKernel(const char* source, const char* kernel) {
    Jitted<Fn> r;
    Compiler compiler;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], kernel);
    if (!k) return r;
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    if (!tm) return r;
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_bytevec_exec", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);
    if (!cajeta::xpu::cpu::lowerKernel(k, *host)) return r;
    auto jitOrErr = cajeta::xpu::test::makeCpuKernelJit();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return r; }
    r.jit = std::move(*jitOrErr);
    if (auto err = r.jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)))) {
        llvm::consumeError(std::move(err));
        return r;
    }
    auto symOrErr = r.jit->lookup(kernel);
    if (!symOrErr) { llvm::consumeError(symOrErr.takeError()); return r; }
    r.fn = symOrErr->template toPtr<Fn>();
    return r;
}

} // namespace

// 1.1.1 — the IR shape: every byte-vector bitwise op and constant shift is a
// word op; the masked signed shift never emits its ashr.
TEST(XpuByteVectorWordFormTests, lowersByteBitwiseOpsAsWords) {
    std::string ir = cpuIr(kByteOpsSource, "byteops");
    EXPECT_NE(ir.find("and <4 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("or <4 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("xor <4 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("shl <4 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("lshr <4 x i32>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("and <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("or <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("xor <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("shl <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("lshr <16 x i8>"), std::string::npos) << ir;
    // Region 4's bare `ql >> 3` is the ONLY ashr; the three masked shifts of
    // region 0 ((ql >> 4) & 15, (qh >> 2) & 3) emit none.
    EXPECT_EQ(countOf(ir, "ashr <16 x i8>"), 1u) << ir;
    // Region 6's `- 8` runs on the BYTES the peephole hands back, never on
    // the words (a use-after-free of the erased ashr once returned the
    // word type from toBytes, and the device subtracted 8 from each word).
    EXPECT_NE(ir.find("sub <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("sub <4 x i32>"), std::string::npos) << ir;
}

// 1.1.2 — a bare signed shift stays per-byte (spec 2.5): a kernel with ONLY
// `v >> 3` on a signed vector emits `ashr <16 x i8>` and no word shift.
TEST(XpuByteVectorWordFormTests, signedShiftWithoutMaskStaysAshr) {
    const char* src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public class M {\n"
        "    @Kernel\n"
        "    public static void bare(KernelBuffer<int8> out, KernelBuffer<int8> a, uint32 n) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < n) {\n"
        "            int64 o = (int64) i * 16L;\n"
        "            Vector<int8,16> v = a.vload<16>(o);\n"
        "            Vector<int8,16> r = v >> 3;\n"
        "            out.vstore(o, r);\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string ir = cpuIr(src, "bare");
    EXPECT_NE(ir.find("ashr <16 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("lshr <4 x i32>"), std::string::npos) << ir;
}

// 1.1.3 — the oracle: 4096 random byte vectors (negatives included), every
// region against a byte-at-a-time host reference.
TEST(XpuByteVectorWordFormTests, byteOpsAreBitIdentical) {
    auto j = jitKernel<ByteOpsFn>(kByteOpsSource, "byteops");
    ASSERT_NE(j.fn, nullptr) << "kernel did not lower/JIT";
    const int32_t B = 64, G = 64;
    const uint32_t N = (uint32_t) (B * G);
    const size_t span = (size_t) N * 16;
    std::vector<int8_t> a(span), b(span), out(7 * span, 0);
    std::mt19937 rng(60);
    for (auto& v : a) v = (int8_t) (rng() & 255);
    for (auto& v : b) v = (int8_t) (rng() & 255);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            j.fn(out.data(), a.data(), b.data(), N,
                 tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);
    size_t bad = 0;
    for (size_t p = 0; p < span && bad < 8; ++p) {
        int8_t ql = a[p], qh = b[p];
        int8_t e0 = (int8_t) (((ql >> 4) & 15) | (((qh >> 2) & 3) << 4));
        int8_t e1 = (int8_t) (ql ^ qh);
        int8_t e2 = (int8_t) (ql << 3);
        int8_t e3 = (int8_t) ((uint8_t) ql >> 3);
        int8_t e4 = (int8_t) (ql >> 3);
        int8_t e5 = (int8_t) ((ql & 15) | (qh & 240));
        int8_t e6 = (int8_t) (((ql >> 4) & 15) - 8);
        int8_t got[7] = {out[p], out[span + p], out[2 * span + p],
                         out[3 * span + p], out[4 * span + p], out[5 * span + p],
                         out[6 * span + p]};
        int8_t exp[7] = {e0, e1, e2, e3, e4, e5, e6};
        for (int r = 0; r < 7; ++r) {
            if (got[r] != exp[r]) {
                ++bad;
                ADD_FAILURE() << "region " << r << " byte " << p
                    << ": ql=" << (int) ql << " qh=" << (int) qh
                    << " got " << (int) got[r] << " want " << (int) exp[r];
            }
        }
    }
}

// 2.1.1 — a runtime-lane byte read is a word extract + shift; a constant
// lane stays a byte extract.
TEST(XpuByteVectorWordFormTests, runtimeLaneReadIsAWordExtract) {
    std::string ir = cpuIr(kLaneSource, "lanes");
    EXPECT_NE(ir.find("extractelement <4 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("extractelement <2 x i32>"), std::string::npos) << ir;
    EXPECT_NE(ir.find("trunc"), std::string::npos) << ir;
    // v[3] — the one constant-lane read — is the only <16 x i8> extract.
    EXPECT_EQ(countOf(ir, "extractelement <16 x i8>"), 1u) << ir;
    EXPECT_EQ(ir.find("extractelement <8 x i8>"), std::string::npos) << ir;
    EXPECT_EQ(ir.find("extractelement <8 x i16>"), std::string::npos) << ir;
}

// 2.1.2 — every lane of int8x16, uint8x8 and int16x8 read by thread index
// reads the byte the host sees there.
TEST(XpuByteVectorWordFormTests, runtimeLaneReadIsBitIdentical) {
    auto j = jitKernel<LanesFn>(kLaneSource, "lanes");
    ASSERT_NE(j.fn, nullptr) << "kernel did not lower/JIT";
    const int32_t B = 32, G = 4;
    const uint32_t N = (uint32_t) (B * G);
    std::vector<int8_t> a(24);
    std::vector<int16_t> h(8);
    std::mt19937 rng(61);
    for (auto& v : a) v = (int8_t) (rng() & 255);
    for (auto& v : h) v = (int16_t) (rng() & 65535);
    std::vector<int8_t> out(3 * N, 0);
    std::vector<int16_t> out16(N, 0);
    for (int32_t ctaid = 0; ctaid < G; ++ctaid)
        for (int32_t tid = 0; tid < B; ++tid)
            j.fn(out.data(), out16.data(), a.data(), h.data(), N,
                 tid, 0, 0, ctaid, 0, 0, B, 1, 1, G, 1, 1);
    for (uint32_t i = 0; i < N; ++i) {
        int jl = (int) (i % 16), k = jl % 8;
        EXPECT_EQ(out[i], a[jl]) << "v[" << jl << "] at " << i;
        EXPECT_EQ(out[N + i], a[16 + k]) << "u[" << k << "] at " << i;
        EXPECT_EQ(out[2 * N + i], a[3]) << "v[3] at " << i;
        EXPECT_EQ(out16[i], h[k]) << "w[" << k << "] at " << i;
    }
}
