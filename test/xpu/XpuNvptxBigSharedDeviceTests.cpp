//
// The over-cap shared tile, on the real device.
//
// XpuNvptxBigSharedTests says ptxas accepts the relocated module. That is not
// the same as the tiles being in the right place: a relocation that packed
// every tile at offset 0 would assemble perfectly and return one tile's data
// for both. What catches that is running it and reading the answers back.
//
// SHAPE. Two `float16` tiles totalling 55296 bytes — `q4kF16CoopN256Kernel`'s
// exact staging, 128*72 and 256*72 — which is 6144 bytes over the 49152-byte
// PTX static cap. Each tile is filled with a value that identifies BOTH the
// tile and the index, so aliasing is not a subtle numeric drift:
//
//     sa[i] = i + 1            1 .. 9216
//     sb[j] = -(j + 1)         -1 .. -18432
//
// If both tiles were relocated to the same offset, sb's fill would land on
// top of sa's and the first sum would come back negative. If they overlapped
// partially, the overlap region would carry sb's values and the sum would be
// short by a readable amount. The reduction is in int32, not f32: the tile
// sums are 10 619 136 and 42 471 936, both past 2^24 where f32 stops counting
// by ones, so a float accumulator would disagree with the reference for
// reasons that have nothing to do with shared memory.
//
// The launch is the other half of the mechanism: the extern block carries no
// size in PTX, so the runtime has to supply one AND take the driver's
// per-function opt-in above 48 KB (cuFuncSetAttribute). Neither is visible in
// the PTX, and a kernel that launched without them would read zeros.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cudaOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Nvptx};
    return o;
}

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "public class Big {\n"
    "    @Kernel\n"
    "    public static void twoTiles(KernelBuffer<int32> out) {\n"
    "        Shared<int32> sa = shared int32[64 * 72];\n"
    "        Shared<int32> sb = shared int32[128 * 72];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = t;\n"
    "        while (i < 4608) { sa[i] = (int32) i + 1; i = i + 256; }\n"
    "        uint32 j = t;\n"
    "        while (j < 9216) { sb[j] = 0 - ((int32) j + 1); j = j + 256; }\n"
    "        Barrier.workgroup();\n"
    "        int32 pa = 0;\n"
    "        uint32 k = t;\n"
    "        while (k < 4608) { pa = pa + sa[k]; k = k + 256; }\n"
    "        int32 pb = 0;\n"
    "        uint32 m = t;\n"
    "        while (m < 9216) { pb = pb + sb[m]; m = m + 256; }\n"
    "        out[(int64) t] = pa;\n"
    "        out[(int64) (t + 256)] = pb;\n"
    "    }\n"
    "    public static int64 run() {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 512);\n"
    "        out.allocate();\n"
    "        int32[] h = heap int32[512];\n"
    "        int32 z = 0;\n"
    "        while (z < 512) { h[z] = 0; z = z + 1; }\n"
    "        out.upload(h);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        twoTiles.launch(s, grid: [1], block: [256])(out);\n"
    "        s.sync();\n"
    "        out.download(h);\n"
    "        out.free();\n"
    "        int64 sa = 0;\n"
    "        int64 sb = 0;\n"
    "        int32 q = 0;\n"
    "        while (q < 256) {\n"
    "            sa = sa + (int64) h[q];\n"
    "            sb = sb + (int64) h[q + 256];\n"
    "            q = q + 1;\n"
    "        }\n"
    "        return sa * 1000000000L + (0L - sb);\n"
    "    }\n"
    "}\n";

} // namespace

// 64*72 + 128*72 int32 = 18432 + 36864 = 55296 bytes, 6144 over the cap.
TEST(XpuNvptxBigSharedDeviceTests, twoRelocatedTilesKeepTheirOwnBytes) {
    CAJETA_SKIP_IF_NO_CUDA();
    auto jit = CajetaJit::compile(kSource, "test.Big", cudaOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);

    // sum(1..4608) = 10 619 136; sum(1..9216) = 42 471 936, returned positive.
    const int64_t wantA = (int64_t) 4608 * 4609 / 2;
    const int64_t wantB = (int64_t) 9216 * 9217 / 2;
    int64_t got = fn();
    int64_t gotA = got / 1000000000L;
    int64_t gotB = got % 1000000000L;
    EXPECT_EQ(gotA, wantA)
        << "tile A read back wrong; 0 means the launch never sized the extern "
           "block, a negative-derived value means B was written over A";
    EXPECT_EQ(gotB, wantB) << "tile B read back wrong";
}
