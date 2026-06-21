---
id: gpu-memory
applies-to: [cajeta/gpu/GpuBuffer, cajeta/gpu/MemoryKind, cajeta/gpu/AddressSpace]
title: GPU device memory — GpuBuffer storage, MemoryKind residency, address-space refs
description: How to allocate, transfer, and reference device memory: GpuBuffer handles, MemoryKind residency, and Global/Shared/Constant/Private/Generic kernel refs.
---

# GPU device memory

Three cooperating pieces. **`GpuBuffer<T>`** is the host-side handle you allocate
and move data through. **`MemoryKind`** picks where that storage lives (Device /
Pinned / Unified), chosen at allocation. **`AddressSpace`** (`Global`/`Shared`/
`Constant`/`Private`/`Generic`) are the qualified-reference markers used *inside*
kernel code, not on the host.

Decide first:
- Need device memory the host fills then reads back → make a `GpuBuffer<T>`.
- Choosing between explicit copies vs zero-copy → that is a `MemoryKind` decision (below).
- Declaring kernel parameters / workgroup tiles → that is `AddressSpace` (below).

## Members and roles

- **`GpuBuffer<T>`** (entry point you construct). A 16-byte handle —
  `{deviceHandle, elementCount, owned, kind}` — over device storage that lives in
  VRAM. Owns its device memory by RAII. This is the cross-cutting type every
  higher layer (cajeta.math, Torch, cajeta.render) passes to kernels.
- **`MemoryKind`** (enum, ordinal-is-contract). `Device=0`, `Pinned=1`,
  `Unified=2`. The ordinal is the *stable native contract* — the
  `__cajeta_xpu_buffer_*` intrinsics switch on it to pick the per-backend
  allocator/deallocator. Never reorder.
- **`Global<T>`, `Shared<T>`, `Constant<T>`, `Private<T>`, `Generic<T>`**
  (address-space qualified references, v1 empty markers). They are *not* pointer
  types — they are references with normal Cajeta borrow semantics that the
  compiler lowers to `addrspace(N)`. You do not instantiate them in host code;
  the launch-site lowering does. Inside a kernel `Shared<T> tile = shared T[N];`
  declares a workgroup tile.

## How they collaborate

`GpuBuffer<T>` *is* the host-side façade over a `Global`-address-space allocation:
a `GpuBuffer<float32>` kernel parameter lowers to a global-memory reference inside
the kernel, and indexing `buf[i]` there borrows the element. `MemoryKind` is a
field of the buffer (`kind`) set at allocation and threaded into every native
alloc/free/host-copy call, so it decides whether the host uses the explicit
`upload`/`download` path or the zero-copy `hostStore`/`hostLoad` path. The
address-space markers are otherwise independent of `GpuBuffer` — you reach for
`Shared`/`Constant`/`Private` only when writing kernel bodies.

## Construction, residency, lifecycle (ownership)

Allocate one of two ways:

```
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.MemoryKind;

GpuBuffer<float32> x = heap GpuBuffer<float32>(n);     // RAII ctor: allocates, kind=Device
// or, to choose residency:
GpuBuffer<float32> u = heap GpuBuffer<float32>(0, n);  // 2-arg ctor: no allocation yet
u.allocate(MemoryKind.Unified);                        // allocate with chosen kind
```

There is **no** `GpuBuffer<T>(n, MemoryKind.Unified)` constructor — residency is
chosen via `allocate(MemoryKind)`, taken as a separate overload from the 2-arg
`(int64 deviceHandle, uint64 elementCount)` form. The factory
`GpuBuffer<T>.alloc(n)` returns a `#`-moved heap handle (Device kind) for
escaping a frame.

**Ownership / lifecycle (the load-bearing part):**
- The device memory is an *owned resource* released by `~GpuBuffer()` via the
  drop chain at scope exit — forgetting to free no longer leaks VRAM. `free()`
  exists as an idempotent early-release escape hatch; it is rarely needed.
- A **launch borrows** every `GpuBuffer` argument until the next
  `GpuStream.sync()`. Letting a buffer reach its drop (or calling `free()`) while
  a launch still references it is a **compile error, XPU-K02**.
- Move-out `#buf` transfers ownership; the moved-from handle's destructor no-ops.
- `slice(offset, count)` returns a non-owning view (`owned = false`) sharing the
  parent's storage — it allocates and frees nothing. The **parent must outlive
  every view** (a view dangles if the parent drops first); parent-outlives-view
  is interim lint/debug-runtime territory, not yet a hard compile error.

## Picking a MemoryKind

- **`Device`** (default) — device-local VRAM; move data with explicit
  `upload(host)` / `download(host)`. Fastest for device-resident compute on a
  discrete GPU. `hostStore`/`hostLoad` **no-op** on a Device buffer — use
  `upload`/`download`.
- **`Pinned`** — page-locked host memory; the precondition for true async DMA
  (`uploadAsync`/`downloadAsync`). Host-accessible via `hostStore`/`hostLoad`.
- **`Unified`** — managed memory one pointer addresses from both host and device;
  genuine zero-copy on an integrated GPU/APU (no transfer, no migration). Use
  `hostStore`/`hostLoad`, not `upload`/`download`.

Backends: CUDA/HIP honour all three. On Vulkan every buffer is already
host-visible+coherent (kind collapses to one path); on the CPU "device" memory is
host memory. Where a driver lacks the managed/pinned entry point it falls back to
plain device memory — correct, just without zero-copy.

## Cross-class call sequence (host → device → host)

```
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.GpuStream;

float32[] hx = heap float32[n];
float32[] hy = heap float32[n];
// ... fill hx, hy ...

GpuBuffer<float32> x = heap GpuBuffer<float32>(n);   // Device, allocates
GpuBuffer<float32> y = heap GpuBuffer<float32>(n);
x.upload(hx);                                          // host -> device
y.upload(hy);

GpuStream s = GpuStream.current();
saxpy.launch(s, grid: [4], block: [256])(y, x, 2.0f, n);  // borrows x, y until sync
s.sync();                                             // borrow ends here
y.download(hy);                                        // device -> host
// x, y device memory freed automatically at scope exit (drop chain)
```

The matching kernel — `GpuBuffer<T>` params are global memory, indexing borrows
the element; a `Shared<T>` tile declares workgroup storage:

```
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.GpuThread;

@Kernel
public static void saxpy(GpuBuffer<float32> y, GpuBuffer<float32> x,
                         float32 a, uint32 n) {
    uint32 i = GpuThread.globalIdX();
    if (i < n) { y[i] = a * x[i] + y[i]; }   // buf[i] legal only in @Kernel/@Device
}
```

Host array element count must equal `length()` for every transfer. Indexing
`buf[i]` is legal **only** inside `@Kernel`/`@Device` functions; on the host use
the explicit transfer methods.

## What this does NOT do

- No host-side `buf[i]` indexing — kernel/device context only.
- `GpuBuffer` has no global `init`/registry; `GpuStream` and the launch protocol
  live elsewhere (`cajeta/gpu/GpuStream`, the `@Kernel` launch lowering).
- The address-space classes have **empty v1 bodies** — no methods to call; they
  exist for the compiler to recognize by canonical name and emit `addrspace(N)`.
  `slice` is contiguous-only (no stride/gather) and does no bounds check.
