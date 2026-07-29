# KernelBuffer\<T\>

`cajeta.xpu.KernelBuffer` — unified handle to device memory, the cross-cutting
type higher-level libraries write against. Backends tag the underlying storage
handle (CUdeviceptr / hipDeviceptr_t / VkBuffer); from the user's perspective
it is one opaque type with a length and the standard upload/download/free
operations. The device memory is an owned resource tied to the handle's
lifetime via RAII: the constructor acquires it and `~KernelBuffer()` releases
it at scope exit, so forgetting to `free()` cannot leak VRAM. A launch borrows
each `KernelBuffer` argument until the next `KernelStream.sync()`; letting a
buffer reach its drop (or an explicit `free()`) while a launch still
references it is a compile error (XPU-K02). Indexing `buf[i]` is only legal
inside `@Kernel` or `@Device` functions; on the host, use the explicit
upload/download paths.

```cajeta
uint32 n = 1024;
float32[] hx = heap float32[n];
KernelBuffer<float32> x = heap KernelBuffer<float32>(n);   // allocates device memory
x.upload(hx);
// ... launch kernels that read/write x, then stream.sync() ...
x.download(hx);
// device memory freed automatically when x drops at scope exit
```

## Methods

| Signature | |
|---|---|
| `KernelBuffer(uint64 elementCount)` ⚑ | RAII constructor: allocate device storage for `elementCount` elements — the idiomatic form |
| `static #KernelBuffer<T> alloc(uint64 n)` ⚑ | Allocate a device buffer of `n` elements and return a heap handle (factory-style escape) |
| `uint64 length()` | Element count |
| `void allocate()` | Allocate device storage for this buffer's `length()` elements (pairs with direct construction) |
| `void allocate(int32 kind)` | Allocate with a chosen memory residency (a `MemoryKind` ordinal: `Device`, `Pinned`, `Unified`) |
| `void upload(T[] host)` | Host → device transfer; the host array's element count must equal `length()` |
| `void download(T[] host)` | Device → host transfer |
| `void uploadAsync(T[] host, KernelStream stream)` | Asynchronous host → device upload, enqueued on `stream`; completes by the next `stream.sync()` |
| `void downloadAsync(T[] host, KernelStream stream)` | Asynchronous device → host download, the twin of `uploadAsync` |
| `void hostStore(T[] host)` | Zero-copy host → buffer write for a host-accessible buffer (`MemoryKind.Unified` / `.Pinned`); no device transfer |
| `void hostLoad(T[] host)` | Zero-copy buffer → host read, the twin of `hostStore` |
| `void free()` | Explicit early release — idempotent escape hatch; the destructor then no-ops |
| `#KernelBuffer<T> slice(uint64 offset, uint64 count)` | Non-owning sub-buffer view over `count` elements starting at `offset`; shares this buffer's storage, which must outlive the view |

⚑ = `@EntryPoint`

## See also

- Tour: [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- [KernelStream](KernelStream.md) — the borrow-scope anchor for launched buffers
- [Device](Device.md) — host-side capability queries
- Source: [`runtime/src/cajeta/xpu/KernelBuffer.cajeta`](../../../runtime/src/cajeta/xpu/KernelBuffer.cajeta)
