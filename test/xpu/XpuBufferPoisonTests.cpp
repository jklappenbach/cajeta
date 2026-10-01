//
// CAJETA_XPU_POISON — a fresh device buffer is filled with 0xFF bytes before
// it is handed out, so a kernel that reads a region nothing wrote answers NaN
// deterministically instead of "whatever the allocator left there".
//
// Found 2026-09-30 in cajeta-llm: ResidentMoeDecodeTest's capped arm turned
// red only when a trivial, host-only test had run before it. The float
// buffer it read before writing landed on a recycled malloc chunk holding
// NaN-patterned bytes; alone, it landed on a zero page. MALLOC_PERTURB_
// cannot reproduce that (its allocation fill is the complement of the byte,
// never 0xFF), so the runtime gained a fill of its own, on every backend.
//
// Both arms, on the cpu backend (the one every box has): the lever FIRES
// (a never-written buffer downloads as NaN in every element) and it DOES
// NOT clobber written data (an uploaded buffer reads back what was
// uploaded, lever on). CAJETA_XPU_POISON_MAX below the buffer's byte count
// leaves it unfilled, which is how a culprit buffer is found by its size.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cmath>
#include <cstdlib>
#include <string>
#if defined(_WIN32)
static inline int setenv(const char* k, const char* v, int) { return _putenv_s(k, v); }
static inline int unsetenv(const char* k) { return _putenv_s(k, ""); }
#endif

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOnly() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// A 256-element int32 buffer that is never written: `fresh` downloads it as
// allocated and reports how many elements read -1 (0xFFFFFFFF, the NaN
// pattern as a float). Counted as integers so no fast-math folding of
// `x != x` can hide the fill. `written` uploads a ramp of floats first and
// reports the sum read back (256 * 255 / 2 = 32640).
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Poison {\n"
    // A kernel the program never launches: a module with no kernel bundles
    // no backend, and a buffer then has no device to live on.
    "    @Kernel\n"
    "    public static void touch(KernelBuffer<int32> y, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = (int32) i; }\n"
    "    }\n"
    "    public static int32 fresh() {\n"
    "        uint32 n = 256;\n"
    "        int32[] h = heap int32[n];\n"
    "        KernelBuffer<int32> d = heap KernelBuffer<int32>((uint64) n);\n"
    "        d.download(h);\n"
    "        int32 filled = 0;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (h[i] == -1) { filled = filled + 1; }\n"
    "        }\n"
    "        return filled;\n"
    "    }\n"
    "    public static int32 recycled() {\n"
    "        uint32 n = 256;\n"
    "        int32[] z = heap int32[n];\n"
    "        KernelBuffer<int32> a = heap KernelBuffer<int32>((uint64) n);\n"
    "        a.upload(z);\n"
    "        a.free();\n"
    "        KernelBuffer<int32> b = heap KernelBuffer<int32>((uint64) n);\n"
    "        int32[] h = heap int32[n];\n"
    "        b.download(h);\n"
    "        int32 filled = 0;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (h[i] == -1) { filled = filled + 1; }\n"
    "        }\n"
    "        return filled;\n"
    "    }\n"
    "    public static float32 written() {\n"
    "        uint32 n = 256;\n"
    "        float32[] h = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { h[i] = (float32) i; }\n"
    "        KernelBuffer<float32> d = heap KernelBuffer<float32>((uint64) n);\n"
    "        d.upload(h);\n"
    "        float32[] back = heap float32[n];\n"
    "        d.download(back);\n"
    "        float32 sum = 0.0f;\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { sum = sum + back[i]; }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

struct EnvScope {
    EnvScope(const char* k, const char* v) : key(k) { setenv(k, v, 1); }
    ~EnvScope() { unsetenv(key); }
    const char* key;
};

}  // namespace

TEST(XpuBufferPoisonTests, aFreshBufferIsAllOnesWithTheLeverOn) {
    EnvScope on("CAJETA_XPU_POISON", "1");
    auto jit = CajetaJit::compile(kSource, "test.Poison", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("fresh");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 256);
}

TEST(XpuBufferPoisonTests, writtenDataSurvivesTheLever) {
    EnvScope on("CAJETA_XPU_POISON", "1");
    auto jit = CajetaJit::compile(kSource, "test.Poison", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("written");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 32640.0f);
}

// The size bound: 256 floats = 1024 bytes, and a cap of 1000 bytes leaves
// that buffer unfilled. The arm makes "unfilled" observable the way the
// llm defect was: a zero-filled buffer of the same size is freed first, so
// the second allocation lands on the recycled chunk (glibc's tcache hands
// a same-size chunk straight back) and reads the zeros the lever did not
// overwrite. The companion arm, with no cap, reads 256 NaN from the same
// recycled chunk.
TEST(XpuBufferPoisonTests, aSizeBoundLeavesOtherBuffersAlone) {
    EnvScope on("CAJETA_XPU_POISON", "1");
    EnvScope cap("CAJETA_XPU_POISON_MAX", "1000");
    auto jit = CajetaJit::compile(kSource, "test.Poison", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("recycled");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

TEST(XpuBufferPoisonTests, aRecycledChunkIsFilledToo) {
    EnvScope on("CAJETA_XPU_POISON", "1");
    auto jit = CajetaJit::compile(kSource, "test.Poison", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("recycled");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 256);
}
