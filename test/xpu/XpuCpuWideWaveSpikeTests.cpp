//
// DE-RISKING SPIKE for Unit 4A.7 (distributed software coop-matrix tile on
// CPU). The whole unit rests on one question: will LLVM LoopVectorize take
// VF=16 on a host whose native i32 vector is 8 wide (avx2), and correctly
// substitute a WIDTH-16 wave-op VFABI variant? The distributed coop tile
// needs a cooperative wave of Cols=16 lanes, which is 2x the register width.
//
// The probe is a full 16-lane REVERSAL shuffle: lane L reads lane (w-1-L).
// This is decisive by construction — if vectorization bails and the wave op
// falls back to its scalar width-1 stub (the IDENTITY, `return value`), then
// out[t] == in[t] and the reversal check fails. A green result therefore
// means BOTH that VF=16 fired AND that the width-16 cross-lane shuffle is
// numerically correct across all 16 lanes. If this walls, Unit 4A.7 does
// not have a foundation and the plan must change.
//
// The width is forced through the spike seam CAJETA_XPU_CPU_WAVE_WIDTH (the
// real unit keys it per kernel off the distributed-coop marker). The control
// test runs the SAME kernel at the host's native width to prove the seam is
// additive — ordinary CPU wave kernels are untouched.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv — absent from the MinGW CRT
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A reversal shuffle plus a width probe. `runRev()` returns the wave width W
// on success, or 1000+t at the first cell whose reversed value is wrong (which
// is what a scalarized identity shuffle produces). Width-agnostic: it verifies
// against the W the kernel actually ran at, so the same source proves both the
// forced-16 case and the native-width control.
const char* kSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    public static void widthk(KernelBuffer<uint32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = Wave.width();
    }
    @Kernel
    public static void revk(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        uint32 t = KernelThread.globalIdX();
        uint32 w = Wave.width();
        uint32 lane = Wave.laneId();
        uint32 src = w - 1 - lane;               // reversed lane in this wave
        out[t] = Wave.shuffleSync(in[t], src);
    }

    public static uint32 probeWidth() {
        uint32 n = 256;
        uint32[] h = heap uint32[n];
        KernelBuffer<uint32> b = heap KernelBuffer<uint32>(n);
        KernelStream s #= KernelStream.current();
        widthk.launch(s, grid: [1], block: [256])(b);
        s.sync();
        b.download(h);
        return h[0];
    }

    public static uint32 runRev() {
        uint32 w = probeWidth();
        if (w < 2) { return 0; }
        uint32 n = 256;
        uint32[] hin = heap uint32[n];
        uint32[] hout = heap uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = i + 100; hout[i] = 0; }
        KernelBuffer<uint32> bin = heap KernelBuffer<uint32>(n);
        KernelBuffer<uint32> bout = heap KernelBuffer<uint32>(n);
        bin.upload(hin);
        KernelStream s #= KernelStream.current();
        revk.launch(s, grid: [1], block: [256])(bout, bin);
        s.sync();
        bout.download(hout);
        // out[t] must be in[base + (w-1-lane)]; a scalar identity gives in[t].
        for (uint32 t = 0; t < n; t = t + 1) {
            uint32 base = (t / w) * w;
            uint32 lane = t % w;
            uint32 revsrc = base + (w - 1 - lane);
            if (hout[t] != hin[revsrc]) { return 1000 + t; }
        }
        return w;
    }
}
)CJ";

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

unsigned runRev() {
    auto jit = CajetaJit::compile(kSrc, "test.M", cpuOptions());
    EXPECT_NE(jit, nullptr);
    if (!jit) return 0;
    auto fn = jit->lookup<unsigned (*)()>("runRev");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : 0;
}

}  // namespace

// THE SPIKE. Force a 16-lane cooperative wave on a host whose native width is
// 8, and prove a full 16-lane reversal shuffle is correct. A pass means
// LoopVectorize took VF=16 and matched the width-16 shuffle variant.
TEST(XpuCpuWideWaveSpike, sixteenLaneReversalShuffleIsCorrectAtForcedVF16) {
    setenv("CAJETA_XPU_CPU_WAVE_WIDTH", "16", 1);
    unsigned w = runRev();
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    EXPECT_EQ(w, 16u)
        << "forced VF=16 did not produce a correct 16-lane reversal shuffle; "
           "w=" << w << " (0 = width<2, 1000+t = wrong/identity at cell t, "
           "8 = the force was ignored). Unit 4A.7 rests on this.";
}

// CONTROL: the same source at the host's native width still runs and is
// correct, so the spike seam is additive — ordinary CPU wave kernels are
// untouched. The native width is whatever this host reports (>=2).
TEST(XpuCpuWideWaveSpike, nativeWidthReversalStillCorrect) {
    unsetenv("CAJETA_XPU_CPU_WAVE_WIDTH");
    unsigned w = runRev();
    EXPECT_GE(w, 2u)
        << "the native-width reversal shuffle regressed; w=" << w;
}
