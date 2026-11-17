//
// datacenter-gpus Unit 2 (specs/datacenter-gpus-spec.md §2.2): CooperativeMatrix on
// CDNA3's matrix cores. RDNA's WMMA does not exist on gfx942; its matrix cores are
// MFMA, on 64-lane waves, with a different fragment map:
//
//   v_mfma_f32_16x16x16_f16 / _bf16 (4 K-values a lane)
//     A  : lane L, element e holds A[L & 15][4 (L >> 4) + e]
//     B  : lane L, element e holds B[4 (L >> 4) + e][L & 15]
//     C/D: lane L, element e holds D[4 (L >> 4) + e][L & 15]
//   v_mfma_i32_16x16x32_i8 (8 K-values a lane): the int8 16x16x16 tile takes the
//     first half of K, so lanes 32-63 carry zeros.
//
// Compile-only: AMDGCN ISA emission needs no device. The values are checked on gem5's
// MI300 model (plan 2.3.2) and then on a rented MI300X (Unit 4).
//

#include <gtest/gtest.h>
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <set>
#include <string>
#include <utility>

using namespace cajeta::xpu;
using probe::lowerForAmdgpu;

namespace {

std::string program(const char* ab, const char* acc, const char* zero) {
    std::string s =
        "package test;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void mmRow(KernelBuffer<AB> a,\n"
        "            KernelBuffer<AB> b, KernelBuffer<ACC> c) {\n"
        "        CooperativeMatrix<AB,16,16,0> ma;\n"
        "        ma.load(a, 0, 0, 16);\n"
        "        CooperativeMatrix<AB,16,16,1> mb;\n"
        "        mb.load(b, 0, 0, 16);\n"
        "        CooperativeMatrix<ACC,16,16,2> mc;\n"
        "        mc.splat(ZERO);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, 16);\n"
        "    }\n"
        "    @Kernel\n"
        "    public static void mmCol(KernelBuffer<AB> a,\n"
        "            KernelBuffer<AB> b, KernelBuffer<ACC> c) {\n"
        "        CooperativeMatrix<AB,16,16,0> ma;\n"
        "        ma.load(a, 0, 1, 16);\n"
        "        CooperativeMatrix<AB,16,16,1> mb;\n"
        "        mb.load(b, 0, 1, 16);\n"
        "        CooperativeMatrix<ACC,16,16,2> mc;\n"
        "        mc.load(c, 0, 1, 16);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 1, 16);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto sub = [&](const std::string& from, const std::string& to) {
        for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
            s.replace(at, from.size(), to);
    };
    sub("ACC", acc);
    sub("AB", ab);
    sub("ZERO", zero);
    return s;
}

const std::string kF16 = program("float16", "float32", "0.0f");
const std::string kBf16 = program("bfloat16", "float32", "0.0f");
const std::string kI8 = program("int8", "int32", "0");
const std::string kF32 = program("float32", "float32", "0.0f");   // portable on both

void expectMfma(const std::string& src, const char* kernel, const char* mnemonic) {
    auto l = lowerForAmdgpu(src, kernel, "test.D", "gfx942", "mfma");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ptx.find(mnemonic), std::string::npos)
        << kernel << ": no " << mnemonic << " in the gfx942 ISA:\n" << l.ptx;
    EXPECT_EQ(l.ptx.find("v_wmma"), std::string::npos) << l.ptx;
    EXPECT_EQ(l.ptx.find("Cannot select"), std::string::npos) << l.ptx;
    EXPECT_EQ(l.ptx.find("amdhsa.kernels:  []"), std::string::npos)
        << kernel << " emitted no kernel for gfx942:\n" << l.ptx;
    EXPECT_NE(l.ptx.find(".private_segment_fixed_size: 0"), std::string::npos)
        << kernel << " carries a scratch frame on gfx942:\n" << l.ptx;
    EXPECT_EQ(l.ir.find("+wavefrontsize32"), std::string::npos)
        << "a gfx942 kernel must not be pinned to wave 32:\n" << l.ir;
}

// The device IR of `kernel` lowered once for a bundle of `arches`, as
// AmdgpuRegistration lowers a multi-arch build.
probe::Lowered lowerForBundle(const std::string& src, const char* kernel,
                              const std::string& arches) {
    probe::Lowered out;
    cajeta::Compiler compiler;
    auto module = probe::compileForInspection(compiler, src, "test.D", "mfmabundle");
    auto k = probe::findMethod(module->getStructures()["test.D"], kernel);
    if (!k) { out.why = "no kernel"; return out; }
    auto tm = amd::createAmdgpuTargetMachine("gfx942");
    llvm::LLVMContext ctx;
    llvm::Module dev("bundle", ctx);
    amd::configureDeviceModule(dev, *tm);
    dev.addModuleFlag(llvm::Module::Warning, "cajeta.amdgpu.archlist",
                      llvm::MDString::get(ctx, arches));
    try {
        amd::lowerKernel(k, dev);
    } catch (cajeta::Exception& e) { out.why = e.getMessage(); return out; }
    catch (const std::exception& e) { out.why = e.what(); return out; }
    llvm::raw_string_ostream os(out.ir);
    dev.print(os, nullptr);
    out.ok = true;
    return out;
}

} // namespace

// 2.1.1: the three native shapes reach CDNA3's matrix cores, row- and column-major.
TEST(AmdgpuMfma, f16LowersToMfmaOnGfx942) {
    expectMfma(kF16, "mmRow", "v_mfma_f32_16x16x16_f16");
    expectMfma(kF16, "mmCol", "v_mfma_f32_16x16x16_f16");
}

TEST(AmdgpuMfma, bf16LowersToMfmaOnGfx942) {
    expectMfma(kBf16, "mmRow", "v_mfma_f32_16x16x16_bf16");
    expectMfma(kBf16, "mmCol", "v_mfma_f32_16x16x16_bf16");
}

TEST(AmdgpuMfma, int8LowersToMfmaOnGfx942) {
    expectMfma(kI8, "mmRow", "v_mfma_i32_16x16x32_i8");
    expectMfma(kI8, "mmCol", "v_mfma_i32_16x16x32_i8");
}

// 2.1.1: RDNA is unchanged — the same source is WMMA on gfx1151.
TEST(AmdgpuMfma, gfx1151KeepsWmma) {
    auto l = lowerForAmdgpu(kF16, "mmRow", "test.D", "gfx1151", "mfma");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ptx.find("v_wmma_f32_16x16x16_f16"), std::string::npos) << l.ptx;
    EXPECT_EQ(l.ptx.find("v_mfma"), std::string::npos) << l.ptx;
}

// A module is lowered once for every arch in its bundle. WMMA and MFMA fragments
// cannot both be right, so a bundle mixing RDNA and CDNA lowers the tile portably:
// correct on both, native on neither.
TEST(AmdgpuMfma, aMixedBundleLowersPortably) {
    auto l = lowerForBundle(kF16, "mmRow", "gfx942,gfx1151");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ir.find("llvm.amdgcn.wmma"), std::string::npos) << l.ir;
    EXPECT_EQ(l.ir.find("llvm.amdgcn.mfma"), std::string::npos) << l.ir;
    auto cdna = lowerForBundle(kF16, "mmRow", "gfx942");
    ASSERT_TRUE(cdna.ok) << cdna.why;
    EXPECT_NE(cdna.ir.find("llvm.amdgcn.mfma"), std::string::npos)
        << "a CDNA-only bundle keeps MFMA:\n" << cdna.ir;
}

// The portable distributed tile maps its lanes from the wave width at compile
// time. gfx942 runs 64 lanes, so a tile pinned to 32 would split the tile across
// half the wave and read the wrong cells.
TEST(AmdgpuMfma, aPortableTileOnGfx942IsLaidOutForWave64) {
    auto l = lowerForAmdgpu(kF32, "mmRow", "test.D", "gfx942", "mfma");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(l.ir.find("+wavefrontsize32"), std::string::npos) << l.ir;
    EXPECT_NE(l.ptx.find(".wavefront_size: 64"), std::string::npos) << l.ptx;
    EXPECT_EQ(l.ptx.find("v_mfma"), std::string::npos)
        << "an f32 16x16x16 has no MFMA here; it must stay portable:\n" << l.ptx;
}

// 2.1.2: the fragment map is the documented one. Each role covers the 16x16 tile
// exactly once over 64 lanes; the int8 operand covers it with lanes 0-31 and pads
// lanes 32-63. Spot cells are AMD's matrix-instruction-calculator rows.
TEST(AmdgpuMfma, theFragmentMapCoversTheTileOnce) {
    for (uint32_t use : {0u, 1u, 2u}) {
        std::set<std::pair<unsigned, unsigned>> seen;
        for (unsigned lane = 0; lane < 64; ++lane)
            for (unsigned e = 0; e < 4; ++e) {
                auto c = amd::mfmaFragmentCell(use, 4, lane, e);
                ASSERT_TRUE(c.valid);
                ASSERT_LT(c.row, 16u);
                ASSERT_LT(c.col, 16u);
                EXPECT_TRUE(seen.insert({c.row, c.col}).second)
                    << "use " << use << " lane " << lane << " e " << e << " repeats a cell";
            }
        EXPECT_EQ(256u, seen.size()) << "use " << use;
    }
    for (uint32_t use : {0u, 1u}) {
        std::set<std::pair<unsigned, unsigned>> seen;
        for (unsigned lane = 0; lane < 64; ++lane)
            for (unsigned e = 0; e < 8; ++e) {
                auto c = amd::mfmaFragmentCell(use, 8, lane, e);
                EXPECT_EQ(lane < 32, c.valid) << "lane " << lane;
                if (c.valid) EXPECT_TRUE(seen.insert({c.row, c.col}).second);
            }
        EXPECT_EQ(256u, seen.size()) << "int8 use " << use;
    }
    auto a = amd::mfmaFragmentCell(0, 4, 17, 2);   // A: row 1, k 6
    EXPECT_EQ(1u, a.row); EXPECT_EQ(6u, a.col);
    auto b = amd::mfmaFragmentCell(1, 4, 50, 3);   // B: k 15, col 2
    EXPECT_EQ(15u, b.row); EXPECT_EQ(2u, b.col);
    auto d = amd::mfmaFragmentCell(2, 4, 35, 1);   // D: row 9, col 3
    EXPECT_EQ(9u, d.row); EXPECT_EQ(3u, d.col);
    auto i = amd::mfmaFragmentCell(0, 8, 21, 7);   // int8 A: row 5, k 15
    EXPECT_EQ(5u, i.row); EXPECT_EQ(15u, i.col);
}

// 2.1.3: the 16-bit wide-load fast path assumes WMMA's 16-K-per-lane run and is
// never taken for an MFMA fragment.
TEST(AmdgpuMfma, theWideLoadFastPathIsNotTakenOnMfma) {
    amd::setCoop16WideLoads(true);
    amd::resetCoop16WideLoadsTaken();
    auto l = lowerForAmdgpu(kF16, "mmRow", "test.D", "gfx942", "mfma");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_EQ(0u, amd::coop16WideLoadsTaken());
    amd::resetCoop16WideLoadsTaken();
    auto r = lowerForAmdgpu(kF16, "mmRow", "test.D", "gfx1151", "mfma");
    ASSERT_TRUE(r.ok) << r.why;
    EXPECT_GT(amd::coop16WideLoadsTaken(), 0u) << "the control: gfx1151 takes it";
}

// A wave-32 pin on CDNA used to compile to no kernel at all: gfx942 cannot run
// +wavefrontsize32, and codegen dropped the function without a word. It is refused
// by name instead, and wave 64 stays pinnable.
TEST(AmdgpuMfma, aWave32PinOnGfx942IsRefusedByName) {
    const std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.Wave;\n"
        "public final class D {\n"
        "    @Kernel @Wave(width = 32)\n"
        "    public static void w32(KernelBuffer<float32> c) { c[0] = 1.0f; }\n"
        "    @Kernel @Wave(width = 64)\n"
        "    public static void w64(KernelBuffer<float32> c) { c[0] = 1.0f; }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto l = lowerForAmdgpu(src, "w32", "test.D", "gfx942", "mfma");
    EXPECT_FALSE(l.ok) << "a wave-32 pin on gfx942 must be refused:\n" << l.ptx;
    EXPECT_NE(l.why.find("@Wave(width = 32)"), std::string::npos) << l.why;
    auto ok = lowerForAmdgpu(src, "w64", "test.D", "gfx942", "mfma");
    ASSERT_TRUE(ok.ok) << ok.why;
    EXPECT_EQ(ok.ptx.find("amdhsa.kernels:  []"), std::string::npos) << ok.ptx;
    auto rdna = lowerForAmdgpu(src, "w32", "test.D", "gfx1151", "mfma");
    EXPECT_TRUE(rdna.ok) << "RDNA keeps wave 32: " << rdna.why;
}

// CDNA3's int8 MFMA is signed only, so a uint8 operand is refused by name rather
// than multiplied as signed.
TEST(AmdgpuMfma, aUint8MmaOnGfx942IsRefusedByName) {
    auto l = lowerForAmdgpu(program("uint8", "int32", "0"), "mmRow", "test.D", "gfx942", "mfma");
    EXPECT_FALSE(l.ok) << l.ptx;
    EXPECT_NE(l.why.find("signed only"), std::string::npos) << l.why;
}

// @Occupancy(maxWaves = N) sizes the work group in waves of the kernel's own width.
// An unpinned kernel on CDNA runs at 64, so 8 waves is 512 threads there, and still
// 256 on RDNA. It used to read the wave only off a +wavefrontsize64 pin and answer
// 256 on gfx942, which refuses a 512-thread launch.
TEST(AmdgpuMfma, occupancyInWavesFollowsTheNativeWidth) {
    const std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.Occupancy;\n"
        "public final class D {\n"
        "    @Kernel @Occupancy(maxWaves = 8)\n"
        "    public static void k(KernelBuffer<float32> c) { c[0] = 1.0f; }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto cdna = lowerForAmdgpu(src, "k", "test.D", "gfx942", "mfma");
    ASSERT_TRUE(cdna.ok) << cdna.why;
    EXPECT_NE(cdna.ir.find("\"amdgpu-flat-work-group-size\"=\"1,512\""), std::string::npos) << cdna.ir;
    auto rdna = lowerForAmdgpu(src, "k", "test.D", "gfx1151", "mfma");
    ASSERT_TRUE(rdna.ok) << rdna.why;
    EXPECT_NE(rdna.ir.find("\"amdgpu-flat-work-group-size\"=\"1,256\""), std::string::npos) << rdna.ir;
}
