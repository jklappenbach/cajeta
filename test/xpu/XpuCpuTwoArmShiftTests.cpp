// A helper that returns early for half an index range, called in a short constant-trip loop, keeps
// its two arms selected on the cpu when the arm not taken shifts out of range
// (cpu-widened-loop-or-miscompile 2.3, 2.4).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

using cajeta_test::CajetaJit;

namespace {

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Vector;
public final class Six {
    @Device
    public static int32 a(int32 lo, int32 hi, uint32 j) {
        if (j < 4) { return (lo >> (int32) (8 * j)) & 63; }
        int32 s = (int32) (8 * (j - 4));
        return ((hi >> s) & 15) | ((((lo >> s) & 255) >> 2) & 48);
    }
    @Device
    public static int32 b(int32 lo, int32 hi, uint32 j) {
        if (j < 4) { return (lo >> (int32) (8 * j)) & 63; }
        int32 s = (int32) (8 * (j - 4));
        return (((hi >> s) & 255) >> 4) | ((((lo >> s) & 255) >> 2) & 48);
    }
}
public final class K {
    public K() { return; }
    @Kernel
    public static void run(KernelBuffer<int32> out, KernelBuffer<int8> in, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) {
            Vector<int32,4> h = in.vload<16>((int64) (i * 16)).asWords();
            int32 h1 = h[1];
            int32 h2 = h[2];
            int32 h3 = h[3];
            int64 o = (int64) (i * 17);
            int32 mm = (h1 & 1023) | 1;
            int32 e = 0;
            while ((mm & 1024) == 0) { mm = mm << 1; e = e + 1; }
            out[o + 16L] = e;
            uint32 j = 0;
            while (j < 8) {
                out[o + (int64) j] = Six.a(h1, h3, j);
                out[o + 8L + (int64) j] = Six.b(h2, h3, j);
                j = j + 1;
            }
        }
    }
    static int32 word(int8[] b, int64 at) {
        return ((int32) b[at] & 255) | (((int32) b[at + 1L] & 255) << 8) | (((int32) b[at + 2L] & 255) << 16) | (((int32) b[at + 3L] & 255) << 24);
    }
    public int32 wrong() {
        int64 n = 96L;
        int8[] ih = heap int8[n * 16L];
        int64 i = 0L;
        int32 v = 12345;
        while (i < n * 16L) { v = (v * 1103 + 12345) & 16777215; ih[i] = (int8) ((v >> 9) & 255); i = i + 1L; }
        KernelBuffer<int8> in = heap KernelBuffer<int8>((uint64) (n * 16L));
        KernelBuffer<int32> out = heap KernelBuffer<int32>((uint64) (n * 17L));
        in.upload(ih);
        KernelStream s #= KernelStream.current();
        run.launch(s, grid: [2], block: [64])(out, in, (uint32) n);
        s.sync();
        int32[] got = heap int32[n * 17L];
        out.download(got);
        int32 bad = 0;
        i = 0L;
        while (i < n) {
            uint32 j = 0;
            while (j < 8) {
                if (got[i * 17L + (int64) j] != Six.a(K.word(ih, i * 16L + 4L), K.word(ih, i * 16L + 12L), j)) { bad = bad + 1; }
                if (got[i * 17L + 8L + (int64) j] != Six.b(K.word(ih, i * 16L + 8L), K.word(ih, i * 16L + 12L), j)) { bad = bad + 1; }
                j = j + 1;
            }
            i = i + 1L;
        }
        return bad;
    }
}
public class M {
    public static int32 run() { K k #= heap K(); return k.wrong(); }
}
)CJ";

} // namespace

TEST(XpuCpuTwoArmShift, theArmNotTakenDoesNotLeakIntoTheResult) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    ASSERT_TRUE(jit);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}
