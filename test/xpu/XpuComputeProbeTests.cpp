//
// Cajeta XPU — proof-of-support compute probes (Stage-12 definition-of-done).
//
// These are *thin* probes — one per acceptance target — that prove the compute
// substrate can support building each downstream library, not the library
// itself. Each is a complete Cajeta program (allocate / upload / kernel.launch /
// sync / download / verify) run end to end through the frozen launch + kernel-arg
// FFI (the `__cajeta_xpu_launch` → `_v2` path, `__cajeta_xpu_register_module` +
// `__cajeta_xpu_register_kernel_params`); see docs/gpu/xpu/CajetaXPU-FFI.md. Each
// `run()` self-checks and returns 777 on success (else a fail code), so a green
// test is an executable proof the pattern works on the substrate.
//
// CPU is the bit-checked reference oracle. (On-device AMD/Vulkan gates ride the
// same sources; AMD in-process device JIT is currently blocked by a libamd_comgr
// symbol collision — see the project memory — so these gate on CPU here.)
//
// Targets covered:
//   1. Numerics (NumPy/SciPy) — broadcast-shaped element-wise + atomic reduction.
//   2. PyTorch — matmul + atomic grad-scatter (scatter-add with collisions).
//   3. Toffee/SPELA — fused forward + local-loss + weight update in one pass.
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

// --- Target 1: numerics — broadcast element-wise + reduction ----------------
//
// The two shapes every NumPy/SciPy kernel leans on: a *broadcast-shaped*
// element-wise op (per-row scale/bias broadcast across columns) and a *reduction*
// (atomic sum of the result into one accumulator). y[r*cols+c] =
// x[r*cols+c] * scale[r] + bias[r]; acc[0] = sum(y). Exercises multiple buffer
// args, broadcast indexing, float atomics (Stage 9), and the FFI marshalling.
const char* kNumericsBroadcastReduceSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class NumProbe {\n"
    "    @Kernel\n"
    "    public static void broadcastReduce(Buffer<float32> acc,\n"
    "                                       Buffer<float32> y, Buffer<float32> x,\n"
    "                                       Buffer<float32> scale,\n"
    "                                       Buffer<float32> bias,\n"
    "                                       uint32 cols, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            uint32 r = i / cols;\n"            // broadcast index (row)
    "            float32 v = x[i] * scale[r] + bias[r];\n"
    "            y[i] = v;\n"
    "            acc.atomicAdd(0, v);\n"            // reduction
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 8;\n"
    "        uint32 cols = 64;\n"
    "        uint32 n = rows * cols;\n"
    "        float32[] hx = heap float32[n];\n"
    "        float32[] hsc = heap float32[rows];\n"
    "        float32[] hbi = heap float32[rows];\n"
    "        for (uint32 r = 0; r < rows; r = r + 1) {\n"
    "            hsc[r] = (float32)(r) + 1.0f;\n"
    "            hbi[r] = (float32)(r) * 0.5f;\n"
    "            for (uint32 c = 0; c < cols; c = c + 1) {\n"
    "                hx[r * cols + c] = (float32)(c);\n"
    "            }\n"
    "        }\n"
    "        float32[] hy = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hy[i] = -1.0f; }\n"
    "        float32[] hacc = heap float32[1];\n"
    "        hacc[0] = 0.0f;\n"
    "        Buffer<float32> x = heap Buffer<float32>(n);\n"
    "        Buffer<float32> sc = heap Buffer<float32>(rows);\n"
    "        Buffer<float32> bi = heap Buffer<float32>(rows);\n"
    "        Buffer<float32> y = heap Buffer<float32>(n);\n"
    "        Buffer<float32> acc = heap Buffer<float32>(1);\n"
    "        x.upload(hx); sc.upload(hsc); bi.upload(hbi);\n"
    "        y.upload(hy); acc.upload(hacc);\n"
    "        Stream s = Stream.current();\n"
    "        broadcastReduce.launch(s, grid: [(n + 63) / 64], block: [64])"
                "(acc, y, x, sc, bi, cols, n);\n"
    "        s.sync();\n"
    "        y.download(hy);\n"
    "        acc.download(hacc);\n"
    "        float32 expSum = 0.0f;\n"
    "        for (uint32 r = 0; r < rows; r = r + 1) {\n"
    "            for (uint32 c = 0; c < cols; c = c + 1) {\n"
    "                float32 e = (float32)(c) * hsc[r] + hbi[r];\n"
    "                float32 dy = hy[r * cols + c] - e;\n"
    "                if (dy < -0.001f || dy > 0.001f) {\n"
    "                    return (int32)(100 + r * cols + c);\n"
    "                }\n"
    "                expSum = expSum + e;\n"
    "            }\n"
    "        }\n"
    "        float32 d = hacc[0] - expSum;\n"
    "        if (d < -1.0f || d > 1.0f) { return 2; }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuComputeProbeTests, numericsBroadcastReductionOnCpu) {
    auto jit = CajetaJit::compile(kNumericsBroadcastReduceSource, "test.NumProbe",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: element-wise mismatch; 2: reduction sum off)";
}

// --- Target 2: PyTorch — matmul + atomic grad-scatter -----------------------
//
// The two compute shapes a DL engine leans on beyond element-wise: a *matmul*
// (C = A·B, one thread per output element accumulating over K) and an *atomic
// scatter-add* (grad accumulation into bins with colliding indices — the
// embedding-grad / scatter pattern that *requires* atomics). Both verified vs a
// CPU recomputation of the same arithmetic.
const char* kTorchMatmulScatterSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class TorchProbe {\n"
    "    @Kernel\n"
    "    public static void matmul(Buffer<float32> C, Buffer<float32> A,\n"
    "                              Buffer<float32> B, uint32 M, uint32 K,\n"
    "                              uint32 N) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < M * N) {\n"
    "            uint32 row = i / N;\n"
    "            uint32 col = i - row * N;\n"
    "            float32 acc = 0.0f;\n"
    "            for (uint32 k = 0; k < K; k = k + 1) {\n"
    "                acc = acc + A[row * K + k] * B[k * N + col];\n"
    "            }\n"
    "            C[i] = acc;\n"
    "        }\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void scatterAdd(Buffer<float32> grad, Buffer<int32> idx,\n"
    "                                  Buffer<float32> val, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            grad.atomicAdd(idx[i], val[i]);\n"     // scatter-add into a bin
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 M = 8; uint32 K = 16; uint32 N = 8;\n"
    "        uint32 nA = M * K; uint32 nB = K * N; uint32 nC = M * N;\n"
    "        float32[] hA = heap float32[nA];\n"
    "        float32[] hB = heap float32[nB];\n"
    "        for (uint32 m = 0; m < M; m = m + 1) {\n"
    "            for (uint32 k = 0; k < K; k = k + 1) {\n"
    "                hA[m * K + k] = (float32)(m + k);\n"
    "            }\n"
    "        }\n"
    "        for (uint32 k = 0; k < K; k = k + 1) {\n"
    "            for (uint32 nn = 0; nn < N; nn = nn + 1) {\n"
    "                hB[k * N + nn] = (float32)(k + nn);\n"
    "            }\n"
    "        }\n"
    "        float32[] hC = heap float32[nC];\n"
    "        for (uint32 i = 0; i < nC; i = i + 1) { hC[i] = -1.0f; }\n"
    "        Buffer<float32> A = heap Buffer<float32>(nA);\n"
    "        Buffer<float32> B = heap Buffer<float32>(nB);\n"
    "        Buffer<float32> C = heap Buffer<float32>(nC);\n"
    "        A.upload(hA); B.upload(hB); C.upload(hC);\n"
    "        Stream s = Stream.current();\n"
    "        matmul.launch(s, grid: [(nC + 63) / 64], block: [64])"
                "(C, A, B, M, K, N);\n"
    "        s.sync();\n"
    "        C.download(hC);\n"
    "        for (uint32 m = 0; m < M; m = m + 1) {\n"
    "            for (uint32 nn = 0; nn < N; nn = nn + 1) {\n"
    "                float32 e = 0.0f;\n"
    "                for (uint32 k = 0; k < K; k = k + 1) {\n"
    "                    e = e + hA[m * K + k] * hB[k * N + nn];\n"
    "                }\n"
    "                float32 d = hC[m * N + nn] - e;\n"
    "                if (d < -0.01f || d > 0.01f) {\n"
    "                    return (int32)(100 + m * N + nn);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        uint32 ns = 256; uint32 P = 8;\n"
    "        int32[] hidx = heap int32[ns];\n"
    "        float32[] hval = heap float32[ns];\n"
    "        for (uint32 i = 0; i < ns; i = i + 1) {\n"
    "            hidx[i] = (int32)(i % P);\n"
    "            hval[i] = (float32)(i % 4) * 0.25f;\n"
    "        }\n"
    "        float32[] hg = heap float32[P];\n"
    "        for (uint32 b = 0; b < P; b = b + 1) { hg[b] = 0.0f; }\n"
    "        Buffer<int32> idx = heap Buffer<int32>(ns);\n"
    "        Buffer<float32> val = heap Buffer<float32>(ns);\n"
    "        Buffer<float32> grad = heap Buffer<float32>(P);\n"
    "        idx.upload(hidx); val.upload(hval); grad.upload(hg);\n"
    "        scatterAdd.launch(s, grid: [(ns + 63) / 64], block: [64])"
                "(grad, idx, val, ns);\n"
    "        s.sync();\n"
    "        grad.download(hg);\n"
    "        float32[] eg = heap float32[P];\n"
    "        for (uint32 b = 0; b < P; b = b + 1) { eg[b] = 0.0f; }\n"
    "        for (uint32 i = 0; i < ns; i = i + 1) {\n"
    "            eg[i % P] = eg[i % P] + (float32)(i % 4) * 0.25f;\n"
    "        }\n"
    "        for (uint32 b = 0; b < P; b = b + 1) {\n"
    "            float32 d = hg[b] - eg[b];\n"
    "            if (d < -0.01f || d > 0.01f) { return (int32)(200 + b); }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuComputeProbeTests, torchMatmulAtomicScatterOnCpu) {
    auto jit = CajetaJit::compile(kTorchMatmulScatterSource, "test.TorchProbe",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+idx: matmul mismatch; 200+bin: scatter-add mismatch)";
}

// --- Target 3: Toffee/SPELA — fused per-layer forward+loss+update -----------
//
// SPELA's defining compute demand over ordinary DL: fuse a layer's *forward* +
// *local loss* + *weight update* into ONE device pass (forward-only, no global
// backprop). One thread per output neuron j owns row j of W: it computes the
// pre-activation a = <W[j], x>, a local error vs a fixed target t[j] (the
// "symmetric vector" stand-in), writes the per-neuron loss, and applies a local
// delta-rule update to its own W row — all in a single launch, no inter-neuron
// sharing. The exact SPELA cosine math is the real framework's job; this proves
// the *fusion + per-neuron locality* the substrate must support.
const char* kSpelaFusedLayerSource =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n"
    "import cajeta.gpu.core.Thread;\n"
    "public class SpelaProbe {\n"
    "    @Kernel\n"
    "    public static void fusedLayer(Buffer<float32> W, Buffer<float32> x,\n"
    "                                  Buffer<float32> t, Buffer<float32> loss,\n"
    "                                  uint32 inDim, uint32 outDim, float32 lr) {\n"
    "        uint32 j = Thread.globalIdX();\n"
    "        if (j < outDim) {\n"
    "            float32 a = 0.0f;\n"
    "            for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "                a = a + W[j * inDim + i] * x[i];\n"
    "            }\n"
    "            float32 err = t[j] - a;\n"
    "            loss[j] = err * err;\n"
    "            for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "                W[j * inDim + i] = W[j * inDim + i] + lr * err * x[i];\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 inDim = 16; uint32 outDim = 8;\n"
    "        float32 lr = 0.1f;\n"
    "        uint32 nW = inDim * outDim;\n"
    "        float32[] hW = heap float32[nW];\n"
    "        float32[] hx = heap float32[inDim];\n"
    "        float32[] ht = heap float32[outDim];\n"
    "        for (uint32 j = 0; j < outDim; j = j + 1) {\n"
    "            for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "                hW[j * inDim + i] = 0.01f * (float32)((j + i) % 5);\n"
    "            }\n"
    "        }\n"
    "        for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "            hx[i] = 0.5f * (float32)(i % 3);\n"
    "        }\n"
    "        for (uint32 j = 0; j < outDim; j = j + 1) {\n"
    "            ht[j] = 0.25f * (float32)(j % 4);\n"
    "        }\n"
    "        float32[] hloss = heap float32[outDim];\n"
    "        for (uint32 j = 0; j < outDim; j = j + 1) { hloss[j] = -1.0f; }\n"
    "        Buffer<float32> W = heap Buffer<float32>(nW);\n"
    "        Buffer<float32> x = heap Buffer<float32>(inDim);\n"
    "        Buffer<float32> t = heap Buffer<float32>(outDim);\n"
    "        Buffer<float32> loss = heap Buffer<float32>(outDim);\n"
    "        W.upload(hW); x.upload(hx); t.upload(ht); loss.upload(hloss);\n"
    "        Stream s = Stream.current();\n"
    "        fusedLayer.launch(s, grid: [1], block: [outDim])"
                "(W, x, t, loss, inDim, outDim, lr);\n"
    "        s.sync();\n"
    "        W.download(hW);\n"
    "        loss.download(hloss);\n"
    "        for (uint32 j = 0; j < outDim; j = j + 1) {\n"
    "            float32 a = 0.0f;\n"
    "            for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "                a = a + 0.01f * (float32)((j + i) % 5) * hx[i];\n"
    "            }\n"
    "            float32 err = ht[j] - a;\n"
    "            float32 dl = hloss[j] - err * err;\n"
    "            if (dl < -0.001f || dl > 0.001f) { return (int32)(100 + j); }\n"
    "            for (uint32 i = 0; i < inDim; i = i + 1) {\n"
    "                float32 ew = 0.01f * (float32)((j + i) % 5) + lr * err * hx[i];\n"
    "                float32 dw = hW[j * inDim + i] - ew;\n"
    "                if (dw < -0.001f || dw > 0.001f) {\n"
    "                    return (int32)(200 + j * inDim + i);\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        return 777;\n"
    "    }\n"
    "}\n";

TEST(XpuComputeProbeTests, spelaFusedLayerOnCpu) {
    auto jit = CajetaJit::compile(kSpelaFusedLayerSource, "test.SpelaProbe",
                                  cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    int r = fn();
    EXPECT_EQ(r, 777)
        << "fail code " << r
        << " (100+j: loss mismatch; 200+idx: updated-weight mismatch)";
}

}  // namespace
