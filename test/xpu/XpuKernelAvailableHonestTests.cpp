//
// Unit 2: the honest predicate. `Device.kernelAvailable(name)` must answer for
// the registry the LAUNCH consults, so a router that picks a route on it and a
// launch that then runs it cannot disagree.
//
// The lie it replaces: on CUDA/HIP the predicate returned 1 unconditionally,
// on the assumption "we compile every kernel". A kernel that is skipped or
// never declared registers no module, so its launch prints "no registered
// kernel" and no-ops while this said yes — and a caller reads back whatever
// was in the output buffer as an answer. Measured on cajeta-llm: a route was
// chosen whose kernels no-opped and the engine returned zeros with nothing
// failing (launch.c:1175).
//
// This checks BOTH halves on a live device: a REGISTERED kernel answers true
// (does-NOT-fire), and a never-declared name answers false (the fix). On the
// unfixed CUDA arm the second returns true, which is the bug.
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
import cajeta.xpu.Device;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void realk(KernelBuffer<int32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = 42;
    }
    // Returns 0 on success; a small code identifies which half failed.
    public static int32 run() {
        KernelBuffer<int32> b = heap KernelBuffer<int32>(16);
        KernelStream s #= KernelStream.current();
        realk.launch(s, grid: [1], block: [16])(b);   // activate the backend
        s.sync();
        // A registered kernel is available (does-NOT-fire half).
        if (!Device.kernelAvailable("realk")) { return 1; }
        // A name that was never declared is NOT available. On the unfixed
        // CUDA/HIP arm this answers true — the lie.
        if (Device.kernelAvailable("ghostKernelNeverDeclared")) { return 2; }
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

// The CPU arm is already honest (it checks the thunk registry); this pins it as
// the control that both halves are right where the predicate was never a lie.
TEST(XpuKernelAvailableHonest, cpuAnswersForTheRegistry) {
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Cpu), 0)
        << "1 = a registered kernel read as unavailable; "
           "2 = a never-declared name read as available";
}

// The fix: on NVIDIA the predicate must answer for the module registry the
// launch consults, not return 1 unconditionally.
TEST(XpuKernelAvailableHonest, nvptxDoesNotClaimAGhostKernel) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runOn(cajeta::xpu::Backend::Nvptx), 0)
        << "2 = kernelAvailable claimed a never-declared kernel exists on "
           "nvptx (the unconditional-yes lie); 1 = a real kernel read as "
           "absent (registry-name mismatch)";
}
