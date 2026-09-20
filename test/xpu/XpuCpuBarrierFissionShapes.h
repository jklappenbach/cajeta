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
//   GUARDED_LOOP   the tree loop under `if (wg < groups) { … }` with every
//                  barrier inside the loop (the WMMA GEMM kernels' shape) —
//                  ACCEPTED since the work-item activity mask landed. It was
//                  declined for a year of commits, and the decline was right
//                  at the time: accepting it regioned the `if`'s join block
//                  twice and miscompiled (RAGreedy SIGSEGV, 2026-09-06).
//                  What changed is that the bypass edge now gets its own
//                  `ret` (so the join is regioned once) and a work-item that
//                  took it is masked out of every later region.
//   EARLY_RETURN   the same reduce behind `if (wg >= groups) { return; }` —
//                  cajeta-llm's `qkNormRowsF32` shape. ACCEPTED, same
//                  machinery: the guard is the bypass and the mask is what
//                  keeps the returned work-items out of the tail.
//   UNIFORM_GUARD_JOIN         a barrier behind a KERNEL-PARAMETER guard with
//                  real work after the join (qkPrepKernel's shape). ACCEPTED:
//                  the condition is not tid-tainted, so the whole workgroup
//                  takes the branch together and it is scaffold.
//   UNIFORM_GUARD_BOTH_RETURN  the same guard with both arms ending the
//                  kernel (iq3xxsQ8WaveGateUpGluKernel's shape) — no join
//                  block exists, so the split must terminate cleanly.
//   DIVERGENT_JOIN a barrier in ONE arm of an if/else with real work after
//                  the join — still DECLINED by name. The mask handles a
//                  work-item that LEAVES; it does not handle one that skips
//                  a barrier and comes back, and accepting that would be a
//                  miscompile rather than a slow path.
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

// The tree loop under a guard, every barrier inside the loop — the shape of
// cajeta-llm's WMMA GEMM kernels (`if (t0 < rows && i0 < outDim) { while (b
// < blocksPerRow) { … Barrier.workgroup(); … } }`). The barriers post-
// dominate the loop's in-loop successor, so the per-barrier check passes; it
// is the LOOP that not every work-item path enters. Declined by name.
inline const char* GUARDED_LOOP =
    "    @Kernel\n"
    "    public static void treeGuarded(KernelBuffer<float32> out, KernelBuffer<float32> in,\n"
    "                                   uint32 n, uint32 groups) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 wg = i / 256;\n"
    "        if (wg < groups) {\n"
    "            float32 v = 0.0f;\n"
    "            if (i < n) { v = in[i]; }\n"
    "            lds[t] = v;\n"
    "            uint32 stride = 128;\n"
    "            while (stride > 0) {\n"
    "                Barrier.workgroup();\n"
    "                if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "                Barrier.workgroup();\n"
    "                stride = stride / 2;\n"
    "            }\n"
    "            if (t == 0) { out[wg] = lds[0]; }\n"
    "        }\n"
    "    }\n";


// The same reduce behind an early `return` instead of an `if` — cajeta-llm's
// `qkNormRowsF32` shape, where `row = globalIdX() / 256` is uniform in
// practice but tid-tainted as far as the fission can prove. Accepted: the
// returning work-items go inactive and the barriers stay scaffold.
inline const char* EARLY_RETURN =
    "    @Kernel\n"
    "    public static void treeEarly(KernelBuffer<float32> out, KernelBuffer<float32> in,\n"
    "                                 uint32 n, uint32 groups) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 wg = i / 256;\n"
    "        if (wg >= groups) { return; }\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        uint32 stride = 128;\n"
    "        while (stride > 0) {\n"
    "            Barrier.workgroup();\n"
    "            if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "            Barrier.workgroup();\n"
    "            stride = stride / 2;\n"
    "        }\n"
    "        if (t == 0) { out[wg] = lds[0]; }\n"
    "    }\n";

// A barrier in ONE arm of a work-item-divergent if/else, with real work after
// the join. This is the shape the relaxation must NOT swallow: the lanes that
// take the else arm skip the barrier and then rejoin, so there is no single
// point at which the workgroup is together. Declined by name.
inline const char* DIVERGENT_JOIN =
    "    @Kernel\n"
    "    public static void divergentJoin(KernelBuffer<float32> out,\n"
    "                                     KernelBuffer<float32> in, uint32 n) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        float32 v = 0.0f;\n"
    "        if (t < 128) {\n"
    "            lds[t] = in[i];\n"
    "            Barrier.workgroup();\n"
    "            v = lds[t] + lds[t + 128];\n"
    "        } else {\n"
    "            v = in[i] * 2.0f;\n"
    "        }\n"
    "        out[i] = v;\n"
    "    }\n";


// A barrier behind a guard the WHOLE WORKGROUP takes together, with real work
// after the join — cajeta-llm's `qkPrepKernel`, whose `if (norm != 0) { …two
// barriers… }` is followed by the RoPE loop. `flag` is a kernel PARAMETER, so
// it is not in the taint set and the branch is provably uniform: it can be
// lifted out of the work-item loops and run once, like a barrier loop's
// header. The activity mask cannot help here — nobody leaves.
inline const char* UNIFORM_GUARD_JOIN =
    "    @Kernel\n"
    "    public static void treeUniform(KernelBuffer<float32> out,\n"
    "                                   KernelBuffer<float32> in,\n"
    "                                   uint32 n, uint32 flag) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 wg = Workgroup.x();\n"
    "        float32 v = 0.0f;\n"
    "        if (flag != 0) {\n"
    "            float32 s = 0.0f;\n"
    "            if (i < n) { s = in[i]; }\n"
    "            lds[t] = s;\n"
    "            uint32 stride = 128;\n"
    "            while (stride > 0) {\n"
    "                Barrier.workgroup();\n"
    "                if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "                Barrier.workgroup();\n"
    "                stride = stride / 2;\n"
    "            }\n"
    "            v = lds[0];\n"
    "        }\n"
    "        // Real work after the join, on every work-item either way.\n"
    "        if (t == 0) { out[wg] = v + 1.0f; }\n"
    "    }\n";

// The same uniform guard, but BOTH arms end the kernel — cajeta-llm's
// `iq3xxsQ8WaveGateUpGluKernel`, where the guarded arm runs a barrier reduce
// and returns and the other arm goes on to its own chain. There is no join
// block at all, so the split has to terminate cleanly rather than look for
// one.
inline const char* UNIFORM_GUARD_BOTH_RETURN =
    "    @Kernel\n"
    "    public static void treeSplit(KernelBuffer<float32> out,\n"
    "                                 KernelBuffer<float32> in,\n"
    "                                 uint32 n, uint32 flag) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 wg = Workgroup.x();\n"
    "        if (flag != 0) {\n"
    "            float32 s = 0.0f;\n"
    "            if (i < n) { s = in[i]; }\n"
    "            lds[t] = s;\n"
    "            uint32 stride = 128;\n"
    "            while (stride > 0) {\n"
    "                Barrier.workgroup();\n"
    "                if (t < stride) { lds[t] = lds[t] + lds[t + stride]; }\n"
    "                Barrier.workgroup();\n"
    "                stride = stride / 2;\n"
    "            }\n"
    "            if (t == 0) { out[wg] = lds[0]; }\n"
    "            return;\n"
    "        }\n"
    "        if (t == 0) { out[wg] = -2.0f; }\n"
    "    }\n";


// `iq3xxsQ8WaveGateUpGluKernel`'s full shape, which the two simpler uniform
// guards above do not reach: an outer uniform guard whose taken arm holds a
// SHORT-CIRCUIT `&&` guard over the barrier chain and then returns, with a
// second uniform guard and a second barrier chain on the other side. The `&&`
// is two branches, so the arm contains a nested split whose join is inside
// another split's arm.
inline const char* UNIFORM_GUARD_NESTED =
    "    @Kernel\n"
    "    public static void treeNested(KernelBuffer<float32> out,\n"
    "                                  KernelBuffer<float32> in,\n"
    "                                  uint32 n, uint32 flagA, uint32 flagB) {\n"
    "        Shared<float32> lds = shared float32[256];\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        uint32 wg = Workgroup.x();\n"
    "        if (wg >= 1) {\n"
    "            if (flagA != 0 && wg == 1) {\n"
    "                float32 s = 0.0f;\n"
    "                if (i < n) { s = in[i]; }\n"
    "                lds[t] = s;\n"
    "                uint32 sa = 128;\n"
    "                while (sa > 0) {\n"
    "                    Barrier.workgroup();\n"
    "                    if (t < sa) { lds[t] = lds[t] + lds[t + sa]; }\n"
    "                    Barrier.workgroup();\n"
    "                    sa = sa / 2;\n"
    "                }\n"
    "                if (t == 0) { out[wg] = lds[0]; }\n"
    "            }\n"
    "            return;\n"
    "        }\n"
    "        if (flagB == 0) { return; }\n"
    "        float32 v = 0.0f;\n"
    "        if (i < n) { v = in[i]; }\n"
    "        lds[t] = v;\n"
    "        uint32 sb = 128;\n"
    "        while (sb > 0) {\n"
    "            Barrier.workgroup();\n"
    "            if (t < sb) { lds[t] = lds[t] + lds[t + sb]; }\n"
    "            Barrier.workgroup();\n"
    "            sb = sb / 2;\n"
    "        }\n"
    "        if (t == 0) { out[wg] = lds[0]; }\n"
    "    }\n";

} // namespace cajeta_fission_shapes
