// SPIKE + REGRESSION FLOOR — the CUDA 700 (illegal-address) fault that killed
// every Q4_K/Q8 quant mat-mul kernel on nvptx (root-cause + fix 2026-09-04,
// memory project_nvptx_quant_matmul_700).
//
// ROOT CAUSE (pinned by bisecting through these probes): a @Kernel that CALLS a
// @Device helper faulted with 700 on nvptx. lowerDeviceFn emits every @Device
// helper `alwaysinline` and passes the kernel's buffer BASE by value, relying
// on inlining to splice that base into the kernel's real buffer access. But
// NvptxBackend::optimizeDeviceModule ran ONLY mem2reg — no inliner — while
// SpirvBackend runs AlwaysInlinerPass and the AMD O3 pipeline inlines. So on
// nvptx the helper stayed OUT-OF-LINE, read a bogus base, and cuLaunchKernel
// returned CUDA_ERROR_ILLEGAL_ADDRESS (700), which is STICKY: it poisons the
// context so every later launch 700s and the whole test zeroes out. This is why
// every quant kernel (they call halfBitsToF32 / scaledAccumInto / ...) faulted
// while the self-contained f32 attention/rope/rmsnorm kernels passed.
// FIX: add AlwaysInlinerPass to optimizeDeviceModule (NvptxBackend.cpp).
//
// What these probes established, in order:
//   (1-6) heap Vector<int32,8> in-kernel, vload<32> int8, dotAccum (both a
//         vload'd and a heap-Vector accumulator = the q4kQ8PackedMatVec shape),
//         dynamic lane index, unaligned vload<32> — every PRIMITIVE the quant
//         kernels use runs correctly in isolation (values 36/496/32/32/28/1008).
//   (7)   a full-kernel replica with all math INLINED runs (64) — addressing,
//         geometry (block:[64]), and buffer sizing are all fine.
//   (8/8a/8b/9) the ONLY thing that faulted was a @Device helper CALL; the
//         bitsToF32 intrinsic (no call) is innocent. (9) minimalDeviceStaticCall
//         is the one-line reproducer: a @Kernel that merely calls a @Device
//         method. NOTE: the helper MUST be @Device — a plain sibling static is
//         rejected as an unsupported device builtin (a skip, not the 700).
//
// A faulting launch is non-fatal (the runtime prints "cuLaunchKernel failed
// (700)" and leaves the output zero), so a fault surfaces as a returned 0. Post
// fix every nvptx case passes; if any EVER returns 0 again the inliner
// regressed. The XpuAmdQuantConstruct.* controls (same sources) are the
// differential: AMD stays green because its pipeline already inlines.
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

// The SAME four kernel sources are portable cajeta — nothing nvidia-specific.
// Running them on AMD is the differential control: these int8-dp4a constructs
// are the validated core of the engine's AMD quant path (the "beats llama.cpp"
// kernels), so AMD MUST pass. AMD green + nvptx red localizes the 700 to nvptx
// codegen, not to the kernel or the constructs. Skips where no ROCm device.
CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

// (4) BASELINE — `heap Vector<int32,8>` constructed inside the kernel, filled,
// const-indexed, reduced. No vload, no dotAccum. If THIS faults, the in-kernel
// heap-Vector lowering itself is the bug. Expect 1+2+3+4+5+6+7+8 = 36.
const char* kHeapVecSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class HeapVec {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<int32,8> v = heap Vector<int32,8>(1, 2, 3, 4, 5, 6, 7, 8);\n"
    "            out[0] = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        out.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(out);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[1];\n"
    "        out.download(h);\n"
    "        out.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

// (3) VLOAD — load a Vector<int8,32> and store it straight back. No dotAccum,
// no heap Vector. If THIS faults, the 8-bit vload/vstore lowering is the bug.
// in[i] = i (0..31); host sums out -> 0+1+...+31 = 496.
const char* kVloadSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Vld {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int8> out, KernelBuffer<int8> in) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<int8,32> v = in.vload<32>(0);\n"
    "            out.vstore(0, v);\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int8[] hin = heap int8[32];\n"
    "        for (int32 i = 0; i < 32; i = i + 1) { hin[i] = (int8) i; }\n"
    "        KernelBuffer<int8> din = heap KernelBuffer<int8>(0, 32);\n"
    "        KernelBuffer<int8> dout = heap KernelBuffer<int8>(0, 32);\n"
    "        din.allocate();\n"
    "        dout.allocate();\n"
    "        din.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(dout, din);\n"
    "        s.sync();\n"
    "        int8[] hout = heap int8[32];\n"
    "        dout.download(hout);\n"
    "        din.free();\n"
    "        dout.free();\n"
    "        int32 sum = 0;\n"
    "        for (int32 i = 0; i < 32; i = i + 1) { sum = sum + (int32) hout[i]; }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

// (1) DOTACCUM with a VLOAD accumulator — the shape the passing CPU dotAccum
// test uses (acc loaded from a seed buffer). w[i]=1, a[i]=1 over 32 lanes ->
// each of 8 acc lanes sums 4 products = 4; total over 8 = 32. If (3)+(4) pass
// but THIS faults, dotAccum's own nvptx lowering is the bug.
const char* kDotAccVloadSeedSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class DotV {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out, KernelBuffer<uint8> w,\n"
    "                         KernelBuffer<int8> a, KernelBuffer<int32> seed) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<uint8,32> wv = w.vload<32>(0);\n"
    "            Vector<int8,32> av = a.vload<32>(0);\n"
    "            Vector<int32,8> acc = seed.vload<8>(0);\n"
    "            Vector<int32,8> r = wv.dotAccum(av, acc);\n"
    "            out[0] = r[0] + r[1] + r[2] + r[3] + r[4] + r[5] + r[6] + r[7];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint8[] hw = heap uint8[32];\n"
    "        int8[] ha = heap int8[32];\n"
    "        int32[] hs = heap int32[8];\n"
    "        for (int32 i = 0; i < 32; i = i + 1) { hw[i] = (uint8) 1; ha[i] = (int8) 1; }\n"
    "        for (int32 i = 0; i < 8; i = i + 1) { hs[i] = 0; }\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(0, 32);\n"
    "        KernelBuffer<int8> a = heap KernelBuffer<int8>(0, 32);\n"
    "        KernelBuffer<int32> seed = heap KernelBuffer<int32>(0, 8);\n"
    "        out.allocate(); w.allocate(); a.allocate(); seed.allocate();\n"
    "        w.upload(hw); a.upload(ha); seed.upload(hs);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(out, w, a, seed);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[1];\n"
    "        out.download(h);\n"
    "        out.free(); w.free(); a.free(); seed.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

// (2) DOTACCUM with a HEAP-VECTOR accumulator — the EXACT shape of
// q4kQ8PackedMatVecKernel (acc seeded by `heap Vector<int32,8>(0,...)`). Same
// inputs as (1), same expected 32. If (1) passes but THIS faults, the trigger
// is a heap-Vector value flowing into dotAccum as its accumulator on nvptx.
const char* kDotAccHeapSeedSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class DotH {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out, KernelBuffer<uint8> w,\n"
    "                         KernelBuffer<int8> a) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<uint8,32> wv = w.vload<32>(0);\n"
    "            Vector<int8,32> av = a.vload<32>(0);\n"
    "            Vector<int32,8> acc = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "            Vector<int32,8> r = wv.dotAccum(av, acc);\n"
    "            out[0] = r[0] + r[1] + r[2] + r[3] + r[4] + r[5] + r[6] + r[7];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint8[] hw = heap uint8[32];\n"
    "        int8[] ha = heap int8[32];\n"
    "        for (int32 i = 0; i < 32; i = i + 1) { hw[i] = (uint8) 1; ha[i] = (int8) 1; }\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        KernelBuffer<uint8> w = heap KernelBuffer<uint8>(0, 32);\n"
    "        KernelBuffer<int8> a = heap KernelBuffer<int8>(0, 32);\n"
    "        out.allocate(); w.allocate(); a.allocate();\n"
    "        w.upload(hw); a.upload(ha);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(out, w, a);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[1];\n"
    "        out.download(h);\n"
    "        out.free(); w.free(); a.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

// (5) DYNAMIC vector-lane index — the real kernel reads `scv[2*g]`/`mnv[sb]`/
// `sv[sbb]` with LOOP-VARIABLE indices; the four passing probes above used only
// CONSTANT indices. A runtime vector-element extract on nvptx spills the vector
// to local memory and indexes it — a bad address there is a 700. in[i]=i over
// 8 int32 lanes, summed through a variable index -> 0+1+...+7 = 28.
const char* kDynIndexSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Dyn {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out, KernelBuffer<int32> in) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<int32,8> v = in.vload<8>(0);\n"
    "            int32 s = 0;\n"
    "            int32 j = 0;\n"
    "            while (j < 8) { s = s + v[j]; j = j + 1; }\n"
    "            out[0] = s;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int32[] hin = heap int32[8];\n"
    "        for (int32 i = 0; i < 8; i = i + 1) { hin[i] = i; }\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        KernelBuffer<int32> in = heap KernelBuffer<int32>(0, 8);\n"
    "        out.allocate(); in.allocate();\n"
    "        in.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(out, in);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[1];\n"
    "        out.download(h);\n"
    "        out.free(); in.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

// (6) UNALIGNED wide vload — the real kernel does `packed.vload<32>(ro + 16)`,
// a 32-byte vector load at a 16-aligned-but-not-32-aligned byte offset. The
// probes above all loaded at offset 0. If nvptx emits a load that assumes
// 32-byte alignment, offset 16 faults. in has 48 int8; load 32 bytes from
// offset 16 -> in[16..47] = 16..47, host sums -> sum(16..47) = 1008.
const char* kUnalignedVloadSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Unal {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int8> out, KernelBuffer<int8> in) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) {\n"
    "            Vector<int8,32> v = in.vload<32>(16);\n"
    "            out.vstore(0, v);\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int8[] hin = heap int8[48];\n"
    "        for (int32 i = 0; i < 48; i = i + 1) { hin[i] = (int8) i; }\n"
    "        KernelBuffer<int8> din = heap KernelBuffer<int8>(0, 48);\n"
    "        KernelBuffer<int8> dout = heap KernelBuffer<int8>(0, 32);\n"
    "        din.allocate(); dout.allocate();\n"
    "        din.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(dout, din);\n"
    "        s.sync();\n"
    "        int8[] hout = heap int8[32];\n"
    "        dout.download(hout);\n"
    "        din.free(); dout.free();\n"
    "        int32 sum = 0;\n"
    "        for (int32 i = 0; i < 32; i = i + 1) { sum = sum + (int32) hout[i]; }\n"
    "        return sum;\n"
    "    }\n"
    "}\n";

// (7) INTEGRATION REPLICA of q4kQ8PackedMatVecKernel at the REAL launch
// geometry (block:[64] = ROWS_PER_BLOCK, the full blocksPerRow loop, buffers
// sized exactly as the caller sizes them: packed = rows*bpr*144, xp = bpr*320).
// The f16/f32 bit conversions are stubbed to trivial arithmetic — only the
// ADDRESSING is under test, and every offset here is copied verbatim from the
// real kernel. All six isolated probes pass, so if THIS faults the trigger is
// the integrated body at scale (register/local pressure, the reused heap-Vector
// accumulator across iterations), not any single construct. rows=64, bpr=2;
// nonzero inputs -> every row's acc is nonzero, so run() returns the count of
// nonzero outputs: 64 if it ran, 0 if it 700-faulted.
const char* kIntegrationSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Intg {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> y, KernelBuffer<int8> packed,\n"
    "                         KernelBuffer<int8> xp, uint32 rows, int64 blocksPerRow) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < rows) {\n"
    "            int64 i = (int64) gi;\n"
    "            int64 rowBytes = blocksPerRow * 144L;\n"
    "            float32 acc = 0.0f;\n"
    "            int64 b = 0;\n"
    "            while (b < blocksPerRow) {\n"
    "                int64 ro = i * rowBytes + b * 144L;\n"
    "                Vector<int8,16> hv = packed.vload<16>(ro);\n"
    "                int32 d0 = (int32) hv[4] & 255;\n"
    "                int32 d1 = (int32) hv[5] & 255;\n"
    "                int32 d2 = (int32) hv[6] & 255;\n"
    "                int32 d3 = (int32) hv[7] & 255;\n"
    "                int32 m0 = (int32) hv[8] & 255;\n"
    "                int32 k0 = (int32) hv[12] & 255;\n"
    "                int32 k1 = (int32) hv[13] & 255;\n"
    "                int32 k2 = (int32) hv[14] & 255;\n"
    "                int32 k3 = (int32) hv[15] & 255;\n"
    "                Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "                Vector<int32,8> scv = zv;\n"
    "                Vector<int32,8> mnv = zv;\n"
    "                scv[0] = d0 & 63;\n"
    "                scv[1] = d1 & 63;\n"
    "                scv[2] = d2 & 63;\n"
    "                scv[3] = d3 & 63;\n"
    "                scv[4] = (k0 & 15) | ((d0 >> 2) & 48);\n"
    "                scv[5] = (k1 & 15) | ((d1 >> 2) & 48);\n"
    "                scv[6] = (k2 & 15) | ((d2 >> 2) & 48);\n"
    "                scv[7] = (k3 & 15) | ((d3 >> 2) & 48);\n"
    "                mnv[0] = m0 & 63;\n"
    "                int64 xb = b * 320L;\n"
    "                Vector<int32,8> tot = zv;\n"
    "                int32 g = 0;\n"
    "                while (g < 4) {\n"
    "                    Vector<uint8,32> raw = packed.vload<32>(\n"
    "                        ro + 16L + (int64) (32 * g)).asUnsigned();\n"
    "                    Vector<uint8,32> lo = raw & 15;\n"
    "                    Vector<uint8,32> hi = (raw >> 4) & 15;\n"
    "                    Vector<int8,32> a0 = xp.vload<32>(xb + (int64) (64 * g));\n"
    "                    Vector<int8,32> a1 = xp.vload<32>(xb + (int64) (64 * g + 32));\n"
    "                    tot = tot + lo.dotAccum(a0, zv) * scv[2 * g];\n"
    "                    tot = tot + hi.dotAccum(a1, zv) * scv[2 * g + 1];\n"
    "                    g = g + 1;\n"
    "                }\n"
    "                int32 sumi = tot[0] + tot[1] + tot[2] + tot[3]\n"
    "                           + tot[4] + tot[5] + tot[6] + tot[7];\n"
    "                Vector<int8,32> sv = xp.vload<32>(xb + 256L);\n"
    "                int32 dmins = 0;\n"
    "                int32 sb = 0;\n"
    "                while (sb < 8) {\n"
    "                    int32 sbb = sb * 4;\n"
    "                    int32 ps = ((int32) sv[sbb] & 255)\n"
    "                        | (((int32) sv[sbb + 1] & 255) << 8)\n"
    "                        | (((int32) sv[sbb + 2] & 255) << 16)\n"
    "                        | (((int32) sv[sbb + 3]) << 24);\n"
    "                    dmins = dmins + mnv[sb] * ps;\n"
    "                    sb = sb + 1;\n"
    "                }\n"
    "                Vector<int8,16> dv = xp.vload<16>(xb + 288L);\n"
    "                float32 xs = (float32) ((int32) dv[0] & 255);\n"
    "                float32 d = (float32) (d0 + 1);\n"
    "                float32 dmin = 1.0f;\n"
    "                acc = acc + d * xs * (float32) sumi - dmin * xs * (float32) dmins;\n"
    "                b = b + 1;\n"
    "            }\n"
    "            y[i] = acc;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 64;\n"
    "        int64 bpr = 2L;\n"
    "        int64 pbytes = (int64) rows * bpr * 144L;\n"
    "        int64 xbytes = bpr * 320L;\n"
    "        int8[] hp = heap int8[pbytes];\n"
    "        int8[] hx = heap int8[xbytes];\n"
    "        for (int64 j = 0; j < pbytes; j = j + 1) { hp[j] = (int8) ((j % 127L) + 1L); }\n"
    "        for (int64 j = 0; j < xbytes; j = j + 1) { hx[j] = (int8) ((j % 127L) + 1L); }\n"
    "        KernelBuffer<int8> packed = heap KernelBuffer<int8>(0, pbytes);\n"
    "        KernelBuffer<int8> xp = heap KernelBuffer<int8>(0, xbytes);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, (int64) rows);\n"
    "        packed.allocate(); xp.allocate(); y.allocate();\n"
    "        packed.upload(hp); xp.upload(hx);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(y, packed, xp, rows, bpr);\n"
    "        s.sync();\n"
    "        float32[] hy = heap float32[rows];\n"
    "        y.download(hy);\n"
    "        packed.free(); xp.free(); y.free();\n"
    "        int32 nz = 0;\n"
    "        for (int32 r = 0; r < (int32) rows; r = r + 1) {\n"
    "            if (hy[r] != 0.0f) { nz = nz + 1; }\n"
    "        }\n"
    "        return nz;\n"
    "    }\n"
    "}\n";

// (8) The (7) replica PLUS the two real bit-conversions the real kernel calls:
// a static helper `hb` (the verbatim GgufFile.halfBitsToF32 body, a cross-class
// static call in the real kernel) and Cajeta.bitsToF32 (an IEEE bitcast
// intrinsic). (7) inlines all math and PASSES; the only thing it drops is these
// two. If (8) faults where (7) ran, the trigger is a device-side static method
// CALL and/or the bitsToF32 intrinsic on nvptx — not the addressing. Same
// geometry/sizes/return contract as (7): 64 if it ran, 0 if it 700-faulted.
const char* kIntegrationRealMathSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.lang.Cajeta;\n"
    "public class IntgM {\n"
    "    @Device static float32 hb(int32 h) {\n"
    "        int32 sign = (h >> 15) & 1;\n"
    "        int32 exp = (h >> 10) & 31;\n"
    "        int32 man = h & 1023;\n"
    "        int64 bits;\n"
    "        if (exp == 0) {\n"
    "            if (man == 0) { bits = (int64) sign << 31; }\n"
    "            else {\n"
    "                int32 k = 9;\n"
    "                while ((man & (1 << k)) == 0) { k = k - 1; }\n"
    "                bits = ((int64) sign << 31) | ((int64) (k + 103) << 23)\n"
    "                     | (((int64) man << (23 - k)) & 8388607L);\n"
    "            }\n"
    "        } else if (exp == 31) {\n"
    "            bits = ((int64) sign << 31) | (255L << 23) | ((int64) man << 13);\n"
    "        } else {\n"
    "            bits = ((int64) sign << 31) | ((int64) (exp + 112) << 23)\n"
    "                 | ((int64) man << 13);\n"
    "        }\n"
    "        return Cajeta.bitsToF32(bits);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> y, KernelBuffer<int8> packed,\n"
    "                         KernelBuffer<int8> xp, uint32 rows, int64 blocksPerRow) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < rows) {\n"
    "            int64 i = (int64) gi;\n"
    "            int64 rowBytes = blocksPerRow * 144L;\n"
    "            float32 acc = 0.0f;\n"
    "            int64 b = 0;\n"
    "            while (b < blocksPerRow) {\n"
    "                int64 ro = i * rowBytes + b * 144L;\n"
    "                Vector<int8,16> hv = packed.vload<16>(ro);\n"
    "                float32 d = IntgM.hb(((int32) hv[0] & 255) | (((int32) hv[1] & 255) << 8));\n"
    "                float32 dmin = IntgM.hb(((int32) hv[2] & 255) | (((int32) hv[3] & 255) << 8));\n"
    "                int32 d0 = (int32) hv[4] & 255;\n"
    "                int32 m0 = (int32) hv[8] & 255;\n"
    "                Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "                Vector<int32,8> scv = zv;\n"
    "                Vector<int32,8> mnv = zv;\n"
    "                scv[0] = d0 & 63;\n"
    "                mnv[0] = m0 & 63;\n"
    "                int64 xb = b * 320L;\n"
    "                Vector<int32,8> tot = zv;\n"
    "                int32 g = 0;\n"
    "                while (g < 4) {\n"
    "                    Vector<uint8,32> raw = packed.vload<32>(\n"
    "                        ro + 16L + (int64) (32 * g)).asUnsigned();\n"
    "                    Vector<uint8,32> lo = raw & 15;\n"
    "                    Vector<uint8,32> hi = (raw >> 4) & 15;\n"
    "                    Vector<int8,32> a0 = xp.vload<32>(xb + (int64) (64 * g));\n"
    "                    Vector<int8,32> a1 = xp.vload<32>(xb + (int64) (64 * g + 32));\n"
    "                    tot = tot + lo.dotAccum(a0, zv) * scv[2 * g];\n"
    "                    tot = tot + hi.dotAccum(a1, zv) * scv[2 * g + 1];\n"
    "                    g = g + 1;\n"
    "                }\n"
    "                int32 sumi = tot[0] + tot[1] + tot[2] + tot[3]\n"
    "                           + tot[4] + tot[5] + tot[6] + tot[7];\n"
    "                Vector<int8,32> sv = xp.vload<32>(xb + 256L);\n"
    "                int32 dmins = 0;\n"
    "                int32 sb = 0;\n"
    "                while (sb < 8) {\n"
    "                    int32 sbb = sb * 4;\n"
    "                    int32 ps = ((int32) sv[sbb] & 255)\n"
    "                        | (((int32) sv[sbb + 1] & 255) << 8)\n"
    "                        | (((int32) sv[sbb + 2] & 255) << 16)\n"
    "                        | (((int32) sv[sbb + 3]) << 24);\n"
    "                    dmins = dmins + mnv[sb] * ps;\n"
    "                    sb = sb + 1;\n"
    "                }\n"
    "                Vector<int8,16> dv = xp.vload<16>(xb + 288L);\n"
    "                float32 xs = Cajeta.bitsToF32(((int32) dv[0] & 255)\n"
    "                    | (((int32) dv[1] & 255) << 8)\n"
    "                    | (((int32) dv[2] & 255) << 16)\n"
    "                    | (((int32) dv[3] & 255) << 24));\n"
    "                acc = acc + d * xs * (float32) sumi - dmin * xs * (float32) dmins;\n"
    "                b = b + 1;\n"
    "            }\n"
    "            y[i] = acc;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 64;\n"
    "        int64 bpr = 2L;\n"
    "        int64 pbytes = (int64) rows * bpr * 144L;\n"
    "        int64 xbytes = bpr * 320L;\n"
    "        int8[] hp = heap int8[pbytes];\n"
    "        int8[] hx = heap int8[xbytes];\n"
    "        for (int64 j = 0; j < pbytes; j = j + 1) { hp[j] = (int8) ((j % 127L) + 1L); }\n"
    "        for (int64 j = 0; j < xbytes; j = j + 1) { hx[j] = (int8) ((j % 127L) + 1L); }\n"
    "        KernelBuffer<int8> packed = heap KernelBuffer<int8>(0, pbytes);\n"
    "        KernelBuffer<int8> xp = heap KernelBuffer<int8>(0, xbytes);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, (int64) rows);\n"
    "        packed.allocate(); xp.allocate(); y.allocate();\n"
    "        packed.upload(hp); xp.upload(hx);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(y, packed, xp, rows, bpr);\n"
    "        s.sync();\n"
    "        float32[] hy = heap float32[rows];\n"
    "        y.download(hy);\n"
    "        packed.free(); xp.free(); y.free();\n"
    "        int32 nz = 0;\n"
    "        for (int32 r = 0; r < (int32) rows; r = r + 1) {\n"
    "            if (hy[r] != 0.0f) { nz = nz + 1; }\n"
    "        }\n"
    "        return nz;\n"
    "    }\n"
    "}\n";

// (8a) SPLIT-A: the (7) replica, all math inlined EXCEPT `xs` uses the
// Cajeta.bitsToF32 INTRINSIC (no cross-class call anywhere). Faults here => the
// bitsToF32 device lowering is the 700. Runs here => bitsToF32 is innocent.
const char* kSplitBitsOnlySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.lang.Cajeta;\n"
    "public class SplB {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> y, KernelBuffer<int8> packed,\n"
    "                         KernelBuffer<int8> xp, uint32 rows, int64 blocksPerRow) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < rows) {\n"
    "            int64 i = (int64) gi;\n"
    "            int64 rowBytes = blocksPerRow * 144L;\n"
    "            float32 acc = 0.0f;\n"
    "            int64 b = 0;\n"
    "            while (b < blocksPerRow) {\n"
    "                int64 ro = i * rowBytes + b * 144L;\n"
    "                Vector<int8,16> hv = packed.vload<16>(ro);\n"
    "                int32 d0 = (int32) hv[4] & 255;\n"
    "                int32 m0 = (int32) hv[8] & 255;\n"
    "                Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "                Vector<int32,8> scv = zv;\n"
    "                Vector<int32,8> mnv = zv;\n"
    "                scv[0] = d0 & 63;\n"
    "                mnv[0] = m0 & 63;\n"
    "                int64 xb = b * 320L;\n"
    "                Vector<int32,8> tot = zv;\n"
    "                int32 g = 0;\n"
    "                while (g < 4) {\n"
    "                    Vector<uint8,32> raw = packed.vload<32>(\n"
    "                        ro + 16L + (int64) (32 * g)).asUnsigned();\n"
    "                    Vector<uint8,32> lo = raw & 15;\n"
    "                    Vector<uint8,32> hi = (raw >> 4) & 15;\n"
    "                    Vector<int8,32> a0 = xp.vload<32>(xb + (int64) (64 * g));\n"
    "                    Vector<int8,32> a1 = xp.vload<32>(xb + (int64) (64 * g + 32));\n"
    "                    tot = tot + lo.dotAccum(a0, zv) * scv[2 * g];\n"
    "                    tot = tot + hi.dotAccum(a1, zv) * scv[2 * g + 1];\n"
    "                    g = g + 1;\n"
    "                }\n"
    "                int32 sumi = tot[0] + tot[1] + tot[2] + tot[3]\n"
    "                           + tot[4] + tot[5] + tot[6] + tot[7];\n"
    "                Vector<int8,16> dv = xp.vload<16>(xb + 288L);\n"
    "                float32 xs = Cajeta.bitsToF32(((int32) dv[0] & 255)\n"
    "                    | (((int32) dv[1] & 255) << 8)\n"
    "                    | (((int32) dv[2] & 255) << 16)\n"
    "                    | (((int32) dv[3] & 255) << 24));\n"
    "                acc = acc + xs * (float32) sumi;\n"
    "                b = b + 1;\n"
    "            }\n"
    "            y[i] = acc;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 64;\n"
    "        int64 bpr = 2L;\n"
    "        int64 pbytes = (int64) rows * bpr * 144L;\n"
    "        int64 xbytes = bpr * 320L;\n"
    "        int8[] hp = heap int8[pbytes];\n"
    "        int8[] hx = heap int8[xbytes];\n"
    "        for (int64 j = 0; j < pbytes; j = j + 1) { hp[j] = (int8) ((j % 127L) + 1L); }\n"
    "        for (int64 j = 0; j < xbytes; j = j + 1) { hx[j] = (int8) ((j % 100L) + 1L); }\n"
    "        KernelBuffer<int8> packed = heap KernelBuffer<int8>(0, pbytes);\n"
    "        KernelBuffer<int8> xp = heap KernelBuffer<int8>(0, xbytes);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, (int64) rows);\n"
    "        packed.allocate(); xp.allocate(); y.allocate();\n"
    "        packed.upload(hp); xp.upload(hx);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(y, packed, xp, rows, bpr);\n"
    "        s.sync();\n"
    "        float32[] hy = heap float32[rows];\n"
    "        y.download(hy);\n"
    "        packed.free(); xp.free(); y.free();\n"
    "        int32 nz = 0;\n"
    "        for (int32 r = 0; r < (int32) rows; r = r + 1) {\n"
    "            if (hy[r] != 0.0f) { nz = nz + 1; }\n"
    "        }\n"
    "        return nz;\n"
    "    }\n"
    "}\n";

// (8b) SPLIT-B: the (7) replica, but `d`/`dmin` come from a cross-class-style
// static method CALL `hb2` that returns pure ARITHMETIC (NO bitsToF32, no
// intrinsic). Faults here => a device-side static method call is the 700. Runs
// here => the call machinery is fine and bitsToF32 (8a) is the culprit.
const char* kSplitCallOnlySource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class SplC {\n"
    "    @Device static float32 hb2(int32 h) {\n"
    "        int32 e = (h >> 10) & 31;\n"
    "        int32 m = h & 1023;\n"
    "        return (float32) (e * 8 + (m & 7) + 1);\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> y, KernelBuffer<int8> packed,\n"
    "                         KernelBuffer<int8> xp, uint32 rows, int64 blocksPerRow) {\n"
    "        uint32 gi = KernelThread.globalIdX();\n"
    "        if (gi < rows) {\n"
    "            int64 i = (int64) gi;\n"
    "            int64 rowBytes = blocksPerRow * 144L;\n"
    "            float32 acc = 0.0f;\n"
    "            int64 b = 0;\n"
    "            while (b < blocksPerRow) {\n"
    "                int64 ro = i * rowBytes + b * 144L;\n"
    "                Vector<int8,16> hv = packed.vload<16>(ro);\n"
    "                float32 d = SplC.hb2(((int32) hv[0] & 255) | (((int32) hv[1] & 255) << 8));\n"
    "                float32 dmin = SplC.hb2(((int32) hv[2] & 255) | (((int32) hv[3] & 255) << 8));\n"
    "                int32 d0 = (int32) hv[4] & 255;\n"
    "                int32 m0 = (int32) hv[8] & 255;\n"
    "                Vector<int32,8> zv = heap Vector<int32,8>(0, 0, 0, 0, 0, 0, 0, 0);\n"
    "                Vector<int32,8> scv = zv;\n"
    "                Vector<int32,8> mnv = zv;\n"
    "                scv[0] = d0 & 63;\n"
    "                mnv[0] = m0 & 63;\n"
    "                int64 xb = b * 320L;\n"
    "                Vector<int32,8> tot = zv;\n"
    "                int32 g = 0;\n"
    "                while (g < 4) {\n"
    "                    Vector<uint8,32> raw = packed.vload<32>(\n"
    "                        ro + 16L + (int64) (32 * g)).asUnsigned();\n"
    "                    Vector<uint8,32> lo = raw & 15;\n"
    "                    Vector<uint8,32> hi = (raw >> 4) & 15;\n"
    "                    Vector<int8,32> a0 = xp.vload<32>(xb + (int64) (64 * g));\n"
    "                    Vector<int8,32> a1 = xp.vload<32>(xb + (int64) (64 * g + 32));\n"
    "                    tot = tot + lo.dotAccum(a0, zv) * scv[2 * g];\n"
    "                    tot = tot + hi.dotAccum(a1, zv) * scv[2 * g + 1];\n"
    "                    g = g + 1;\n"
    "                }\n"
    "                int32 sumi = tot[0] + tot[1] + tot[2] + tot[3]\n"
    "                           + tot[4] + tot[5] + tot[6] + tot[7];\n"
    "                acc = acc + d * (float32) sumi + dmin;\n"
    "                b = b + 1;\n"
    "            }\n"
    "            y[i] = acc;\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 64;\n"
    "        int64 bpr = 2L;\n"
    "        int64 pbytes = (int64) rows * bpr * 144L;\n"
    "        int64 xbytes = bpr * 320L;\n"
    "        int8[] hp = heap int8[pbytes];\n"
    "        int8[] hx = heap int8[xbytes];\n"
    "        for (int64 j = 0; j < pbytes; j = j + 1) { hp[j] = (int8) ((j % 127L) + 1L); }\n"
    "        for (int64 j = 0; j < xbytes; j = j + 1) { hx[j] = (int8) ((j % 100L) + 1L); }\n"
    "        KernelBuffer<int8> packed = heap KernelBuffer<int8>(0, pbytes);\n"
    "        KernelBuffer<int8> xp = heap KernelBuffer<int8>(0, xbytes);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, (int64) rows);\n"
    "        packed.allocate(); xp.allocate(); y.allocate();\n"
    "        packed.upload(hp); xp.upload(hx);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [64])(y, packed, xp, rows, bpr);\n"
    "        s.sync();\n"
    "        float32[] hy = heap float32[rows];\n"
    "        y.download(hy);\n"
    "        packed.free(); xp.free(); y.free();\n"
    "        int32 nz = 0;\n"
    "        for (int32 r = 0; r < (int32) rows; r = r + 1) {\n"
    "            if (hy[r] != 0.0f) { nz = nz + 1; }\n"
    "        }\n"
    "        return nz;\n"
    "    }\n"
    "}\n";

// (9) MINIMAL reproducer — a trivial kernel whose ONLY non-primitive act is a
// call to a user static method that returns pure arithmetic. No vectors, no
// dp4a, no loop, one thread. If THIS faults, device-side user-function calls
// are categorically broken on nvptx — the whole story behind the quant 700s.
// in[0]=41 -> addOne -> out[0]=42.
const char* kMinimalDeviceCallSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class MinCall {\n"
    "    @Device static int32 addOne(int32 x) { return x + 1; }\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out, KernelBuffer<int32> in) {\n"
    "        uint32 g = KernelThread.globalIdX();\n"
    "        if (g < 1) { out[0] = MinCall.addOne(in[0]); }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        int32[] hin = heap int32[1];\n"
    "        hin[0] = 41;\n"
    "        KernelBuffer<int32> out = heap KernelBuffer<int32>(0, 1);\n"
    "        KernelBuffer<int32> in = heap KernelBuffer<int32>(0, 1);\n"
    "        out.allocate(); in.allocate();\n"
    "        in.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [1])(out, in);\n"
    "        s.sync();\n"
    "        int32[] h = heap int32[1];\n"
    "        out.download(h);\n"
    "        out.free(); in.free();\n"
    "        return h[0];\n"
    "    }\n"
    "}\n";

int runInt(const char* src, const char* cls, const CajetaJit::Options& opts) {
    auto jit = CajetaJit::compile(src, cls, opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -999999;
    auto fn = jit->lookup<int (*)()>("run");
    EXPECT_NE(fn, nullptr);
    if (!fn) return -999999;
    return fn();
}

}  // namespace

// ---- NVPTX: the backend under suspicion (run one at a time; 700 is sticky) --

// (4) in-kernel heap Vector<int32,8> — the accumulator seed, alone.
TEST(XpuNvptxQuantConstruct, heapVectorInsideKernel) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kHeapVecSource, "test.HeapVec", cudaOptions()), 36)
        << "heap Vector<int32,8> inside an nvptx kernel returned 0/garbage "
           "(0 == a 700-faulted launch)";
}

// (3) 8-bit vload<32>/vstore, alone.
TEST(XpuNvptxQuantConstruct, vload32Int8) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kVloadSource, "test.Vld", cudaOptions()), 496)
        << "vload<32>/vstore of int8 on nvptx returned 0/garbage "
           "(0 == a 700-faulted launch)";
}

// (1) dotAccum with a vload'd accumulator (the passing-CPU-test shape).
TEST(XpuNvptxQuantConstruct, dotAccumVloadSeed) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kDotAccVloadSeedSource, "test.DotV", cudaOptions()), 32)
        << "dotAccum (vload'd accumulator) on nvptx returned 0/garbage "
           "(0 == a 700-faulted launch)";
}

// (2) dotAccum with a heap-Vector accumulator (the q4kQ8PackedMatVec shape).
TEST(XpuNvptxQuantConstruct, dotAccumHeapVectorSeed) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kDotAccHeapSeedSource, "test.DotH", cudaOptions()), 32)
        << "dotAccum (heap-Vector accumulator, the q4kQ8PackedMatVec shape) on "
           "nvptx returned 0/garbage (0 == a 700-faulted launch)";
}

// (5) dynamic (loop-variable) vector-lane index.
TEST(XpuNvptxQuantConstruct, dynamicVectorIndex) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kDynIndexSource, "test.Dyn", cudaOptions()), 28)
        << "dynamic vector-lane index (scv[2*g] shape) on nvptx returned "
           "0/garbage (0 == a 700-faulted launch)";
}

// (6) unaligned wide vload<32> at a 16-aligned-but-not-32 offset.
TEST(XpuNvptxQuantConstruct, unalignedVload32) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kUnalignedVloadSource, "test.Unal", cudaOptions()), 1008)
        << "vload<32> at offset 16 (packed.vload<32>(ro+16) shape) on nvptx "
           "returned 0/garbage (0 == a 700-faulted launch)";
}

// (7) integration replica at real geometry — the bisection reproducer.
TEST(XpuNvptxQuantConstruct, integrationReplicaAtRealGeometry) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kIntegrationSource, "test.Intg", cudaOptions()), 64)
        << "the q4kQ8PackedMatVec replica (block:[64], full loop, real sizes) "
           "on nvptx produced " << "fewer than 64 nonzero rows (0 == a "
           "700-faulted launch) — the integrated body at scale is the trigger";
}

// (8) replica + real device static-method call (hb) + bitsToF32 intrinsic.
TEST(XpuNvptxQuantConstruct, integrationRealMath) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kIntegrationRealMathSource, "test.IntgM", cudaOptions()), 64)
        << "the replica WITH the real halfBitsToF32 device call + bitsToF32 on "
           "nvptx produced fewer than 64 nonzero rows (0 == a 700-faulted "
           "launch) — a device static-method call/bitsToF32 is the trigger";
}

// (8a) bitsToF32 intrinsic alone (no cross-class call).
TEST(XpuNvptxQuantConstruct, splitBitsToF32Only) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kSplitBitsOnlySource, "test.SplB", cudaOptions()), 64)
        << "bitsToF32 intrinsic in the loop on nvptx faulted (0 nonzero rows)";
}

// (8b) device static-method call alone (returns arithmetic, no bitsToF32).
TEST(XpuNvptxQuantConstruct, splitDeviceCallOnly) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kSplitCallOnlySource, "test.SplC", cudaOptions()), 64)
        << "a device static-method call in the loop on nvptx faulted (0 rows)";
}

// (9) THE minimal reproducer: a @Kernel that only calls a user static method.
TEST(XpuNvptxQuantConstruct, minimalDeviceStaticCall) {
    CAJETA_SKIP_IF_NO_CUDA();
    EXPECT_EQ(runInt(kMinimalDeviceCallSource, "test.MinCall", cudaOptions()), 42)
        << "a @Kernel that merely CALLS a user static method faulted on nvptx "
           "(0/garbage) — device-side user-function calls are broken on nvptx, "
           "the root cause of every quant-kernel 700";
}

TEST(XpuAmdQuantConstruct, minimalDeviceStaticCall) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kMinimalDeviceCallSource, "test.MinCall", amdOptions()), 42)
        << "a @Kernel calling a user static method must work on AMD (control)";
}

// ---- AMD: the differential control. Same sources; these are the validated
// core of the engine's AMD quant path, so they MUST pass. AMD green while
// nvptx is red proves the 700 is nvptx codegen, not the kernel. (Skips here —
// no ROCm device on this box; the AMD session runs these.) --------------------

TEST(XpuAmdQuantConstruct, heapVectorInsideKernel) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kHeapVecSource, "test.HeapVec", amdOptions()), 36)
        << "heap Vector<int32,8> inside a kernel must work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, vload32Int8) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kVloadSource, "test.Vld", amdOptions()), 496)
        << "vload<32>/vstore of int8 must work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, dotAccumVloadSeed) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kDotAccVloadSeedSource, "test.DotV", amdOptions()), 32)
        << "dotAccum (vload'd accumulator) must work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, dotAccumHeapVectorSeed) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kDotAccHeapSeedSource, "test.DotH", amdOptions()), 32)
        << "dotAccum (heap-Vector accumulator, q4kQ8PackedMatVec shape) must "
           "work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, dynamicVectorIndex) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kDynIndexSource, "test.Dyn", amdOptions()), 28)
        << "dynamic vector-lane index must work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, unalignedVload32) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kUnalignedVloadSource, "test.Unal", amdOptions()), 1008)
        << "vload<32> at offset 16 must work on AMD (control)";
}

TEST(XpuAmdQuantConstruct, integrationReplicaAtRealGeometry) {
    CAJETA_SKIP_IF_NO_HIP();
    EXPECT_EQ(runInt(kIntegrationSource, "test.Intg", amdOptions()), 64)
        << "the q4kQ8PackedMatVec replica must run on AMD (control) — this is "
           "the shape of the engine's validated AMD quant path";
}
