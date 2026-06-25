---
id: xpu-buffer-slice
applies-to: [cajeta/gpu/KernelBuffer.slice]
title: KernelBuffer.slice — non-owning sub-view over a parent's device storage
description: slice(offset,count) returns a borrowed KernelBuffer<T> sub-view that aliases parent VRAM; it owns nothing and frees nothing.
---

# `KernelBuffer<T>.slice(offset, count)` — borrow a sub-range

`public #KernelBuffer<T> slice(uint64 offset, uint64 count)`

Returns a **new heap `KernelBuffer<T>` view** of `count` elements starting at element
`offset`, **aliasing the parent's existing device storage**. It allocates nothing
and frees nothing — use it to upload/download through, or to hand a kernel a
contiguous sub-range. The result is **moved out** (`#`), so you take ownership of
the *handle* (it drops normally at scope exit) but **not** of the device memory it
points into.

## Ownership / lifetime — the whole point

- The returned view has `owned = false`. Its `~KernelBuffer()` / `free()` **no-op**:
  the device memory is released **exactly once, by the parent**. Never expect a
  slice to free anything.
- **The parent must outlive every view of it.** A view dangles if the parent drops
  (or is `free()`d, or moved out with `#parent`) first. Parent-outlives-view is
  *not* a hard compile error yet — it is interim lint / debug-runtime checking (the
  same borrow-soundness deferral as field-stored / captured borrows), so the
  compiler will not catch a use-after-free here. This is the sharp edge.
- The view value passed at a launch site is still subject to the launch-borrow rule
  (XPU-K02): it is borrowed by the launch until the next `GpuStream.sync()`.
- The view inherits the parent's `kind` (residency), so it is host-accessible iff
  the parent is.

## Parameters

- `offset` — start element index into the parent (element units, not bytes). Folded
  to a byte address (`offset * elementBytes()`) at the backend boundary.
- `count` — number of elements in the view; becomes the view's `length()`.

## Preconditions — caller's responsibility

v1 is a **contiguous sub-range only** (no stride / gather) with **no bounds check**
(matching in-kernel `buf[i]`). `offset + count <= parent.length()` is on you; an
out-of-range slice silently produces a bad view. Both args are `uint64`.

## What it does NOT do

- Does not copy or allocate device memory — it shares the parent's storage.
- Does not free the parent's memory (`owned = false`).
- No stride, no negative/gather indexing, no bounds validation.
- Does not deep-copy the handle's data; mutations through the view are visible in
  the parent (and vice versa) — they are the same VRAM.

## Example (with imports)

```cajeta
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.GpuStream;

// sliceFill writes the local index through whatever sub-view it is given.
public static void sliceFill(KernelBuffer<int32> b, uint32 n) {
    uint32 i = GpuThread.globalIdX();
    if (i < n) { b[i] = (int32) i; }
}

static void run() {
    uint32 n = 128;
    uint32 half = 64;
    int32[] h = heap int32[n];
    for (uint32 i = 0; i < n; i = i + 1) { h[i] = -1; }

    KernelBuffer<int32> all = heap KernelBuffer<int32>(n);   // owns the VRAM
    all.upload(h);

    KernelBuffer<int32> tail = all.slice(half, half);     // non-owning second-half view
    GpuStream s = GpuStream.current();
    sliceFill.launch(s, grid: [1], block: [64])(tail, half);
    s.sync();                                          // ends the launch borrow

    all.download(h);   // head[0..63] still -1; tail wrote 0..63 into parent[64..127]
    // `tail` drops here: no-op (owned=false). `all` drops: frees the VRAM once.
}
```

## Related

- Class skill `cajeta/gpu/KernelBuffer` — construction, RAII/drop, upload/download,
  `free()`, `MemoryKind` residency. Slice inherits all of that handle behavior;
  only the ownership flip and aliasing are slice-specific.
