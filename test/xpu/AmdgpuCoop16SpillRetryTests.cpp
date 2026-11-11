//
// AmdgpuCoop16SpillRetryTests — the 16-bit wide fragment load on amdgpu (e83f7f35) and the
// re-lowering that keeps it out of a kernel it pushes past the register file (cajeta-ml-gpu 23.2.2.1).
//

#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"
#include "../jit/JitTestHelper.h"

#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/amd/AmdgpuKernelLowering.h"
#include "cajeta/xpu/amd/AmdgpuRegistration.h"
#include "cajeta/xpu/core/KernelManifest.h"

#include <string>
#include <vector>

using namespace cajeta::xpu::probe;

namespace {

// One row-major f16 A fragment at stride 16: each lane's 16 halves are consecutive.
const char* kLoad =
    "package test;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "public final class M {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float16> a, KernelBuffer<float16> b) {\n"
    "        CooperativeMatrix<float16,16,16,0> ma;\n"
    "        ma.load(a, 0, 0, 16);\n"
    "        CooperativeMatrix<float16,16,16,1> mb;\n"
    "        mb.load(b, 0, 1, 16);\n"
    "        CooperativeMatrix<float32,16,16,2> acc;\n"
    "        acc.splat(0.0f);\n"
    "        acc.mma(ma, mb);\n"
    "        acc.store(out, 0, 0, 16);\n"
    "    }\n"
    "}\n";

// cajeta-llm's q4kF16CoopN256Kernel (llm b2c3a9b) and its half decode, verbatim: sixteen f32 accumulators and eight
// f16 fragments a K step, at the 256-VGPR edge on gfx1151, where the wide loads spilled 28 bytes.
const char* kN256 = R"CJ(package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.lang.Cajeta;
public final class M {
    @Device
    public static float32 halfBitsToF32(int32 h) {
        int32 sign = (h >> 15) & 1;
        int32 exp = (h >> 10) & 31;
        int32 man = h & 1023;
        int64 bits;
        if (exp == 0) {
            if (man == 0) {
                bits = (int64) sign << 31;
            } else {
                int32 k = 9;
                while ((man & (1 << k)) == 0) { k = k - 1; }
                bits = ((int64) sign << 31)
                     | ((int64) (k + 103) << 23)
                     | (((int64) man << (23 - k)) & 8388607L);
            }
        } else if (exp == 31) {
            bits = ((int64) sign << 31) | (255L << 23) | ((int64) man << 13);
        } else {
            bits = ((int64) sign << 31)
                 | ((int64) (exp + 112) << 23)
                 | ((int64) man << 13);
        }
        return Cajeta.bitsToF32(bits);
    }

    @Kernel(targets = {"gpu"})
    public static void q4kF16CoopN256Kernel(KernelBuffer<float32> y,
            KernelBuffer<int32> packed, KernelBuffer<float16> xh,
            uint32 tokens, uint32 outDim, uint32 nRowTiles,
            int64 blocksPerRow) {
        Shared<float16> sa = shared float16[128 * 72];
        Shared<float16> sb = shared float16[256 * 72];
        uint32 lane = KernelThread.x();
        uint32 wg = KernelThread.globalIdX() / 256;
        uint32 nTokTiles = tokens / 256;
        uint32 tt = wg % nTokTiles;
        uint32 to = wg / nTokTiles;
        uint32 sg = lane / 32;
        uint32 warpR = sg % 2;
        uint32 warpC = sg / 2;
        uint32 aRow = lane & 127;
        uint32 aHalf = lane >> 7;
        int64 rowWords = blocksPerRow * 36L;
        int64 kBlocks = blocksPerRow * 8L;
        int64 cols = kBlocks * 32L;
        CooperativeMatrix<float32,16,16,2> a00; a00.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a01; a01.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a02; a02.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a03; a03.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a10; a10.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a11; a11.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a12; a12.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a13; a13.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a20; a20.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a21; a21.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a22; a22.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a23; a23.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a30; a30.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a31; a31.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a32; a32.splat(0.0f);
        CooperativeMatrix<float32,16,16,2> a33; a33.splat(0.0f);
        int64 kb = 0;
        while (kb < kBlocks) {
            int64 kbh = kb + (int64) aHalf;
            int64 blk = kbh >> 3;
            int32 gq = (int32) (kbh & 7L);
            int64 ro = ((int64) (to * 128 + aRow)) * rowWords
                + blk * 36L;
            Vector<int32,4> hw = packed.vload<4>(ro);
            int32 h0 = hw[0];
            float32 d = M.halfBitsToF32(h0 & 65535);
            float32 dmin = M.halfBitsToF32((h0 >> 16) & 65535);
            int32 hA = hw[1];
            int32 hB = hw[2];
            int32 hC = hw[3];
            int32 sRaw = 0;
            int32 mRaw = 0;
            if (gq < 4) {
                sRaw = (hA >> (8 * gq)) & 63;
                mRaw = (hB >> (8 * gq)) & 63;
            } else {
                int32 j = gq - 4;
                int32 kk = (hC >> (8 * j)) & 255;
                int32 dd = (hA >> (8 * j)) & 255;
                int32 mm = (hB >> (8 * j)) & 255;
                sRaw = (kk & 15) | ((dd >> 2) & 48);
                mRaw = (kk >> 4) | ((mm >> 2) & 48);
            }
            float32 sc = d * (float32) sRaw;
            float32 mn = dmin * (float32) mRaw;
            int32 rg = gq >> 1;
            int32 nib = (gq & 1) * 4;
            Vector<int32,8> qw = packed.vload<8>(ro + 4L
                + (int64) (rg * 8));
            int64 aBase = (int64) aRow * 72L + (int64) (32 * aHalf);
            int32 e = 0;
            while (e < 8) {
                int32 w = qw[e];
                int32 bi = 0;
                while (bi < 4) {
                    int32 q = (w >> (8 * bi + nib)) & 15;
                    sa[aBase + (int64) (4 * e + bi)] = (float16)
                        (sc * (float32) q - mn);
                    bi = bi + 1;
                }
                e = e + 1;
            }
            int32 hs2 = 0;
            while (hs2 < 2) {
                int64 sBase = (int64) lane * 72L + (int64) (32 * hs2);
                int64 xBase = ((int64) (tt * 256 + lane)) * cols
                    + (kb + (int64) hs2) * 32L;
                int32 jj = 0;
                while (jj < 32) {
                    Vector<float16,4> vv = xh.vload<4>(xBase
                        + (int64) jj);
                    sb.vstore(sBase + (int64) jj, vv);
                    jj = jj + 4;
                }
                hs2 = hs2 + 1;
            }
            Barrier.workgroup();
            int32 ks = 0;
            while (ks < 4) {
                uint32 ko = (uint32) (16 * ks);
                uint32 ab = (warpR * 64) * 72 + ko;
                uint32 bb2 = (warpC * 64) * 72 + ko;
                CooperativeMatrix<float16,16,16,0> t0;
                CooperativeMatrix<float16,16,16,0> t1;
                CooperativeMatrix<float16,16,16,0> t2;
                CooperativeMatrix<float16,16,16,0> t3;
                t0.load(sa, ab, 0, 72);
                t1.load(sa, ab + 1152, 0, 72);
                t2.load(sa, ab + 2304, 0, 72);
                t3.load(sa, ab + 3456, 0, 72);
                CooperativeMatrix<float16,16,16,1> u0;
                CooperativeMatrix<float16,16,16,1> u1;
                CooperativeMatrix<float16,16,16,1> u2;
                CooperativeMatrix<float16,16,16,1> u3;
                u0.load(sb, bb2, 1, 72);
                u1.load(sb, bb2 + 1152, 1, 72);
                u2.load(sb, bb2 + 2304, 1, 72);
                u3.load(sb, bb2 + 3456, 1, 72);
                a00.mma(t0, u0); a01.mma(t0, u1);
                a02.mma(t0, u2); a03.mma(t0, u3);
                a10.mma(t1, u0); a11.mma(t1, u1);
                a12.mma(t1, u2); a13.mma(t1, u3);
                a20.mma(t2, u0); a21.mma(t2, u1);
                a22.mma(t2, u2); a23.mma(t2, u3);
                a30.mma(t3, u0); a31.mma(t3, u1);
                a32.mma(t3, u2); a33.mma(t3, u3);
                ks = ks + 1;
            }
            Barrier.workgroup();
            kb = kb + 2L;
        }
        uint32 rb = to * 128 + warpR * 64;
        uint32 cb = tt * 256 + warpC * 64;
        a00.store(y, cb * outDim + rb, 1, outDim);
        a01.store(y, (cb + 16) * outDim + rb, 1, outDim);
        a02.store(y, (cb + 32) * outDim + rb, 1, outDim);
        a03.store(y, (cb + 48) * outDim + rb, 1, outDim);
        a10.store(y, cb * outDim + rb + 16, 1, outDim);
        a11.store(y, (cb + 16) * outDim + rb + 16, 1, outDim);
        a12.store(y, (cb + 32) * outDim + rb + 16, 1, outDim);
        a13.store(y, (cb + 48) * outDim + rb + 16, 1, outDim);
        a20.store(y, cb * outDim + rb + 32, 1, outDim);
        a21.store(y, (cb + 16) * outDim + rb + 32, 1, outDim);
        a22.store(y, (cb + 32) * outDim + rb + 32, 1, outDim);
        a23.store(y, (cb + 48) * outDim + rb + 32, 1, outDim);
        a30.store(y, cb * outDim + rb + 48, 1, outDim);
        a31.store(y, (cb + 16) * outDim + rb + 48, 1, outDim);
        a32.store(y, (cb + 32) * outDim + rb + 48, 1, outDim);
        a33.store(y, (cb + 48) * outDim + rb + 48, 1, outDim);
    }
}
)CJ";

struct WideOff {
    WideOff() { cajeta::xpu::amd::setCoop16WideLoads(false); }
    ~WideOff() { cajeta::xpu::amd::setCoop16WideLoads(true); }
};

} // namespace

TEST(AmdgpuCoop16SpillRetryTests, aConsecutiveF16FragmentIsOneWideLoad) {
    auto L = lowerForAmdgpu(kLoad, "k");
    ASSERT_TRUE(L.ok) << L.why;
    EXPECT_EQ(countOf(L.ir, "load <16 x half>"), 2u) << L.ir;
}

TEST(AmdgpuCoop16SpillRetryTests, withTheWideLoadOffTheFragmentLoadsByElement) {
    WideOff off;
    auto L = lowerForAmdgpu(kLoad, "k");
    ASSERT_TRUE(L.ok) << L.why;
    EXPECT_EQ(countOf(L.ir, "load <16 x half>"), 0u) << L.ir;
    EXPECT_EQ(countOf(L.ir, "cm.ld16"), 0u) << L.ir;
}

TEST(AmdgpuCoop16SpillRetryTests, aKernelTheWideLoadsSpillIsReloweredWithoutThem) {
    CAJETA_SKIP_IF_NO_HIP();
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kN256);
    auto k = findMethod(module->getStructures()["test.M"], "q4kF16CoopN256Kernel");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_coop16_retry", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx1151", {}, &out);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(out.size(), 1u) << err;
    ASSERT_TRUE(out[0].spillBytes.has_value());
    EXPECT_EQ(*out[0].spillBytes, 0u) << cajeta::xpu::toJson(out[0]) << err;
    EXPECT_NE(err.find("[xpu-kernel-relowered]"), std::string::npos) << err;
    EXPECT_EQ(err.find("[xpu-kernel-spill]"), std::string::npos) << err;
}

TEST(AmdgpuCoop16SpillRetryTests, aKernelThatDoesNotSpillKeepsItsWideLoads) {
    CAJETA_SKIP_IF_NO_HIP();
    cajeta::Compiler compiler;
    auto module = compileForInspection(compiler, kLoad);
    auto k = findMethod(module->getStructures()["test.M"], "k");
    ASSERT_NE(k, nullptr);
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_coop16_keep", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::amd::emitKernelRegistration({k}, host, "gfx1151", {}, &out);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(out.size(), 1u) << err;
    EXPECT_EQ(err.find("[xpu-kernel-relowered]"), std::string::npos) << err;
}
