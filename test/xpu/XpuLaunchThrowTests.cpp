//
// Unit 2.1.2 / 2.1.3: a REFUSED launch is catchable, naming kernel + backend.
//
// A kernel that a backend skips registers no device code, so its launch prints
// `no registered kernel` and no-ops — and the caller reads back whatever the
// output buffer already held, a false sense that the run worked. The runtime now
// records the refusal (thread-local: kernel name + backend), and
// `Device.checkLaunch()` reads it and RAISES a catchable `XpuLaunchException`
// naming both. A routing layer catches it and falls through; a direct launch
// lets it abort rather than compute on garbage.
//
// The compiler emits `Device.checkLaunch()` after every `.launch()`, so a
// refused launch RAISES at the launch site itself. This pins the two halves a
// check needs (CLAUDE.md §5) on the CPU backend, which refuses a native-only
// construct deterministically with no device:
//   FIRES         — a WaveVector.ofLane kernel (native-only) is refused on cpu;
//                   its `.launch()` throws XpuLaunchException naming the kernel
//                   and "cpu", caught at the launch site.
//   does NOT fire — a plain kernel registers and runs; its `.launch()` is silent.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSrc = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.WaveVector;
import cajeta.xpu.XpuLaunchException;
public final class M {
    // Refused on cpu: WaveVector.ofLane is native-only, so this registers no cpu
    // code and its launch is a no-op that records a refusal.
    @Kernel
    public static void refusedK(KernelBuffer<float32> rowIn,
                                KernelBuffer<float32> colIn,
                                KernelBuffer<float32> out) {
        Shared<float32> rowF = shared float32[16];
        Shared<float32> colF = shared float32[16];
        uint32 lane = KernelThread.x();
        if (lane < 16) { rowF[lane] = rowIn[lane]; colF[lane] = colIn[lane]; }
        Barrier.workgroup();
        float32 cv = colIn[(int64) (lane & 15)];
        CooperativeMatrix<float32,16,16,2> facc;
        facc.splat(0.0f);
        facc.rank1Accum(rowF, WaveVector.ofLane(cv));
        facc.store(out, 0, 0, 16);
    }
    // Registers on cpu: a plain elementwise kernel, no native-only construct.
    @Kernel
    public static void plainK(KernelBuffer<int32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = 7;
    }
    // 0 = both halves right; 1..4 identify which failed.
    public static int32 run() {
        KernelStream s #= KernelStream.current();
        KernelBuffer<float32> rin = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> cin = heap KernelBuffer<float32>(16);
        KernelBuffer<float32> fout = heap KernelBuffer<float32>(256);
        // FIRES: the refused launch raises at the launch site (compiler-emitted
        // checkLaunch), catchable and naming the kernel + backend.
        boolean threw = false;
        try {
            refusedK.launch(s, grid: [1], block: [32])(rin, cin, fout);
        } catch (XpuLaunchException e) {
            threw = true;
            if (!e.message.contains("refusedK")) { return 2; }
            if (!e.message.contains("cpu")) { return 3; }
        }
        if (!threw) { return 1; }
        // does NOT fire: a registered launch does not raise.
        KernelBuffer<int32> pout = heap KernelBuffer<int32>(16);
        try {
            plainK.launch(s, grid: [1], block: [16])(pout);
            s.sync();
        } catch (XpuLaunchException e) {
            return 4;
        }
        return 0;
    }
}
)CJ";

int runOn(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSrc, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn() : -1;
}

}  // namespace

// The CPU arm is deterministic and needs no device: it refuses the native-only
// ofLane kernel by name, so both halves run here.
TEST(XpuLaunchThrow, cpuRefusedLaunchRaisesCatchableNamed) {
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Cpu), 0)
        << "1 = a refused launch did NOT raise; "
           "2 = the exception did not name the kernel; "
           "3 = the exception did not name the backend; "
           "4 = a registered launch wrongly raised";
}
