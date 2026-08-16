//
// CajetaXPU — AsyncCopy (xpu-pipelined-gemm-primitives Unit 1): the device-side
// async global->LDS copy + group commit/wait, run end to end on the CPU backend.
//
// AsyncCopy.copy issues a direct global->Shared<T> transfer (the workgroup
// stripes it across its threads); commit closes a group; wait(n) blocks until
// <= n groups remain. On a backend with no native async path (CPU here) all
// three lower to a SYNCHRONOUS strided copy + no-op commit/wait — bit-identical
// to a synchronous CoopStage.panel, just without compute/transfer overlap.
//
// These JIT-compile a kernel `--xpu-backend=cpu` and run it: (1) an async-staged
// tile read cross-lane after a barrier is correct for every work-item (the
// staging completed for the whole block), and (2) the AsyncCopy path produces
// byte-identical output to the same staging written with CoopStage.panel.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// Stage in[0..256) into a shared tile via AsyncCopy (whole-panel copy, workgroup-
// strided), commit + wait(0) to drain, barrier, then read a DIFFERENT lane
// (tile[255-t]). out[t] == in[255-t] == 255-t for every t proves the async copy
// landed the whole block before any read. The commit/wait verbs are exercised
// (no-ops on CPU) and must compile and register.
const char* kAsyncStageSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.AsyncCopy;
public class M {
    @Kernel
    public static void asyncstage(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        Shared<uint32> tile = shared uint32[256];
        uint32 t = KernelThread.x();
        AsyncCopy.copy(tile, 0, in, 0, 256);
        AsyncCopy.commit();
        AsyncCopy.wait(0);
        Barrier.workgroup();
        out[t] = tile[255 - t];
    }
    public static uint32 run() {
        uint32 n = 256;
        uint32[] hin = heap uint32[n];
        uint32[] hout = heap uint32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = i; hout[i] = 0; }
        KernelBuffer<uint32> in = heap KernelBuffer<uint32>(n);
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(n);
        in.upload(hin);
        out.upload(hout);
        KernelStream s #= KernelStream.current();
        asyncstage.launch(s, grid: [1], block: [256])(out, in);
        s.sync();
        out.download(hout);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hout[i] != 255 - i) { return 100 + i; }
        }
        return 12345;
    }
}
)CJ";

// The same staging written two ways — AsyncCopy.copy vs CoopStage.panel — into
// the SAME shape (a 1x256 row panel). run() launches both and compares every
// lane: the async fallback must be byte-identical to the synchronous panel copy.
const char* kAsyncVsPanelSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Barrier;
import cajeta.xpu.Shared;
import cajeta.xpu.AsyncCopy;
import cajeta.xpu.CoopStage;
public class M {
    @Kernel
    public static void viaAsync(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> tile = shared int32[256];
        uint32 t = KernelThread.x();
        AsyncCopy.copy(tile, 0, in, 0, 256);
        AsyncCopy.commit();
        AsyncCopy.wait(0);
        Barrier.workgroup();
        out[t] = tile[t] + tile[(t + 7) & 255];
    }
    @Kernel
    public static void viaPanel(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        Shared<int32> tile = shared int32[256];
        uint32 t = KernelThread.x();
        CoopStage.panel(tile, in, 0, 0, 1, 256, 256);
        Barrier.workgroup();
        out[t] = tile[t] + tile[(t + 7) & 255];
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] hin = heap int32[n];
        int32[] hA = heap int32[n];
        int32[] hB = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = (int32) (i * 3 + 1); hA[i] = 0; hB[i] = 0; }
        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> oa = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> ob = heap KernelBuffer<int32>(n);
        in.upload(hin);
        oa.upload(hA);
        ob.upload(hB);
        KernelStream s #= KernelStream.current();
        viaAsync.launch(s, grid: [1], block: [256])(oa, in);
        viaPanel.launch(s, grid: [1], block: [256])(ob, in);
        s.sync();
        oa.download(hA);
        ob.download(hB);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (hA[i] != hB[i]) { return (int32) (100 + i); }
        }
        return 777;
    }
}
)CJ";

} // namespace

// U1.1.a + U1.3: a kernel using AsyncCopy.copy/commit/wait compiles, registers,
// and runs correct on --xpu-backend=cpu (synchronous fallback).
TEST(XpuAsyncCopyTests, asyncStagedTileReadCrossLane) {
    auto jit = CajetaJit::compile(kAsyncStageSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<unsigned (*)()>("run");
    ASSERT_NE(fn, nullptr);
    unsigned r = fn();
    EXPECT_EQ(r, 12345u) << "fail code " << r << " (100+i: out[i] != 255-i)";
}

// U1.1.b: the AsyncCopy fallback is bit-identical to a synchronous CoopStage.panel.
TEST(XpuAsyncCopyTests, asyncCopyMatchesSynchronousPanel) {
    auto jit = CajetaJit::compile(kAsyncVsPanelSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777) << "fail code " << r << " (100+i: async[i] != panel[i])";
}
