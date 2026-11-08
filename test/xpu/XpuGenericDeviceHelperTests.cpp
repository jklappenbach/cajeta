// cajeta-ml-gpu 14.2.2: a generic @Device helper with a type-parameter formal, called from a
// generic kernel, is instantiated with the kernel. On v0.53.0 the kernel's own E formals and
// E locals lowered, but `beats<E>(E a, int32 i, E b, int32 j)` called from `argmax<E>` was
// XPU-N01 "kernel parameter type 'E'" on every backend (measured 2026-11-07).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public final class D {\n"
    "    /** True when (a, i) beats (b, j): the larger value, the lower index on a tie. */\n"
    "    @Device\n"
    "    static boolean beats<E>(E a, int32 i, E b, int32 j) {\n"
    "        if (a > b) { return true; }\n"
    "        if (a < b) { return false; }\n"
    "        return i < j;\n"
    "    }\n"
    "    /** idx[r] = the first index of the maximum of row r (cols elements), one lane per row. */\n"
    "    @Kernel\n"
    "    public static void argmax<E>(KernelBuffer<int32> idx, KernelBuffer<E> in, uint32 rows, uint32 cols) {\n"
    "        uint32 r = KernelThread.globalIdX();\n"
    "        if (r < rows) {\n"
    "            E best = in[r * cols];\n"
    "            int32 at = 0;\n"
    "            uint32 c = 1;\n"
    "            while (c < cols) {\n"
    "                E v = in[r * cols + c];\n"
    "                if (D.beats<E>(v, (int32) c, best, at)) { best = v; at = (int32) c; }\n"
    "                c = c + 1;\n"
    "            }\n"
    "            idx[r] = at;\n"
    "        }\n"
    "    }\n"
    "    public static void rows<E>(KernelBuffer<int32> idx, KernelBuffer<E> in, int64 rows, int64 cols) {\n"
    "        uint32 ur = (uint32) rows;\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        argmax.launch(s, grid: [(ur + 63) / 64], block: [64])(idx, in, ur, (uint32) cols);\n"
    "        s.sync();\n"
    "    }\n"
    "    /** Mismatches of the device argmax against the host over 7 rows of 50, ties included: 0 when right. */\n"
    "    public static int32 run() {\n"
    "        int64 rows = 7L;\n"
    "        int64 cols = 50L;\n"
    "        float32[] h = heap float32[rows * cols];\n"
    "        int32 i = 0;\n"
    "        while (i < 350) { h[i] = (float32) ((i * 7919) % 13); i = i + 1; }\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) (rows * cols));\n"
    "        KernelBuffer<int32> idx = heap KernelBuffer<int32>((uint64) rows);\n"
    "        in.upload(h);\n"
    "        D.rows<float32>(idx, in, rows, cols);\n"
    "        int32[] got = heap int32[rows];\n"
    "        idx.download(got);\n"
    "        int32 bad = 0;\n"
    "        int32 r = 0;\n"
    "        while (r < 7) {\n"
    "            int32 at = 0;\n"
    "            int32 c = 1;\n"
    "            while (c < 50) { if (h[r * 50 + c] > h[r * 50 + at]) { at = c; } c = c + 1; }\n"
    "            if (got[r] != at) { bad = bad + 1; }\n"
    "            r = r + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

TEST(XpuGenericDeviceHelper, aGenericHelperWithTypeParameterFormalsLowersWithItsKernel) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kSource, "test.D", o);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0) << err;
}
