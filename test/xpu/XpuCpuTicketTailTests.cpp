//
// A wave reduce whose result is used only under `if (lane == 0)` must still reduce
// every lane (xpu-kernel-adaptor 4.2.1.13). In a kernel holding a Vector value the
// cpu pipeline runs InstCombine, which sank the pure-looking reduce into that branch,
// and LoopVectorize masked it to lane 0. The shape is cajeta-llm's id down-combine
// tail: a ticket, the last block summing a row, shared partials.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.MemoryOrder;
import cajeta.xpu.Shared;
import cajeta.xpu.Wave;
public class M {
    @Kernel
    @Wave(width = 32)
    public static void plaink(KernelBuffer<float32> xs, KernelBuffer<float32> out, uint32 dim) {
        Shared<float32> part = shared float32[8];
        uint32 tid = KernelThread.x();
        uint32 wid = tid / 32;
        uint32 wl = tid % 32;
        float32 ss = 0.0f;
        uint32 i = tid;
        while (i < dim) {
            float32 v = xs[(int64) i];
            ss = ss + v * v;
            i = i + 256;
        }
        float32 wsum = Wave.reduceSumF32(ss);
        if (wl == 0) { part[wid] = wsum; }
        Barrier.workgroup();
        float32 tsq = part[0] + part[1] + part[2] + part[3]
            + part[4] + part[5] + part[6] + part[7];
        if (tid == 0) { out[0L] = tsq; }
    }

    @Kernel
    @Wave(width = 32)
    public static void tailk(KernelBuffer<float32> xs, KernelBuffer<float32> out,
            KernelBuffer<int32> counter, uint32 nblk, uint32 dim, uint32 rows,
            uint32 normTail, KernelBuffer<float32> nOut, KernelBuffer<int32> pk) {
        Shared<float32> part = shared float32[8];
        Shared<int32> psum = shared int32[256];
        Shared<int32> tk = shared int32[1];
        uint32 lane = KernelThread.x() % 32;
        uint32 row = KernelThread.globalIdX() / 32;
        float32 acc = 0.0f;
        if (row < rows) {
            Vector<int32,4> zv = heap Vector<int32,4>(0, 0, 0, 0);
            uint32 kb = lane;
            while (kb < 64) {
                acc = acc + 0.0f * (float32) kb;
                kb = kb + 32;
            }
        }
        float32 tot = Wave.reduceSumF32(acc);
        if (lane == 0 && row < rows) {
            xs[(int64) row] = xs[(int64) row] + tot;
        }
        if (normTail == 0) { return; }
        uint32 tid = KernelThread.x();
        if (lane == 0) { Barrier.deviceMemory(MemoryOrder.Release); }
        Barrier.workgroup();
        if (tid == 0) {
            int32 old = counter.atomicAdd(0L, 1, MemoryOrder.AcqRel);
            tk[0] = old;
        }
        Barrier.workgroup();
        if ((uint32) tk[0] != nblk - 1) { return; }
        Barrier.deviceMemory(MemoryOrder.Acquire);
        if (tid == 0) { counter[0L] = 0; }
        uint32 wid = tid / 32;
        uint32 wl = tid % 32;
        float32 ss = 0.0f;
        uint32 i = tid;
        while (i < dim) {
            float32 v = xs[(int64) i];
            ss = ss + v * v;
            i = i + 256;
        }
        float32 wsum = Wave.reduceSumF32(ss);
        if (wl == 0) { part[wid] = wsum; }
        Barrier.workgroup();
        float32 tsq = part[0] + part[1] + part[2] + part[3]
            + part[4] + part[5] + part[6] + part[7];
        if (tid == 0) { out[0L] = tsq; }
        float32 scale = 1.0f / Math.sqrt(tsq / (float32) dim + 0.00001f);
        uint32 blocks = dim / 256;
        float32 v0 = 0.0f; float32 v1 = 0.0f; float32 v2 = 0.0f; float32 v3 = 0.0f;
        float32 v4 = 0.0f; float32 v5 = 0.0f; float32 v6 = 0.0f; float32 v7 = 0.0f;
        uint32 e0 = wid * 256 + wl * 8;
        if (wid < blocks) {
            v0 = xs[(int64) e0] * scale;
            v1 = xs[(int64) (e0 + 1)] * scale;
            v2 = xs[(int64) (e0 + 2)] * scale;
            v3 = xs[(int64) (e0 + 3)] * scale;
            v4 = xs[(int64) (e0 + 4)] * scale;
            v5 = xs[(int64) (e0 + 5)] * scale;
            v6 = xs[(int64) (e0 + 6)] * scale;
            v7 = xs[(int64) (e0 + 7)] * scale;
            nOut[e0] = v0; nOut[e0 + 1] = v1; nOut[e0 + 2] = v2; nOut[e0 + 3] = v3;
            nOut[e0 + 4] = v4; nOut[e0 + 5] = v5; nOut[e0 + 6] = v6; nOut[e0 + 7] = v7;
        }
        float32 amax = 0.0f;
        float32 av = v0; if (av < 0.0f) { av = 0.0f - av; } if (av > amax) { amax = av; }
        av = v7; if (av < 0.0f) { av = 0.0f - av; } if (av > amax) { amax = av; }
        float32 bmax = Wave.reduceMaxF32(amax);
        int32 sm = 0;
        if (wid < blocks) {
            sm = (int32) Math.round(v0 * bmax);
        }
        psum[tid] = sm;
        Barrier.workgroup();
        if (wid < blocks && wl < 8) {
            uint32 base = wid * 32 + wl * 4;
            pk[(int64) base] = psum[base] + psum[base + 1];
        }
    }

    public static float32 run(int32 blocks, int32 ticketed) {
        uint32 dim = 512;
        float32[] h = heap float32[dim];
        for (uint32 i = 0; i < dim; i = i + 1) { h[i] = i < 256 ? (float32) (i + 1) : 0.0f; }
        KernelBuffer<float32> xs = heap KernelBuffer<float32>(dim);
        xs.upload(h);
        KernelBuffer<float32> out = heap KernelBuffer<float32>(1);
        KernelBuffer<int32> counter = heap KernelBuffer<int32>(4);
        KernelBuffer<float32> nOut = heap KernelBuffer<float32>(dim);
        KernelBuffer<int32> pk = heap KernelBuffer<int32>(dim);
        int32[] z = heap int32[4];
        counter.upload(z);
        KernelStream s #= KernelStream.current();
        if (ticketed == 0) {
            plaink.launch(s, grid: [1], block: [256])(xs, out, dim);
        } else {
            tailk.launch(s, grid: [(uint32) blocks], block: [256])
                (xs, out, counter, (uint32) blocks, dim, (uint32) blocks * 8, 1, nOut, pk);
        }
        s.sync();
        float32[] r = heap float32[1];
        out.download(r);
        return r[0];
    }
}
)CJ";

float runOnCpu(int blocks, int ticketed) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1000.0f;
    auto fn = jit->lookup<float (*)(int, int)>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn(blocks, ticketed) : -1000.0f;
}

}  // namespace

// The control: one block, no ticket, every thread sums its two elements.
TEST(XpuCpuTicketTail, theUnticketedSumCoversEveryThread) {
    EXPECT_EQ(runOnCpu(1, 0), 5625216.0f);
}

// One block takes the ticket and is last, so the tail runs with all 256 threads.
TEST(XpuCpuTicketTail, aSingleTicketedBlockSumsEveryThread) {
    EXPECT_EQ(runOnCpu(1, 1), 5625216.0f);
}

// Sixty-four blocks race for the ticket; only the last runs the tail.
TEST(XpuCpuTicketTail, theLastOfManyTicketedBlocksSumsEveryThread) {
    EXPECT_EQ(runOnCpu(64, 1), 5625216.0f);
}
