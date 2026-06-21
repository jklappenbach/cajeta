---
id: gpu-xpu
applies-to: [cajeta/gpu/xpu]
title: Cooperative-matrix tiles and LDS panel staging for tiled GEMM
description: How to write matrix/tensor-core GEMM kernels with CooperativeMatrix fragment tiles and the CoopStage global->LDS staging helper.
---

# cajeta.gpu.xpu — matrix/tensor-core GEMM building blocks

Two device-only types for writing tiled matrix-multiply `@Kernel`s against the
hardware matrix/tensor cores:

| You want to… | Use |
|---|---|
| hold one tile of a GEMM on the matrix cores; load/zero/mma/store it | **`CooperativeMatrix<T, Rows, Cols, Use>`** |
| stage a global panel into workgroup-shared (LDS) once per K-step so many waves reuse it | **`CoopStage.panel(...)`** |
| barrier between staging and consuming | not here — `cajeta.gpu.Barrier.workgroup()` |
| allocate the LDS panel itself | not here — `Shared<T> s = shared T[n];` (`cajeta.gpu.AddressSpace`) |
| decode which output tile this wave owns | not here — `cajeta.gpu.GpuThread` / `cajeta.gpu.Workgroup` |
| a fully-managed end-to-end tiled-GEMM block | not provided — you write the K-loop yourself |

These are **device-only**. Both classes are usable only inside an `@Kernel`;
using either on the host is unsupported. The method bodies are empty resolution
placeholders — each is lowered at its call site, never emitted as a real call.

## CooperativeMatrix — the fragment tile

`CooperativeMatrix<T, Rows, Cols, Use>` is one tile of a matmul held
*cooperatively* by a subgroup (wavefront), parameterized by element type `T` and
three compile-time `uint32` constants. `Use` selects the role:
**0 = MatrixA, 1 = MatrixB, 2 = Accumulator**. Distinct `(T,Rows,Cols,Use)` are
distinct device types (like `Vector<T,N>`). v1 targets the square
`Rows == Cols == K` tile (16x16x16 verified), so A is `<...,0>` and B is `<...,1>`
of the same `<T,Rows,Cols>`.

Entry-point verbs (all device-only intrinsics, all return `void`, all mutate the
receiver tile in place):

- `load(GpuBuffer<T> src, uint32 offset, uint32 layout, uint32 stride)` — gather a
  `Rows`x`Cols` sub-tile from global memory. `layout`: 0 = row-major, 1 =
  column-major. `stride` is the **full row width of the wider matrix**, not the
  tile width — that is what makes `load` pick a window out of a larger matrix.
- `load(Shared<T> src, ...)` — same, but the source is a workgroup-shared (LDS)
  panel. Identical fragment layout and addressing; only the storage class differs.
- `splat(T value)` — broadcast a scalar across the tile (zero the accumulator).
- `mma(CooperativeMatrix<T,Rows,Cols,0> a, CooperativeMatrix<T,Rows,Cols,1> b)` —
  `this = a * b + this`, one matrix-core FMA. `this` is the Use-2 accumulator.
- `store(GpuBuffer<T> dst, ...)` / `store(Shared<T> dst, ...)` — write-side
  counterparts of `load`.

### Ownership / lifecycle — there is nothing to free

A `CooperativeMatrix` is **not a host value and not addressable memory**: at
Subgroup scope the tile lives distributed across the wavefront's per-invocation
registers, owned cooperatively by all lanes. You never take its address, never
`#`-transfer it, and never free it — it is kernel-local and dies with the kernel.
The `GpuBuffer<T>` / `Shared<T>` arguments to `load`/`store` are **borrowed**: the
matrix VALUE never leaves the register file, and `load`/`store` do not take
ownership of the buffer or panel.

### Native vs software tier — written once, runs everywhere

The same kernel takes the fastest path each backend offers, chosen statically per
`(backend, dtype)`:

- **Native** — hardware matrix cores: Vulkan `SPV_KHR_cooperative_matrix`
  (float16 and int8 multiplicands), AMD RDNA3 WMMA (f16/bf16 with f32
  accumulator; int8 with i32 accumulator). bf16 is native on AMD.
- **Software** — a portable flat tile-matmul, bit-identical, just not
  matrix-core accelerated. Used on the CPU and where a backend exposes no config
  for the dtype (most notably **bf16 on Vulkan**).

The tier is invisible at the source level (same verbs), but two rules bite:

- **All three tiles of one `mma` must share a tier.** Mixing a native and a
  software operand is a **compile error**. Give a bf16 GEMM a bf16 accumulator,
  an f16 GEMM an f32 accumulator.
- When a tile takes the software path the compiler emits, once per GEMM, a
  `note: [mma-tiering]` diagnostic (severity **below** warning — a capability
  statement, not a defect; it auto-promotes to the cores on hardware that
  exposes the config). Do not "fix" it.

## CoopStage — the global->LDS staging copy

`CoopStage` is a single static method, the ergonomic layer over
`CooperativeMatrix.load(Shared<T>, ...)`:

```
CoopStage.panel<T>(Shared<T> dst, GpuBuffer<T> src,
                   uint32 rowBase, uint32 colBase,
                   uint32 rows, uint32 cols, uint32 ld)
```

The whole workgroup cooperates (threads stride by the block size) to copy the
`rows`x`cols` sub-tile of row-major `src` whose top-left is `(rowBase, colBase)`
and whose leading dimension (full row width) is `ld`, into the **contiguous
row-major LDS tile `dst`** (packed stride = `cols`). A later
`load(dst, subOffset, 0, cols)` then addresses tiles inside the staged panel.
`dst` and `src` are borrowed.

**`panel` does NOT barrier.** The caller owns both barriers: one after staging so
the writes are visible to all waves, and one after the MMAs before the next K-step
overwrites the panel.

## Worked example — LDS-staged 32x32 GEMM (device-verified, AMD/CPU/Vulkan)

A 2x2 grid of waves, each running the K-loop; the waves share the staged panels.
Mirrors `test/xpu/XpuCooperativeMatrixAmdDeviceTests.cpp` (`ldsStagedGemm`).

```cajeta
package test;
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.xpu.CooperativeMatrix;
import cajeta.gpu.xpu.CoopStage;
import cajeta.gpu.GpuThread;
import cajeta.gpu.Barrier;

public class M {
    @Kernel
    public static void gemm(GpuBuffer<float16> a, GpuBuffer<float16> b,
                            GpuBuffer<float32> c) {
        uint32 wave = GpuThread.x() / 32u;       // every lane of the wave agrees
        uint32 ti = wave / 2u;                    // output tile row
        uint32 tj = wave % 2u;                    // output tile col
        Shared<float16> sa = shared float16[32 * 16];  // A row-panel in LDS
        Shared<float16> sb = shared float16[16 * 32];  // B col-panel in LDS
        CooperativeMatrix<float32,16,16,2> acc;
        acc.splat(0.0f);                          // zero accumulator once
        CooperativeMatrix<float16,16,16,0> wa;
        CooperativeMatrix<float16,16,16,1> wb;
        for (uint32 kt = 0; kt < 2u; kt = kt + 1u) {
            CoopStage.panel(sa, a, 0, kt * 16, 32, 16, 32);  // stage A panel
            CoopStage.panel(sb, b, kt * 16, 0, 16, 32, 32);  // stage B panel
            Barrier.workgroup();                  // writes visible to all waves
            wa.load(sa, ti * 256, 0, 16);         // this wave's A tile from LDS
            wb.load(sb, tj * 16,  0, 32);         // this wave's B tile from LDS
            acc.mma(wa, wb);                      // acc += wa . wb
            Barrier.workgroup();                  // before next K overwrites panel
        }
        acc.store(c, ti * 512 + tj * 16, 0, 32);  // write the owned output tile
    }
}
```

For a single un-staged tile, skip `CoopStage`/`Shared` and `load` straight from
`GpuBuffer<T>` (see `XpuCooperativeMatrixDeviceTests.cpp`). Double-buffering (a
second LDS panel staged while the current computes) is a kernel-authoring pattern
on top of these same verbs — there is no built-in for it.
