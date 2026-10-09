//
// xpu-kernel-adaptor 1.5.4.5 — a float64 software tile ACCUMULATES IN
// FLOAT64, on every backend that takes the portable tile.
//
// Both software tiers (the replicated tile and the distributed one) used to
// compute every floating-point mma in f32 and narrow the sum to the
// accumulator type afterwards: `compTy = fp ? float : acc`. For float64 that
// is a silent precision loss on every mma, replicated and distributed alike.
// Ewise.matmulF64's existing cross-check never saw it because its inputs are
// small integers, exact in f32.
//
// THE INPUT EXERCISES THE MANTISSA PAST F32. A is 1 + 2^-30 everywhere, B is
// 1.0, K = 16. In f64 every product is exact and the sum is 16 + 2^-26, also
// exact. Narrowed to f32, 1 + 2^-30 rounds to 1.0 before the multiply and the
// cell reads 16.0 exactly: the f32 path misses by 2^-26, so an exact f64
// comparison separates the two paths and nothing else does. The expected cell
// is checked bit for bit on purpose.
//
// cpu takes the replicated tile by default (CpuTarget does not distribute
// portable tiles); nvptx and amdgpu distribute theirs by default (1.5.4.1,
// 1.5.4.4). The one program runs on each backend whose device is here, so the
// test reaches both tiers on a box with a GPU and the replicated one anywhere.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// run() returns 0 when every cell equals 16 + 2^-26 exactly, or 1 + the
// index of the first wrong cell. A cell that reads exactly 16.0 is the f32
// accumulation; run() reports it as -1 so the failure names the defect.
const char* kSrc = R"CJ(
package test;
import cajeta.xpu.CooperativeMatrix;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
public class F {
    @Kernel
    public static void mm(KernelBuffer<float64> out, KernelBuffer<float64> a,
                          KernelBuffer<float64> b) {
        CooperativeMatrix<float64,16,16,0> ma;
        CooperativeMatrix<float64,16,16,1> mb;
        CooperativeMatrix<float64,16,16,2> mc;
        mc.splat(0.0);
        ma.load(a, 0, 0, 16);
        mb.load(b, 0, 0, 16);
        mc.mma(ma, mb);
        mc.store(out, 0, 0, 16);
    }
    public static int32 run() {
        float64[] ha = heap float64[256];
        float64[] hb = heap float64[256];
        float64[] ho = heap float64[256];
        int32 i = 0;
        while (i < 256) {
            ha[i] = 1.0 + 9.313225746154785e-10;
            hb[i] = 1.0;
            ho[i] = -777.0;
            i = i + 1;
        }
        KernelBuffer<float64> a = heap KernelBuffer<float64>(256);
        KernelBuffer<float64> b = heap KernelBuffer<float64>(256);
        KernelBuffer<float64> out = heap KernelBuffer<float64>(256);
        a.upload(ha);
        b.upload(hb);
        out.upload(ho);
        KernelStream s #= KernelStream.current();
        mm.launch(s, grid: [1], block: [32])(out, a, b);
        s.sync();
        out.download(ho);
        float64 want = 16.0 + 16.0 * 9.313225746154785e-10;
        i = 0;
        while (i < 256) {
            if (ho[i] != want) {
                if (ho[i] == 16.0) { return -1; }
                return i + 1;
            }
            i = i + 1;
        }
        return 0;
    }
}
)CJ";

struct BackendCase { const char* label; cajeta::xpu::Backend be; bool live; };

std::vector<BackendCase> backends() {
    return {
        {"cpu",    cajeta::xpu::Backend::Cpu,    true},
        {"nvptx",  cajeta::xpu::Backend::Nvptx,  cajeta::xpu::test::cudaAvailable()},
        {"amdgpu", cajeta::xpu::Backend::Amdgpu, cajeta::xpu::test::hipAvailable()},
    };
}

} // namespace

TEST(XpuCoopF64Accum, aFloat64TileAccumulatesInFloat64OnEveryBackend) {
    int checked = 0;
    for (const BackendCase& b : backends()) {
        if (!b.live) continue;
        SCOPED_TRACE(b.label);
        CajetaJit::Options o;
        o.xpuBackends = {b.be};
        testing::internal::CaptureStderr();
        auto jit = CajetaJit::compile(kSrc, "test.F", o);
        std::string err = testing::internal::GetCapturedStderr();
        ASSERT_NE(jit, nullptr) << err;
        if (err.find("[xpu-kernel-skipped] mm") != std::string::npos) {
            // Lowered nowhere on this backend (no assembler, or a refusal):
            // not a verdict on the accumulator. Say so and move on.
            std::fprintf(stderr, "[f64-accum] %s: mm did not register: %s\n",
                         b.label, err.c_str());
            continue;
        }
        auto fn = jit->lookup<int32_t (*)()>("run");
        ASSERT_NE(fn, nullptr);
        int32_t r = fn();
        EXPECT_EQ(r, 0)
            << (r == -1 ? "every cell reads exactly 16.0: the tile accumulated "
                          "in f32 and lost 1 + 2^-30 before the multiply"
                        : "first wrong cell (1-based) ")
            << (r > 0 ? std::to_string(r) : std::string()) << " on " << b.label;
        ++checked;
    }
    std::fprintf(stderr, "[f64-accum] checked on %d backend(s)\n", checked);
    EXPECT_GE(checked, 1);
}
