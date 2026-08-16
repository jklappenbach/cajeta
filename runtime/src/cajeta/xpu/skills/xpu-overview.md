---
id: xpu-overview
applies-to: [cajeta.xpu]
title: cajeta.xpu — portable XPU compute (library map & routing)
description: Routing map for cajeta.xpu — host control plane vs device intrinsics, backend selection, the capability heuristic + override, KernelBuffer ownership, and the task→entry-point table.
---

# cajeta.xpu — portable GPU/XPU compute

Write a compute kernel **once** and run it on CUDA, HIP, Vulkan, or a CPU
emulation backend. `cajeta.xpu` is two cooperating planes:

- **Host control plane** — types you instantiate and call from ordinary host
  code to allocate memory, move data, launch kernels, and synchronize:
  `KernelBuffer<T>`, `GpuStream`, `Event`, `Fence`, `Device`, `Texture2D`,
  `Image2D`, `AccelerationStructure`.
- **Device-side intrinsics** — types whose methods are only meaningful inside a
  function marked `@Kernel` / `@Device`; each lowers at its call site to a
  native instruction (no real call is emitted): `GpuThread`, `Workgroup`,
  `Wave`, `Quad`, `Bits`, `Barrier`, `RayQuery`, and
  `cajeta.xpu.xpu.CooperativeMatrix` / `CoopStage`.

If your task is "manage GPU memory / launch work / pick a path at run time," you
are in the host plane. If it is "what does each thread compute," you are writing
a `@Kernel` and using the device plane.

## Task → entry point

| Want to do | Start with |
| --- | --- |
| Allocate device memory | `heap KernelBuffer<T>(n)` (RAII ctor) |
| Choose memory residency (device/pinned/unified) | `KernelBuffer.allocate(MemoryKind.X)`; see `MemoryKind` |
| Move data host↔device (explicit) | `KernelBuffer.upload` / `.download` |
| Move data on a stream, async | `.uploadAsync` / `.downloadAsync` (pair with `MemoryKind.Pinned`) |
| Zero-copy host access (APU / pinned) | `.hostStore` / `.hostLoad` (no-op on a discrete Device buffer) |
| A sub-range of a buffer | `KernelBuffer.slice(offset, count)` — non-owning view |
| Order / wait on work | `GpuStream` (`current()`, `create()`, `sync()`, `waitFor`) |
| Cross-stream dependency, device-side | `Event` (`record`/`waitFor` via stream) |
| Host-observable completion signal | `Fence` |
| Ask the active device what it can do | `Device.supports(Capability.X)` |
| Per-thread / per-group coordinates in a kernel | `GpuThread`, `Workgroup` |
| Warp/subgroup cooperation (reduce, ballot, shuffle, scan) | `Wave` (`Quad` for 2x2) |
| Per-thread bit ops (popcount, bitreverse, rotate) | `Bits` |
| Synchronize threads / fence memory in a kernel | `Barrier` |
| Tensor-core / matrix-core GEMM tile | `cajeta.xpu.xpu.CooperativeMatrix` (+ `CoopStage` for LDS staging) |
| Ray / spatial-index query in a kernel | build `AccelerationStructure`, walk with `RayQuery` |
| Sample a read-only image in a kernel | `Texture2D` + `Sampler` |
| Write an image from a kernel | `Image2D` (`store`) |

**Not here (dead ends to avoid):**
- There is **no `Kernel` class and no host `launch()` method**. A kernel is an
  ordinary function annotated `@Kernel`; the launch syntax
  (`kernel.launch(stream, grid)(args...)`) is **compiler sugar** lowered at the
  call site, not a method you can look up. On the CPU-emulation path you may
  instead just call the kernel in a host loop over the index space.
- `GpuThread`/`Wave`/`RayQuery`/`CooperativeMatrix` do **nothing on the host** —
  calling them outside a `@Kernel` is unsupported.
- No `mmap`/pointer arithmetic on a `KernelBuffer`: host code never dereferences it;
  `buf[i]` is legal only inside a kernel.
- `RayQuery` and `Image2D` are **Vulkan-only**; `Texture2D.sample` is float-only
  (integer textures are `fetch`-only).

## Backend selection & the capability heuristic (library-wide)

The runtime picks **one** backend at the first device touch, in the order
**CUDA → HIP → Vulkan → CPU**, honoring the `CAJETA_XPU_BACKEND` env override.
You then write to capabilities, not to a backend, via a two-tier model:

- **Compile-time gate** — capability *traits* in `Capabilities` (`TensorCoreF16`,
  `WaveBallot`, `AsyncCopy`, …) used as `@Kernel<Target: Trait>` template
  constraints; they gate *which kernel compiles* for a target, resolved at
  codegen from `--xpu-backend` / `--xpu-arch`.
- **Run-time gate** — `Device.supports(Capability.X)` (a **host** query) gates
  *which path the host dispatches* on the device it actually got. The pattern:
  take the hardware fast path when supported, else fall to a portable core path
  that floors to software — same source, runs anywhere.

For acceleration structures the heuristic has an **explicit override**: `AsImpl`
(`Auto` / `Software` / `Native` / `NativeNoFloor`) passed to
`AccelerationStructure.of(...)`, plus the `CAJETA_GPU_AS_IMPL=software|native`
env var (env wins). The chosen impl is recorded on the *noun* (the AS) so the
*verb* (`RayQuery`) follows it regardless of the active backend. Likewise
`CooperativeMatrix` runs natively where the dtype/backend exposes matrix cores
and otherwise on a bit-identical software tile-matmul (a `note: [mma-tiering]`
diagnostic, not a warning, tells you which tier you got).

## Ownership & lifecycle (library-wide invariants)

- **RAII / drop chain.** `KernelBuffer`, `Texture2D`, `Image2D`,
  `AccelerationStructure` are small host handles over an *owned* device
  resource. The constructor acquires it; the destructor (`~Type()`) releases it
  at scope exit — you cannot leak VRAM by forgetting to free. Destructors are
  null-guarded and idempotent; an explicit `free()` is an early-release escape
  hatch (the `FileReader.close()` pattern), rarely needed.
- **Move-out with `#`.** Factories return `#Type` (`KernelBuffer.alloc`,
  `GpuStream.current/create`, `Event.create`, `AccelerationStructure.of`). `#x`
  transfers ownership; the moved-from handle's destructor no-ops.
- **Launch borrow (XPU-K02).** A kernel launch *borrows* each `KernelBuffer` /
  texture / image / AS argument until the next `GpuStream.sync()` ordered after
  the launch. Letting such an argument reach its drop (or an explicit `free()`)
  before that `sync()` is a **compile error**. `GpuStream` is the borrow-scope
  anchor: always `sync()` before the buffer leaves scope and before reading
  results back.
- **Views borrow their parent.** `slice()` returns a non-owning (`owned=false`)
  `KernelBuffer` sharing the parent's storage; it allocates and frees nothing. The
  parent must outlive every view (parent-outlives-view is interim lint, not yet a
  hard error). The drop chain frees the parent's memory exactly once.
- **Streams / events / fences are explicitly destroyed** (`destroy()`), not
  drop-managed; the default stream (handle 0) is a no-op to destroy.
- **Errors** surface as `XpuKernelError` (`KernelError`): a device-side
  `xpu.kernel.fail(code)` writes a per-launch status buffer, and the next
  `GpuStream.sync()` on the host throws it. Codes 0..63 are runtime faults
  (bounds checks); 64.. are user codes.

## Naming gotchas (reserved words)

The spec names collide with cajeta keywords, so the real spellings differ:
`GpuStream.current()` (not `default()`), `Event.recordOn(stream)` (not
`record`), and a kernel is `@Kernel` annotated — `launch` is call-site sugar.

## Canonical end-to-end example (SAXPY: y = a·x + y)

```cajeta
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.GpuStream;
import cajeta.xpu.GpuThread;

public class Saxpy {
    @Kernel
    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,
                             float32 a, uint32 n) {
        uint32 i = GpuThread.globalIdX();
        if (i < n) {
            y[i] = a * x[i] + y[i];     // buf[i] is legal only inside @Kernel
        }
    }

    public static void run(float32[] hx, float32[] hy, uint32 n) {
        GpuStream s #= GpuStream.current();        // per-thread default stream
        KernelBuffer<float32> x = heap KernelBuffer<float32>(n);   // RAII: allocates VRAM
        KernelBuffer<float32> y = heap KernelBuffer<float32>(n);
        x.upload(hx);
        y.upload(hy);
        saxpy.launch(s, n)(y, x, 2.0f, n);         // call-site launch sugar
        s.sync();                                  // releases the launch borrow
        y.download(hy);                            // results back to host
        // x, y device memory freed automatically at scope exit (drop chain)
    }
}
```

On the CPU-emulation backend the same `@Kernel` can be driven by a plain host
loop calling `saxpy(...)` once per index (see `test/xpu/XpuLaunchAndSaxpyTests`).

## Setup / preconditions

- Backend chosen at first device touch (`CAJETA_XPU_BACKEND` to force one);
  `CAJETA_GPU_AS_IMPL` forces the ray-query impl.
- Builds only against the cajeta-llvm fork (some Vulkan SPIR-V paths need fork
  intrinsics); ray query and storage images require the Vulkan backend.

## Downward pointers

Per-type detail lives at the class level — read those skills (and the source
docstrings) for full signatures: `KernelBuffer` (memory + slices), `GpuStream` /
`Event` / `Fence` (sync), `Texture2D` / `Image2D` / `Sampler` / `TextureFormat`
(imaging), `AccelerationStructure` / `RayQuery` / `AsImpl` (ray query),
`cajeta.xpu.xpu.CooperativeMatrix` / `CoopStage` (matrix cores), `Wave` / `Quad`
/ `Bits` / `Barrier` / `GpuThread` / `Workgroup` (device intrinsics),
`MemoryKind` / `MemoryOrder` / `Capability` / `Capabilities` (enums & traits).
