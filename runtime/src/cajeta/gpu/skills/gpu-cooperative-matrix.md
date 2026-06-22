---
id: gpu-cooperative-matrix
applies-to: [cajeta/gpu/xpu/CooperativeMatrix]
title: CooperativeMatrix — subgroup-cooperative MMA tile
description: Device-only MMA tile <T,Rows,Cols,Use> — declare, load/splat, mma fused multiply-add, store; one tile of a tiled GEMM.
---

# CooperativeMatrix<T, Rows, Cols, Use>

A device-only tile of a matrix multiply held *cooperatively by a subgroup* (wavefront),
mapping to hardware matrix/tensor cores. One `mma` is a full tile-sized fused
multiply-add. Lives in package `cajeta.gpu.xpu`. **Not an access point you call from a
factory — it is a value type you declare directly inside an `@Kernel`.**

Use it when you want matrix-core MMA inside a kernel (GEMM, attention, conv-as-matmul).
For the global→LDS staging copy that feeds the LDS-staged variant, see the sibling skill
`cajeta/gpu/xpu/CoopStage` — this skill covers the tile itself.

## Construction & ownership

Declare it as a local with all four type parameters bound to compile-time constants;
the default constructor takes no args and zeroes the one reserved field. You do **not**
`heap`-allocate it, take its address, pass it to the host, or return it across the kernel
boundary — its storage is the wavefront's distributed register file, not addressable
memory. Distinct `(T, Rows, Cols, Use)` are distinct device types (like `Vector<T,N>`).

`Use` is the SPIR-V cooperative-matrix use: **0 = MatrixA, 1 = MatrixB, 2 = Accumulator**.
v1 targets the square `Rows == Cols == K` tile (16x16x16 is the device-verified config).

The `load`/`store` buffer/Shared arguments are **borrowed**, not transferred — no `#`,
the kernel's caller still owns the `GpuBuffer`/`Shared` backing store. The matrix VALUE
never leaves the register file; `load`/`store` only move element data in/out.

## The methods that matter

All are device-only intrinsics (the bodies are resolution placeholders, never real
calls) — each is lowered at the `@Kernel` call site to the chosen tier. All return
`void` and mutate the receiver tile in place.

- `load(GpuBuffer<T> src, uint32 offset, uint32 layout, uint32 stride)` — fill this tile
  from global memory. `offset` (in elements) selects a sub-tile of a wider matrix;
  `layout` is **0 = row-major, 1 = column-major**; `stride` is the **source's full row
  width**, not the tile width — that is what gathers a `Rows`x`Cols` window out of a
  larger matrix. For a row-major M×K matrix, tile (ti,tj) is `offset = (ti*Rows)*K +
  tj*Cols`, `stride = K`.
- `load(Shared<T> src, uint32 offset, uint32 layout, uint32 stride)` — same, but reads
  from a workgroup-shared (LDS) tile staged by `CoopStage.panel`. Same fragment layout
  and tier rules; only the storage class differs.
- `splat(T value)` — broadcast one scalar to every element. Use `splat(0.0f)` to zero an
  accumulator before a K-loop.
- `mma(CooperativeMatrix<T,Rows,Cols,0> a, CooperativeMatrix<T,Rows,Cols,1> b)` — fused
  multiply-add **in place**: `this = a*b + this`. `this` must be the accumulator
  (Use 2), `a` MatrixA (Use 0), `b` MatrixB (Use 1) — the types enforce the roles.
- `store(GpuBuffer<T> dst, ...)` / `store(Shared<T> dst, ...)` — write the tile out with
  the same sub-tile addressing as `load`.

## Lifecycle, state, concurrency

No `close`/dispose and no drop-on-scope — it is a register value, gone when the kernel
invocation ends. It is **cooperatively owned by the whole subgroup**: every invocation in
the wave executes the same `load`/`mma`/`store` together; you never index a single lane's
fragment. Reusable in place across a K-loop: `splat` once, then `mma` repeatedly into the
same accumulator.

## Sharp edges

- **All three tiles of one `mma` must share a tier** (native vs software). The compiler
  picks a tier statically per `(backend, dtype)`; mixing native and software operands is a
  **compile error**. Give an f16 GEMM an f32 accumulator; give a bf16 GEMM a bf16
  accumulator.
- **`stride` is the wide-matrix row width, not the tile width.** Passing `Cols` instead of
  the real leading dimension silently reads the wrong window.
- **Software-tier `note`.** When a `(backend, dtype)` has no native config (e.g. bfloat16
  on Vulkan) you get one `note: [mma-tiering]` per GEMM — severity *below* warning. It is
  a capability statement, not a defect: the result is bit-identical and auto-promotes to
  the matrix cores on hardware that exposes the config. Do not "fix" it.
- **Host use is unsupported** — there are no host getters/setters; it only exists inside
  `@Kernel`-lowered code.

## Errors

No runtime exceptions — failures are **compile-time**: mismatched tiers, an invalid A/B
`Use`/shape pairing into the accumulator, or host use.

## Minimal example — one 16x16x16 f16 tile (mirrors XpuCooperativeMatrixAmdDeviceTests)

```cajeta
package test;

import cajeta.gpu.GpuBuffer;
import cajeta.gpu.xpu.CooperativeMatrix;

public class M {
    @Kernel
    public static void wmma(GpuBuffer<float16> a, GpuBuffer<float16> b,
                            GpuBuffer<float32> c) {
        CooperativeMatrix<float16,16,16,0> ma;
        ma.load(a, 0, 0, 16);          // A tile: offset 0, row-major, stride 16
        CooperativeMatrix<float16,16,16,1> mb;
        mb.load(b, 0, 0, 16);          // B tile
        CooperativeMatrix<float32,16,16,2> mc;
        mc.splat(0.0f);                // zero accumulator
        mc.mma(ma, mb);                // mc = ma * mb + mc
        mc.store(c, 0, 0, 16);
    }
}
```

A real GEMM tiles this: zero the accumulator once, then loop K, advancing the A/B
`offset` into the wider matrices and accumulating into the same `mc` in place; one
workgroup (wave) per output tile decodes its tile from `Workgroup.x()`. The LDS-staged
form replaces the per-K-step global `load`s with `CoopStage.panel` →
`Barrier.workgroup()` → `load(Shared<T>, ...)` — see `cajeta/gpu/xpu/CoopStage`.
