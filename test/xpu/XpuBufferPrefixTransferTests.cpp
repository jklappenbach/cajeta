//
// CajetaXPU — KernelBuffer prefix upload/download (threaded-forward-path 10.1).
//
// `upload(host)` ships the buffer's whole capacity. A buffer sized once to the
// largest payload it will hold and then fed a smaller one therefore pays for
// capacity rather than content — measured in cajeta-llama's activation stage as
// 17920 bytes shipped where 5120 were live, on a pageable host copy where that
// is not free.
//
// `upload(host, count)` / `download(host, count)` ship a PREFIX. These run the
// pair end to end on the CPU backend and assert three things, deliberately
// including the two negatives:
//
//   1. the prefix ARRIVES  — a kernel reading [0, count) sees the host values;
//   2. the prefix STOPS    — device memory past `count` is NOT overwritten, so
//                            a shorter upload cannot be mistaken for a full one;
//   3. the clamp HOLDS     — a count past either end is truncated rather than
//                            reading or writing out of bounds.
//
// (2) is the one that matters: without it a prefix upload that silently copied
// everything would pass a correctness check and quietly undo the transfer win.
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

// A 256-element buffer is filled on the device with a sentinel, then a 64-
// element PREFIX is uploaded over it. Lanes [0,64) must read the new values and
// lanes [64,256) must still read the sentinel.
const char* kPrefixUploadSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    @Kernel
    public static void copyOut(KernelBuffer<int32> out, KernelBuffer<int32> in) {
        uint32 t = KernelThread.globalIdX();
        if (t < 256) { out[t] = in[t]; }
    }
    public static int32 run() {
        uint32 n = 256;
        int32[] sentinel = heap int32[n];
        int32[] fresh = heap int32[n];
        int32[] back = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            sentinel[i] = -1;
            fresh[i] = (int32) (i + 1000);
            back[i] = 0;
        }
        KernelBuffer<int32> in = heap KernelBuffer<int32>(n);
        KernelBuffer<int32> out = heap KernelBuffer<int32>(n);
        in.upload(sentinel);
        out.upload(back);
        // The whole point: only the first 64 elements move.
        in.upload(fresh, 64);
        KernelStream s #= KernelStream.current();
        copyOut.launch(s, grid: [1], block: [256])(out, in);
        s.sync();
        out.download(back);
        for (uint32 i = 0; i < 64; i = i + 1) {
            if (back[i] != (int32) (i + 1000)) { return (int32) (100 + i); }
        }
        // The does-NOT-fire half: everything past the prefix is untouched.
        for (uint32 i = 64; i < n; i = i + 1) {
            if (back[i] != -1) { return (int32) (1000 + i); }
        }
        return 4242;
    }
}
)CJ";

// The download twin, plus the clamp. A full buffer is uploaded, a 32-element
// prefix is downloaded into a zeroed host array, and the rest of that array
// must still be zero. Then an oversized count is asked for on both sides and
// must be truncated rather than running off either end.
const char* kPrefixDownloadSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    // Load-bearing, not decoration: a program with NO @Kernel bundles no
    // backend at all, and its uploads/downloads then silently move nothing —
    // which reads as "the prefix did not arrive" (the 9.2 lesson, met again).
    @Kernel
    public static void touch(KernelBuffer<int32> b) {
        uint32 t = KernelThread.globalIdX();
        if (t < 1) { b[0] = b[0]; }
    }
    public static int32 run() {
        uint32 n = 128;
        int32[] src = heap int32[n];
        int32[] dst = heap int32[n];
        for (uint32 i = 0; i < n; i = i + 1) { src[i] = (int32) (i + 7); dst[i] = 0; }
        KernelBuffer<int32> b = heap KernelBuffer<int32>(n);
        b.upload(src);
        KernelStream s #= KernelStream.current();
        touch.launch(s, grid: [1], block: [1])(b);
        s.sync();
        b.download(dst, 32);
        for (uint32 i = 0; i < 32; i = i + 1) {
            if (dst[i] != (int32) (i + 7)) { return (int32) (100 + i); }
        }
        for (uint32 i = 32; i < n; i = i + 1) {
            if (dst[i] != 0) { return (int32) (1000 + i); }
        }
        // Clamped, not out of bounds: a count past the buffer AND past the
        // host array is truncated to the smaller of the two.
        for (uint32 i = 0; i < n; i = i + 1) { dst[i] = 0; }
        b.download(dst, 4096);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (dst[i] != (int32) (i + 7)) { return (int32) (2000 + i); }
        }
        // A non-positive count moves nothing at all.
        for (uint32 i = 0; i < n; i = i + 1) { dst[i] = -5; }
        b.download(dst, 0);
        for (uint32 i = 0; i < n; i = i + 1) {
            if (dst[i] != -5) { return (int32) (3000 + i); }
        }
        return 8484;
    }
}
)CJ";

} // namespace

// 10.1: a prefix upload moves exactly `count` elements — and moves no more.
TEST(XpuBufferPrefixTransferTests, prefixUploadMovesOnlyThePrefix) {
    auto jit = CajetaJit::compile(kPrefixUploadSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 4242) << "fail code " << r
                       << " (100+i: prefix missing; 1000+i: tail clobbered)";
}

// 10.1: the download twin, and the clamp on both ends.
TEST(XpuBufferPrefixTransferTests, prefixDownloadMovesOnlyThePrefixAndClamps) {
    auto jit = CajetaJit::compile(kPrefixDownloadSource, "test.M", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 8484) << "fail code " << r
                       << " (100+i: prefix missing; 1000+i: tail clobbered; "
                          "2000+i: oversized count not clamped; "
                          "3000+i: zero count still copied)";
}
