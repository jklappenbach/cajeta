// Kernel shapes shared by the CPU barrier-fission tests
// (XpuCpuBarrierFissionNoteTests, XpuCpuBarrierFissionLoopTests), so the
// accepted and declined examples are defined once and both files assert
// against the same source text.
//
// The fission (src/cajeta/xpu/cpu/CpuBarrierFission.cpp) splits a kernel at
// its barriers into regions and wraps each in a work-item loop; a
// block-uniform loop that contains a barrier is kept as an outer scalar
// scaffold whose header and latch run once per iteration. The shapes below
// cover the cases that scaffold has to get right (cpu-barrier-fission-loops
// spec 2.1–2.3):
//
//   FLAT_BARRIER   one straight-line barrier — always accepted.
//   TREE_REDUCE    the LDS tree reduce: a barrier ends the stride loop's body
//                  and the code after it IS the latch (`stride = stride / 2`),
//                  block-uniform — accepted (declined before the fix, because
//                  the region walk started a region at the latch).
//   FINAL_SUM2     the two-array final reduce: a per-work-item strided inner
//                  loop with no barrier, then the same tree loop — accepted.
//   TILED_GEMM     16x16 LDS tiles, two barriers per K-step, the latch is
//                  `k0 = k0 + 16` — accepted.
//   TAINTED_LATCH  the tree reduce with a per-work-item accumulation after
//                  the loop's last barrier, in the latch block — DECLINED by
//                  name; running it once per iteration would be wrong.
#pragma once
#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include <string>

namespace cajeta_fission_shapes {

inline std::string compileCpuCapturingStderr(const std::string& src) {
    cajeta_test::CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = cajeta_test::CajetaJit::compile(src, "test.D", o);
    std::string err = testing::internal::GetCapturedStderr();
    (void) jit;
    return err;
}

inline const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Workgroup;\n"
    "public final class D {\n";

inline const char* END = "}\n";

// One straight-line barrier: the shape the fission has always accepted.
inline const char* FLAT_BARRIER =
    "    @Kernel\n"
    "    public static void flat(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        Barrier.workgroup();\n"
    "        if (t == 0) { out[Workgroup.x()] = lds[0] + lds[1]; }\n"
    "    }\n";

// The LDS tree reduce (xpu-tile baseline `reduceSum`): the block after the
// loop's last barrier is the latch, holding only the uniform stride update.
inline const char* TREE_REDUCE =
    "    @Kernel\n"
    "    public static void tree(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        Barrier.workgroup();\n"
    "        uint32 stride = 128;\n"
    "        while (stride > 0) {\n"
    "            if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "            Barrier.workgroup();\n"
    "            stride = stride / 2;\n"
    "        }\n"
    "        if (t == 0) { out[Workgroup.x()] = lds[0]; }\n"
    "    }\n";

// The two-array final reduce (baseline `finalSum2`): a per-work-item strided
// loop with no barrier feeds the same tree loop.
inline const char* FINAL_SUM2 =
    "    @Kernel\n"
    "    public static void finalSum2(KernelBuffer<float32> out, KernelBuffer<float32> pa,\n"
    "                                 KernelBuffer<float32> pb, uint32 count) {\n"
    "        Shared<float32> la = shared float32[256];\n"
    "        Shared<float32> lb = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        float32 sa = 0.0f;\n"
    "        float32 sb = 0.0f;\n"
    "        uint32 i = t;\n"
    "        while (i < count) {\n"
    "            sa = sa + pa[i];\n"
    "            sb = sb + pb[i];\n"
    "            i = i + 256;\n"
    "        }\n"
    "        la[t] = sa;\n"
    "        lb[t] = sb;\n"
    "        Barrier.workgroup();\n"
    "        uint32 stride = 128;\n"
    "        while (stride > 0) {\n"
    "            if (t < stride) {\n"
    "                la[t] = la[t] + la[t + stride];\n"
    "                lb[t] = lb[t] + lb[t + stride];\n"
    "            }\n"
    "            Barrier.workgroup();\n"
    "            stride = stride / 2;\n"
    "        }\n"
    "        if (t == 0) {\n"
    "            out[0] = la[0];\n"
    "            out[1] = lb[0];\n"
    "        }\n"
    "    }\n";

// The LDS-tiled GEMM (baseline `matmulTiled`): two barriers per K-step, a
// per-work-item inner loop between them, the latch is `k0 = k0 + 16`.
inline const char* TILED_GEMM =
    "    @Kernel\n"
    "    public static void matmulTiled(KernelBuffer<float32> c, KernelBuffer<float32> a,\n"
    "                                   KernelBuffer<float32> b, uint32 n) {\n"
    "        Shared<float32> as = shared float32[256];\n"
    "        Shared<float32> bs = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 tr = t / 16;\n"
    "        uint32 tc = t - tr * 16;\n"
    "        uint32 row = Workgroup.y() * 16 + tr;\n"
    "        uint32 col = Workgroup.x() * 16 + tc;\n"
    "        float32 acc = 0.0f;\n"
    "        uint32 k0 = 0;\n"
    "        while (k0 < n) {\n"
    "            as[t] = a[row * n + k0 + tc];\n"
    "            bs[t] = b[(k0 + tr) * n + col];\n"
    "            Barrier.workgroup();\n"
    "            uint32 k = 0;\n"
    "            while (k < 16) {\n"
    "                acc = acc + as[tr * 16 + k] * bs[k * 16 + tc];\n"
    "                k = k + 1;\n"
    "            }\n"
    "            Barrier.workgroup();\n"
    "            k0 = k0 + 16;\n"
    "        }\n"
    "        c[row * n + col] = acc;\n"
    "    }\n";

// The tree reduce with per-work-item code after the loop's last barrier: the
// accumulation lands in the latch block, which the scaffold runs once per
// iteration with no work-item context. Declined by name, never run.
inline const char* TAINTED_LATCH =
    "    @Kernel\n"
    "    public static void treeLatch(KernelBuffer<float32> out, KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        Barrier.workgroup();\n"
    "        float32 acc = 0.0f;\n"
    "        uint32 stride = 128;\n"
    "        while (stride > 0) {\n"
    "            if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "            Barrier.workgroup();\n"
    "            acc = acc + lds[t];\n"
    "            stride = stride / 2;\n"
    "        }\n"
    "        if (t == 0) { out[Workgroup.x()] = lds[0] + acc; }\n"
    "    }\n";

} // namespace cajeta_fission_shapes
