// A barrier inside a block-uniform loop on the CPU backend
// (cpu-barrier-fission-loops spec 2.1–2.4, plan 1.1.1–1.1.4).
//
// The fission keeps a uniform loop that contains a barrier as an outer
// scalar scaffold: header and latch run once per iteration, the regions
// between barriers run per work-item. When the code after the loop's LAST
// barrier is the latch block itself — the LDS tree reduce's
// `stride = stride / 2`, the tiled GEMM's `k0 = k0 + 16` — the region walk
// used to start a region AT the latch, flow through the header and around
// the loop, and decline the kernel as "unstructured barrier control flow".
// Three baseline kernels of the xpu-tile family were pending on the CPU
// leg for exactly that (xpu-tile-scheduling report §5, 2026-09-06).
//
// Each accepted shape is run against a scalar reference on small exact
// integer data (every partial sum stays below 2^24, so f32 summation order
// cannot move the result). The declined shape puts per-work-item code in
// the latch: the build names it and the launch counts as a failure.
#include "XpuCpuBarrierFissionShapes.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using namespace cajeta_fission_shapes;

namespace {

std::unique_ptr<CajetaJit> compileCpu(const std::string& src, std::string* err) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    *err = testing::internal::GetCapturedStderr();
    return jit;
}

// in[i] = i mod 13; the tree's partial per block is summed on the host.
const char* RUN_TREE =
    "    public static int32 run(int32 n) {\n"
    "        int32 g = n / 256;\n"
    "        float32[] hin = heap float32[n];\n"
    "        int32 i = 0;\n"
    "        while (i < n) { hin[i] = (float32) (i - (i / 13) * 13); i = i + 1; }\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>((uint64) g);\n"
    "        in.upload(hin);\n"
    "        float32[] hout = heap float32[g];\n"
    "        out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        uint32 un = (uint32) n;\n"
    "        tree.launch(s, grid: [g], block: [256])(out, in, un);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        float32 sum = 0.0f;\n"
    "        int32 k = 0;\n"
    "        while (k < g) { sum = sum + hout[k]; k = k + 1; }\n"
    "        return (int32) sum;\n"
    "    }\n";

int32_t treeReference(int n) {
    int32_t s = 0;
    for (int i = 0; i < n; ++i) s += i % 13;
    return s;
}

// pa[i] = i mod 5, pb[i] = i mod 3 over `count` partials (count > 256 so the
// strided inner loop iterates); returns sumA * 10000 + sumB.
const char* RUN_FINAL =
    "    public static int32 run(int32 count) {\n"
    "        float32[] ha = heap float32[count];\n"
    "        float32[] hb = heap float32[count];\n"
    "        int32 i = 0;\n"
    "        while (i < count) {\n"
    "            ha[i] = (float32) (i - (i / 5) * 5);\n"
    "            hb[i] = (float32) (i - (i / 3) * 3);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<float32> pa = heap KernelBuffer<float32>((uint64) count);\n"
    "        KernelBuffer<float32> pb = heap KernelBuffer<float32>((uint64) count);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(2);\n"
    "        pa.upload(ha);\n"
    "        pb.upload(hb);\n"
    "        float32[] hout = heap float32[2];\n"
    "        out.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        uint32 uc = (uint32) count;\n"
    "        finalSum2.launch(s, grid: [1], block: [256])(out, pa, pb, uc);\n"
    "        s.sync();\n"
    "        out.download(hout);\n"
    "        return ((int32) hout[0]) * 10000 + (int32) hout[1];\n"
    "    }\n";

int32_t finalReference(int count) {
    int32_t a = 0, b = 0;
    for (int i = 0; i < count; ++i) { a += i % 5; b += i % 3; }
    return a * 10000 + b;
}

// a[i] = (i mod 7) - 3, b[i] = (i mod 5) - 2, n x n; returns the number of
// entries of c that differ from a scalar product computed on the host.
const char* RUN_GEMM =
    "    public static int32 run(int32 n) {\n"
    "        int32 nn = n * n;\n"
    "        float32[] ha = heap float32[nn];\n"
    "        float32[] hb = heap float32[nn];\n"
    "        float32[] hc = heap float32[nn];\n"
    "        int32 i = 0;\n"
    "        while (i < nn) {\n"
    "            ha[i] = (float32) ((i - (i / 7) * 7) - 3);\n"
    "            hb[i] = (float32) ((i - (i / 5) * 5) - 2);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        KernelBuffer<float32> a = heap KernelBuffer<float32>((uint64) nn);\n"
    "        KernelBuffer<float32> b = heap KernelBuffer<float32>((uint64) nn);\n"
    "        KernelBuffer<float32> c = heap KernelBuffer<float32>((uint64) nn);\n"
    "        a.upload(ha);\n"
    "        b.upload(hb);\n"
    "        c.upload(hc);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int32 g = n / 16;\n"
    "        uint32 un = (uint32) n;\n"
    "        matmulTiled.launch(s, grid: [g, g], block: [256])(c, a, b, un);\n"
    "        s.sync();\n"
    "        c.download(hc);\n"
    "        int32 bad = 0;\n"
    "        int32 r = 0;\n"
    "        while (r < n) {\n"
    "            int32 col = 0;\n"
    "            while (col < n) {\n"
    "                float32 ref = 0.0f;\n"
    "                int32 k = 0;\n"
    "                while (k < n) { ref = ref + ha[r * n + k] * hb[k * n + col]; k = k + 1; }\n"
    "                if (hc[r * n + col] != ref) { bad = bad + 1; }\n"
    "                col = col + 1;\n"
    "            }\n"
    "            r = r + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n";

// Launch the declined kernel; return how far Device.launchFailures() moved.
const char* RUN_LATCH_LAUNCH =
    "    public static int32 run() {\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>(4);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        int64 f0 = Device.launchFailures();\n"
    "        uint32 n = 256;\n"
    "        treeLatch.launch(s, grid: [1], block: [256])(out, in, n);\n"
    "        s.sync();\n"
    "        return (int32) (Device.launchFailures() - f0);\n"
    "    }\n";

} // namespace

// 1.1.1 — spec 2.1: the stride-loop tree reduce lowers and matches.
TEST(XpuCpuBarrierFissionLoop, treeReduceMatchesScalarReference) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + TREE_REDUCE + RUN_TREE + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos)
        << "a uniform latch after the loop's last barrier is scaffold, not a region:\n" << err;
    auto fn = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(256), treeReference(256));
    EXPECT_EQ(fn(4096), treeReference(4096));
}

// 1.1.2 — spec 2.2: a per-work-item strided loop (no barrier) before the
// barrier loop stays inside its region; both sums match.
TEST(XpuCpuBarrierFissionLoop, finalReduceMatchesBothSums) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + FINAL_SUM2 + RUN_FINAL + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(1000), finalReference(1000));
}

// 1.1.3 — spec 2.1: two barriers per K-step, the latch is the K update.
TEST(XpuCpuBarrierFissionLoop, tiledGemmMatchesScalarProductExactly) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + TILED_GEMM + RUN_GEMM + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    auto fn = jit->lookup<int32_t (*)(int32_t)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(32), 0) << "entries of c that differ from the scalar 32x32 product";
}

// 1.1.4 — spec 2.3 + 2.4: per-work-item code in the latch after the loop's
// last barrier is declined by name, and the launch counts as a failure.
TEST(XpuCpuBarrierFissionLoop, perWorkItemLatchIsDeclinedByName) {
    std::string err;
    auto jit = compileCpu(std::string(PRE) + TAINTED_LATCH + RUN_LATCH_LAUNCH + END, &err);
    ASSERT_NE(jit, nullptr) << err;
    EXPECT_NE(err.find("[xpu-kernel-skipped] treeLatch"), std::string::npos)
        << "the note must name the kernel:\n" << err;
    EXPECT_NE(err.find("barrier fission"), std::string::npos) << err;
    EXPECT_NE(err.find("per-work-item code in the latch of a barrier loop"), std::string::npos)
        << "the reason must name the latch, not the generic unstructured-flow message:\n" << err;
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    testing::internal::CaptureStderr();
    int32_t delta = fn();
    std::string runErr = testing::internal::GetCapturedStderr();
    EXPECT_EQ(delta, 1) << "one declined launch moves Device.launchFailures() by one:\n" << runErr;
}
