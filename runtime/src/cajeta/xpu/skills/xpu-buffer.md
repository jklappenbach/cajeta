---
id: xpu-buffer
applies-to: [cajeta/gpu/KernelBuffer]
title: KernelBuffer<T> — the device-memory access point (alloc, upload/download, slice)
description: How to allocate, fill, launch against, and free GPU device memory through KernelBuffer<T>, including MemoryKind residency, async-on-stream copies, and RAII/borrow ownership.
---

# KernelBuffer<T>

The main entry point for device memory in `cajeta.xpu`. A `KernelBuffer<T>` is a small
(16-byte) **host-side handle** — `deviceHandle` + `elementCount` (+ `owned`, `kind`) —
over storage that lives in VRAM. **This is the type you instantiate** to hold GPU data;
kernel parameters are declared as `KernelBuffer<T>` and you pass buffers at launch.

Backends tag the underlying handle (CUdeviceptr / hipDeviceptr_t / VkBuffer); from here
it is one opaque type with a length and the upload/download/free operations.

## Construct (RAII — the constructor allocates)

Idiomatic form: `heap KernelBuffer<T>(n)` allocates `n` elements of device storage in the
constructor and `~KernelBuffer()` frees it at scope exit. You do **not** call `free()`.

```cajeta
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.GpuStream;

float32[] hx = ...;                                  // host data, length n
KernelBuffer<float32> x = heap KernelBuffer<float32>(n);   // RAII ctor allocates VRAM
KernelBuffer<float32> y = heap KernelBuffer<float32>(n);
x.upload(hx);
y.upload(hy);
GpuStream s = GpuStream.current();
saxpy.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, 2.0f, n);
s.sync();                                            // releases the launch borrow
y.download(hy);
// x, y device memory freed automatically when they leave scope
```

Use `stack KernelBuffer<T>(0, n)` + `x.allocate()` for a same-scope handle (the 2-arg
ctor takes `(int64 deviceHandle, uint64 elementCount)` — pass `0` for the handle).

`public static #KernelBuffer<T> alloc(uint64 n)` — factory returning an **owned, heap**
handle that escapes the frame (it `#`-moves the buffer out). Prefer plain
`heap KernelBuffer<T>(n)` unless you need factory-style escape.

## Choose memory residency (MemoryKind)

Kind is selected **after construction via `allocate`**, not by a constructor argument —
there is no `KernelBuffer<T>(n, kind)` 2-arg ctor (the `(int64, uint64)` form is taken):

```cajeta
KernelBuffer<float32> u = heap KernelBuffer<float32>(0, n);
u.allocate(MemoryKind.Unified);   // managed, host-accessible (i32 ordinal under the hood)
u.hostStore(hx);                  // zero-copy write — no device transfer
kernel.launch(s, ...)(u, ...);
s.sync();
u.hostLoad(hx);                   // zero-copy read-back
```

`allocate(int32 kind)` sets `kind` then allocates; `allocate()` re-uses the current
kind (`Device` by default) and frees any prior handle first (call once per buffer).
See `cajeta/gpu/MemoryKind` for what `Device`/`Pinned`/`Unified` mean per backend.

## The methods that matter

- `uint64 length()` — element count.
- `void upload(T[] host)` / `void download(T[] host)` — host↔device copy on the
  **default stream**. `host`'s element count must equal `length()`.
- `void uploadAsync(T[] host, GpuStream stream)` / `void downloadAsync(...)` —
  copy ordered on `stream`, returns without blocking; completes by the next
  `stream.sync()`. `host` must stay alive until that sync. Best with `MemoryKind.Pinned`
  (the precondition for true async DMA); CPU/Vulkan copy synchronously but are
  semantically identical.
- `void hostStore(T[] host)` / `void hostLoad(T[] host)` — zero-copy memcpy in the
  shared address space for a **host-accessible** (`Pinned`/`Unified`) buffer.
  **No-ops for a `Device` buffer on a discrete GPU** — use `upload`/`download` there.
- `#KernelBuffer<T> slice(uint64 offset, uint64 count)` — returns a **non-owning view**
  over a contiguous sub-range sharing this buffer's storage (allocates/frees nothing).
- `void free()` — explicit early release; idempotent and null-guarded so the
  destructor then no-ops. Rarely needed (RAII covers the normal case).

## Host vs in-kernel indexing

`buf[i]` is **only legal inside `@Kernel`/`@Device` functions**, where it borrows the
element. On the **host** there is no indexing — move data with `upload`/`download`
(or `hostStore`/`hostLoad` for host-accessible kinds). Don't reach for an element
accessor on the host; it does not exist.

## Ownership & lifecycle

- The **handle** is governed by the drop chain; the **device memory** is an owned
  resource acquired by the constructor and released by `~KernelBuffer()` — forgetting
  `free()` does not leak VRAM.
- `#buf` move-out transfers ownership; the moved-from handle's destructor no-ops
  (both `alloc` and `slice` rely on this).
- A `slice()` **view** is `owned = false`: it borrows the parent's storage, frees
  nothing, and **dangles if the parent drops first** — the parent must outlive every
  view. (Parent-outlives-view is interim lint, not yet a hard compile error.)

## Sharp edges

- **Launch borrow (XPU-K02).** A kernel launch borrows each `KernelBuffer` argument until
  the next `GpuStream.sync()` on that stream. Letting a buffer reach its drop — or
  calling `free()` — while a launch still references it is a **compile error**. Always
  `sync()` before the buffer leaves scope or is freed.
- **No bounds checking.** `slice()` does not validate `offset + count <= length()`, and
  in-kernel `buf[i]` is unchecked — matching raw device indexing. Caller's
  responsibility.
- `slice` v1 is contiguous only — no stride / gather.
- `hostStore`/`hostLoad` silently no-op on `Device` memory; if data isn't showing up,
  you likely needed `upload`/`download` (or a `Pinned`/`Unified` allocation).

No exceptions are raised by these methods; the one enforced failure is the compile-time
launch-borrow check (XPU-K02).
