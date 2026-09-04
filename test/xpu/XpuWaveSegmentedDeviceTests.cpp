// Wave.reduceSumF32Segmented / reduceMaxF32Segmented — the block-scoped reduce
// that stays correct when the hardware wave is wider than the logical block.
//
// The whole point is a reduction over a SEGMENT of the wave rather than the
// whole wave. A test that only ran segment == width could not tell the
// segmented primitive from the plain reduce, so the load-bearing case here is
// segment < width: on this NVIDIA wave of 32, a segment of 16 must produce TWO
// independent sums. That is the exact same bounded-butterfly logic a segment of
// 32 within a wave of 64 needs on CDNA — which is why wave64 silicon is not
// required to validate the mechanism. segment < width is the thing under test,
// and a 32-wide wave supplies it.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cudaOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    return o;
}

// One 32-lane wave. Lane t loads in[t] = (t < 16) ? 1 : 100. A segment-16
// reduce gives lanes 0..15 the sum 16 and lanes 16..31 the sum 1600; a
// whole-wave (segment-32) reduce gives every lane 1616. run() returns
// out[0]*1_000_000 + out[16] so the caller can read both a low-half and a
// high-half lane from one value and tell the two apart:
//   correct segment-16 : 16 * 1e6 + 1600      = 16001600
//   a merge-to-32 bug  : 1616 * 1e6 + 1616     = 1616001616
const char* kSegSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class Seg {\n"
    "    @Kernel\n"
    "    public static void seg(KernelBuffer<float32> out, KernelBuffer<float32> in) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.reduceSumF32Segmented(in[t], 16);\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 32;\n"
    "        float32[] hin = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (i < 16) { hin[i] = 1.0f; } else { hin[i] = 100.0f; }\n"
    "        }\n"
    "        KernelBuffer<float32> din = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, n);\n"
    "        din.allocate();\n"
    "        dout.allocate();\n"
    "        din.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        seg.launch(s, grid: [1], block: [32])(dout, din);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[n];\n"
    "        dout.download(hout);\n"
    "        din.free();\n"
    "        dout.free();\n"
    "        return hout[0] * 1000000.0f + hout[16];\n"
    "    }\n"
    "}\n";

// The degenerate case: segment == width must equal the plain whole-wave reduce.
// Lane t loads in[t]=1; a segment-32 reduce on a 32-wave gives every lane 32.
const char* kSegFullSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "public class SegFull {\n"
    "    @Kernel\n"
    "    public static void segfull(KernelBuffer<float32> out) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Wave.reduceSumF32Segmented(1.0f + (float32) t - (float32) t, 32);\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 32;\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, n);\n"
    "        dout.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        segfull.launch(s, grid: [1], block: [32])(dout);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[n];\n"
    "        dout.download(hout);\n"
    "        dout.free();\n"
    "        return hout[0];\n"
    "    }\n"
    "}\n";

} // namespace

// segment < width FIRES: two independent 16-lane sums, not one 32-lane sum.
TEST(XpuWaveSegmentedDevice, segmentSmallerThanWaveReducesIndependently) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSegSource, "test.Seg", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    const float got = fn();
    // 16 * 1e6 + 1600. A whole-wave merge would give 1616 * 1e6 + 1616.
    EXPECT_FLOAT_EQ(got, 16001600.0f)
        << "segment-16 did not produce two independent sums on a 32-wide wave; "
           "got " << got << " (a merge-to-32 bug reads 1616001616)";
}

// segment == width DEGENERATES to the plain whole-wave reduce.
TEST(XpuWaveSegmentedDevice, segmentEqualToWaveEqualsWholeWaveReduce) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSegFullSource, "test.SegFull", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 32.0f)
        << "segment == wave width must equal reduceSumF32 over the whole wave";
}
