---
id: xpu-kernel-intrinsics
applies-to: [cajeta/gpu/GpuThread, cajeta/gpu/Workgroup, cajeta/gpu/Wave, cajeta/gpu/Quad, cajeta/gpu/Barrier, cajeta/gpu/Bits, cajeta/gpu/MemoryOrder]
title: GPU kernel intrinsics (thread/workgroup coords, wave & quad cross-lane ops, Barrier, Bits, MemoryOrder)
description: Device-only kernel primitives — where am I (GpuThread/Workgroup), cooperate across lanes (Wave/Quad), synchronize (Barrier), bit-twiddle (Bits), and order atomics/fences (MemoryOrder).
---

# GPU kernel intrinsics

These are the **device-only** building blocks you call *inside* a `@Kernel` / `@Device`
function. Pick by what you need:

| Want to… | Use |
| --- | --- |
| Know which work-item I am (grid-global or in-block index) | `GpuThread` |
| Know which workgroup I am / how big it is | `Workgroup` |
| Cooperate across the whole wave (warp/wavefront/subgroup) | `Wave` |
| Cooperate across the 2×2 quad (derivatives, tiny stencils) | `Quad` |
| Make threads rendezvous, or publish memory without a rendezvous | `Barrier` |
| Bit-reverse / popcount / rotate my own 32-bit value | `Bits` |
| Set the ordering of an atomic or a memory fence | `MemoryOrder` |

Not here: launching kernels, streams, events, atomics on memory, shared memory.
Atomics (`atomicAdd`/`atomicMax`/…) and the `KernelBuffer<T>` they act on live in the
buffer skill; host-side launch/sync lives in `gpu-execution`. `MemoryOrder` is the
*argument* those atomics (and `Barrier` fences) take.

## Members and roles

All seven are `public final class`es exposing only `static` methods — **you never
instantiate them**; you call `Wave.laneId()`, `GpuThread.globalIdX()`, etc. `MemoryOrder`
is a plain `enum`. They lower to native intrinsics at codegen (NVPTX/AMDGPU/SPIR-V) or to
CPU-emulation forwarders; you write one source, every backend gets the right instruction.

- **`GpuThread`** — `x()/y()/z()` (block-local index), `globalIdX()/Y()/Z()`
  (grid-global = `Workgroup.x()*Workgroup.dimX()+GpuThread.x()`). All `uint32`.
- **`Workgroup`** — `x()/y()/z()` (this block's position in the grid) and
  `dimX()/dimY()/dimZ()` (block size in threads = the `block:` launch arg). Uniform
  across the block. All `uint32`.
- **`Wave`** — environment: `width()`, `laneId()` (in `[0,width())`), `isFirstLane()`.
  Cross-lane: `shuffleSync(value, srcLane)`, `ballotSync(predicate)→uint64`,
  `reduceSum/Max/Min/And/Or/Xor(value)`, `prefixSum/prefixProduct(value)` (both
  **exclusive**: lane 0 gets 0 / 1), `rotate(value, delta)`. Scalar ops are `uint32`;
  unsigned for max/min.
- **`Quad`** — `broadcast(value, index)` (`index` 0..3), `swapHorizontal/Vertical/
  Diagonal(value)` (partners `laneId()^1 / ^2 / ^3`), `all(predicate)` / `any(predicate)`
  votes over the four lanes `[laneId()&~3 .. +3]`.
- **`Barrier`** — `workgroup()`, `wave()`, `workgroupMemory([MemoryOrder])`,
  `deviceMemory([MemoryOrder])`.
- **`Bits`** — per-thread scalar: `reverse(value)`, `count(value)` (popcount, `[0,32]`),
  `rotateLeft/rotateRight(value, amount)` (mod 32). NOT wave-cooperative — each work-item
  on its own value.
- **`MemoryOrder`** — `Relaxed, Acquire, Release, AcqRel, SeqCst` (ordinal is the
  contract; do not reorder).

## Ownership / lifecycle

Nothing crosses an ownership boundary here: every parameter and return is a primitive
(`uint32`/`uint64`/`boolean`) passed by value, or the `MemoryOrder` enum. No `#`
transfer, no heap, no `close()`, nothing to free. The only constraints are **placement**
(device-only) and **ordering** (barriers, see below) — not memory ownership.

## How they cooperate — the rules that bite

**1. `Barrier` before any `Wave` shuffle/reduce.** Wave cross-lane ops (`shuffleSync`,
`ballotSync`, the `reduce*`/`prefix*`/`rotate`) require the lanes to be reconverged.
`Barrier.wave()` is a no-op on lock-stepped targets but is **mandatory on Volta+
(independent thread scheduling)** and is the portable way to guarantee it. The `.sync`
on NVPTX is implicit — the lowering enforces a wave-uniform precondition.

**2. `Barrier.workgroup()` around shared memory.** Every thread in the block waits;
the canonical pattern is stage-into-shared → `Barrier.workgroup()` → read-neighbours.
The barrier must be reached **uniformly** (don't put it inside a divergent `if`).

**3. `workgroupMemory()` / `deviceMemory()` are fences, not rendezvous** — they order &
publish writes (AcquireRelease) with *no* thread wait (a barrier minus the wait). Use to
publish data before an atomic flag store. Their optional `MemoryOrder` overload tunes the
ordering.

**4. Width-agnostic, always.** Write wave code in terms of `Wave.laneId()` and
`Wave.width()` — **never** a hardcoded 32/64. `width()` is target-known by the time it
returns (NVIDIA 32, AMD 32/64, Vulkan runtime, CPU SIMD width). `Wave.isFirstLane()`
(lane 0) is the canonical "one lane commits the result" guard.

**5. `MemoryOrder` must be a compile-time literal.** Pass `MemoryOrder.Relaxed` etc.
directly — LLVM bakes the ordering into the IR at build time, so a runtime variable will
not work. Omit it for the safe default (release/acquire; `AcqRel` on Vulkan). On Vulkan,
a device-scope `Relaxed` or `SeqCst` atomic is clamped up to `AcqRel` to stay valid.
Choose `Relaxed` for pure counters/histograms where only the final value matters;
`AcqRel` is the read-modify-write default.

## Worked example — wave-cooperative reduction, then one lane commits

```cajeta
package app;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.GpuThread;
import cajeta.xpu.Wave;
import cajeta.xpu.Barrier;
import cajeta.xpu.MemoryOrder;

public class Reduce {
    @Kernel
    public static void sum(KernelBuffer<uint32> out, KernelBuffer<uint32> in, uint32 n) {
        uint32 g = GpuThread.globalIdX();
        uint32 v = (g < n) ? in[g] : 0;

        Barrier.wave();                       // reconverge before any cross-lane op
        uint32 waveTotal = Wave.reduceSum(v); // every lane gets the same total

        if (Wave.isFirstLane()) {             // exactly one lane commits
            out.atomicAdd(0, waveTotal, MemoryOrder.Relaxed);  // atomic on KernelBuffer
        }
    }
}
```

Note: `reduceSum` returns the total to *every* lane, so the `isFirstLane()` guard avoids
adding it `width()` times. `atomicAdd` is a `KernelBuffer` method (see the buffer skill);
`MemoryOrder.Relaxed` is the literal trailing arg — correct here because only the final
sum matters.

## Shared-memory + quad notes

- Shared-memory tile reduction: `tile[t]=in[g]; Barrier.workgroup();` then a uniform
  halving loop with a `Barrier.workgroup()` each step; `if (t==0)` writes the block
  result. The barrier sits in the uniform loop body, not in the divergent `if`.
- `Quad` ops mirror `Wave` at 2×2 granularity for screen-space derivatives / tiny
  transposes / per-quad voting; the lane layout (`laneId()&~3`) is bit-identical across
  backends. They are cross-lane, so the same reconvergence guarantee as `Wave` applies.
- `Bits.count` pairs naturally with `Wave.ballotSync` — ballot to a `uint64` mask, then
  popcount it to count or index lanes.
