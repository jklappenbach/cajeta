// Every building block a matrix-core kernel needs has a conformance kernel in
// the corpus (xpu-kernel-independence 4.9.1.2, spec §4.1): fragment load,
// store and multiply; per-element fragment access with the lane-to-element
// map; integer multiply-add over packed bytes; byte permute; wave shuffle,
// reduce and ballot; conversions between widths; async copy; a swizzled
// shared tile.
//
// One program holds a kernel per block and a host `run` that launches each.
// The launches are recorded on a backend (CAJETA_XPU_RECORD) and replayed
// through the reference interpreter by the corpus runner, so every block has
// defined reference semantics and every backend that lowers it agrees with
// them. A kernel the interpreter cannot define comes back `refused`, which
// fails here: the corpus is the place a building block proves itself.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"
#include "CpuKernelHarness.h"
#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/reference/Conformance.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ref = cajeta::xpu::reference;
using cajeta_test::CajetaJit;
using cajeta_test::findKernel;

namespace {

const char* kKernels[] = {"bbFragment", "bbElements", "bbDot", "bbPermute",
                          "bbWave", "bbConvert", "bbAsyncCopy", "bbSwizzled"};

const char* kSource = R"CJ(
package test;
import cajeta.xpu.AsyncCopy;
import cajeta.xpu.Barrier;
import cajeta.xpu.Bits;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Swizzled;
import cajeta.xpu.Wave;
public class M {
    // Fragment load, multiply and store: C[16x16] = 1 + A[16x32] . B[32x16].
    @Kernel
    public static void bbFragment(KernelBuffer<float32> c, KernelBuffer<float32> a,
                                  KernelBuffer<float32> b) {
        CooperativeMatrix<float32,16,16,0> ta;
        CooperativeMatrix<float32,16,16,1> tb;
        CooperativeMatrix<float32,16,16,2> acc;
        acc.splat(1.0f);
        for (uint32 kt = 0; kt < 2; kt = kt + 1) {
            ta.load(a, kt * 16, 0, 32);
            tb.load(b, kt * 16, 1, 32);
            acc.mma(ta, tb);
        }
        acc.store(c, 0, 0, 16);
    }
    // Per-element access with the lane-to-element map: y = 2a + (r*16 + c).
    @Kernel
    public static void bbElements(KernelBuffer<float32> y, KernelBuffer<float32> a) {
        CooperativeMatrix<float32,16,16,2> acc;
        acc.load(a, 0, 0, 16);
        uint32 n = acc.elements();
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 r = acc.row(i);
            uint32 c = acc.col(i);
            acc.set(i, acc.get(i) * 2.0f + (float32) (r * 16 + c));
        }
        acc.store(y, 0, 0, 16);
    }
    // Integer multiply-add over packed bytes.
    @Kernel
    public static void bbDot(KernelBuffer<int32> out, KernelBuffer<uint8> w,
                             KernelBuffer<int8> a, KernelBuffer<int32> zero) {
        uint32 g = KernelThread.globalIdX();
        Vector<uint8,16> wv = w.vload<16>((int64) g * 16L);
        Vector<int8,16> av = a.vload<16>((int64) g * 16L);
        Vector<int32,4> acc = zero.vload<4>((int64) g * 4L);
        out.vstore((int64) g * 4L, wv.dotAccum(av, acc));
    }
    // Byte permute: byte i of the result is byte (nibble i of the selector)
    // of the eight bytes {lo, hi}, low nibble first.
    @Kernel
    public static void bbPermute(KernelBuffer<uint32> out, KernelBuffer<uint32> x,
                                 KernelBuffer<uint32> y) {
        uint32 g = KernelThread.globalIdX();
        out[g * 2] = Bits.permute(x[g], y[g], 0x7250);
        out[g * 2 + 1] = Bits.permute(x[g], y[g], x[g] & 0x7777);
    }
    // Wave shuffle in its four forms, reduce and ballot.
    @Kernel
    public static void bbWave(KernelBuffer<uint32> out, KernelBuffer<uint32> x,
                              KernelBuffer<float32> outf, KernelBuffer<float32> xf) {
        uint32 g = KernelThread.globalIdX();
        uint32 lane = Wave.laneId();
        uint32 w = Wave.width();
        uint32 v = x[g];
        out[g * 6] = Wave.shuffleSync(v, (lane + 1) % w);
        out[g * 6 + 1] = Wave.shuffleXorSync(v, 1);
        out[g * 6 + 2] = Wave.shuffleUpSync(v, 2);
        out[g * 6 + 3] = Wave.shuffleDownSync(v, 3);
        out[g * 6 + 4] = Wave.reduceSum(v);
        out[g * 6 + 5] = (uint32) Wave.ballotSync(v > 100);
        outf[g] = Wave.shuffleSyncF32(xf[g], (lane + 2) % w);
    }
    // Conversions between widths: the vector forms to and from the 16-bit
    // floats, and scalar casts across the integer and float widths.
    @Kernel
    public static void bbConvert(KernelBuffer<float32> x, KernelBuffer<float16> h,
                                 KernelBuffer<bfloat16> bf, KernelBuffer<float32> back,
                                 KernelBuffer<int64> wide, KernelBuffer<int8> narrow,
                                 KernelBuffer<float64> dbl) {
        uint32 g = KernelThread.globalIdX();
        Vector<float32,4> v = x.vload<4>((int64) g * 4L);
        Vector<float16,4> hv = v.toF16();
        Vector<bfloat16,4> bv = v.toBF16();
        h.vstore((int64) g * 4L, hv);
        bf.vstore((int64) g * 4L, bv);
        back.vstore((int64) g * 4L, bv.toF32());
        float32 s = x[g];
        wide[g] = (int64) s * 1000L;
        narrow[g] = (int8) (int32) s;
        dbl[g] = (float64) s * 0.5;
    }
    // Async copy of a global range into a shared tile.
    @Kernel
    public static void bbAsyncCopy(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        Shared<uint32> tile = shared uint32[256];
        AsyncCopy.copy(tile, 0, in, 0, 256);
        AsyncCopy.commit();
        AsyncCopy.wait(0);
        Barrier.workgroup();
        uint32 g = KernelThread.globalIdX();
        for (uint32 k = 0; k < 8; k = k + 1) {
            out[g * 8 + k] = tile[255 - (g * 8 + k)] + tile[g];
        }
    }
    // A swizzled shared tile, written by rows and read transposed.
    @Kernel
    public static void bbSwizzled(KernelBuffer<uint32> out, KernelBuffer<uint32> in) {
        Swizzled<uint32, 16> tile = shared uint32[256];
        uint32 g = KernelThread.globalIdX();
        for (uint32 k = 0; k < 8; k = k + 1) { tile[g + 32 * k] = in[g + 32 * k]; }
        Barrier.workgroup();
        for (uint32 k = 0; k < 8; k = k + 1) { out[g * 8 + k] = tile[k * 32 + ((g + 5) % 32)]; }
    }
    public static int32 run() {
        float32[] hf = heap float32[512];
        for (int32 i = 0; i < 512; i = i + 1) { hf[i] = (float32) ((i * 7) % 11 - 5) * 1.25f; }
        uint32[] hu = heap uint32[256];
        for (int32 i = 0; i < 256; i = i + 1) { hu[i] = (uint32) (i * 2654435761L + 97L); }
        uint8[] hw = heap uint8[512];
        int8[] ha = heap int8[512];
        for (int32 i = 0; i < 512; i = i + 1) {
            hw[i] = (uint8) (i * 29 + 3);
            ha[i] = (int8) (0 - (i % 100));
        }
        int32[] hz = heap int32[128];
        KernelBuffer<float32> fa = heap KernelBuffer<float32>(512);
        KernelBuffer<float32> fb = heap KernelBuffer<float32>(512);
        KernelBuffer<float32> fc = heap KernelBuffer<float32>(256);
        fa.upload(hf);
        fb.upload(hf);
        fc.upload(hf);
        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(512);
        KernelBuffer<int8> a = heap KernelBuffer<int8>(512);
        KernelBuffer<int32> zero = heap KernelBuffer<int32>(128);
        KernelBuffer<int32> dot = heap KernelBuffer<int32>(128);
        w.upload(hw);
        a.upload(ha);
        zero.upload(hz);
        dot.upload(hz);
        KernelBuffer<uint32> ux = heap KernelBuffer<uint32>(256);
        KernelBuffer<uint32> uy = heap KernelBuffer<uint32>(256);
        KernelBuffer<uint32> uo = heap KernelBuffer<uint32>(256);
        ux.upload(hu);
        uy.upload(hu);
        uo.upload(hu);
        KernelBuffer<float16> h16 = heap KernelBuffer<float16>(128);
        KernelBuffer<bfloat16> b16 = heap KernelBuffer<bfloat16>(128);
        KernelBuffer<float32> back = heap KernelBuffer<float32>(128);
        KernelBuffer<int64> wide = heap KernelBuffer<int64>(32);
        KernelBuffer<int8> narrow = heap KernelBuffer<int8>(32);
        KernelBuffer<float64> dbl = heap KernelBuffer<float64>(32);
        KernelStream s #= KernelStream.current();
        bbFragment.launch(s, grid: [1], block: [32])(fc, fa, fb);
        s.sync();
        bbElements.launch(s, grid: [1], block: [32])(fc, fa);
        s.sync();
        bbDot.launch(s, grid: [1], block: [32])(dot, w, a, zero);
        s.sync();
        bbPermute.launch(s, grid: [1], block: [32])(uo, ux, uy);
        s.sync();
        bbWave.launch(s, grid: [1], block: [32])(uo, ux, back, fa);
        s.sync();
        bbConvert.launch(s, grid: [1], block: [32])(fa, h16, b16, back, wide, narrow, dbl);
        s.sync();
        bbAsyncCopy.launch(s, grid: [1], block: [32])(uo, ux);
        s.sync();
        bbSwizzled.launch(s, grid: [1], block: [32])(uo, uy);
        s.sync();
        return 1;
    }
}
)CJ";

// Compile and run `run` on `be` with every launch recorded into `dir`.
void recordOn(cajeta::xpu::Backend be, const fs::path& dir) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    setenv("CAJETA_XPU_RECORD", dir.string().c_str(), 1);
    int32_t r = fn();
    unsetenv("CAJETA_XPU_RECORD");
    EXPECT_EQ(r, 1);
}

// Every launch replays and passes, and every block's kernel was recorded.
void everyBlockPassesOn(cajeta::xpu::Backend be, const char* backend) {
    fs::path dir = fs::temp_directory_path() / ("cajeta-bb-corpus-" + std::string(backend));
    fs::remove_all(dir);
    fs::create_directories(dir);
    recordOn(be, dir);
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    std::vector<cajeta::MethodPtr> kernels;
    for (const char* n : kKernels)
        if (auto k = findKernel(module, "test.M", n)) kernels.push_back(k);
    ASSERT_EQ(kernels.size(), 8u);
    ref::CorpusRun run = ref::runCorpus(kernels, dir.string(), "");
    std::map<std::string, std::string> outcome;
    for (auto& r : run.results) {
        for (const char* n : kKernels)
            if (r.kernel.find(n) != std::string::npos) outcome[n] = r.outcome + " " + r.detail;
        EXPECT_EQ(r.outcome, "pass") << r.kernel << " on " << r.backend << ": " << r.detail;
    }
    for (const char* n : kKernels)
        EXPECT_TRUE(outcome.count(n)) << n << " has no recording on " << backend;
    fs::remove_all(dir);
}

} // namespace

TEST(XpuBuildingBlockCorpus, everyBuildingBlockKernelPassesOnCpu) {
    everyBlockPassesOn(cajeta::xpu::Backend::Cpu, "cpu");
}

TEST(XpuBuildingBlockCorpus, everyBuildingBlockKernelPassesOnNvptx) {
    if (!cajeta::xpu::test::cudaAvailable()) GTEST_SKIP() << "no CUDA device";
    everyBlockPassesOn(cajeta::xpu::Backend::Nvptx, "nvptx");
}
