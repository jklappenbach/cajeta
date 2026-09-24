# Device

`cajeta.xpu.Device` — the active XPU device, and host-side queries about it.
The runtime selects one backend at the first device touch (CUDA → HIP →
Vulkan → CPU, honoring `CAJETA_XPU_BACKEND`); `Device` answers questions about
whatever it got, so an app can pick a path at run time. `supports` is a host
query — call it before launching, to choose which kernel or which build to
dispatch — not a device-side op.

```cajeta
if (Device.supports(Capability.RayQueryNative)) {
    // hardware inline ray query — take the native fast path
} else {
    // same source, portable software BVH path
}
```

## Geometry follows the active backend

The geometry queries (`multiprocessorCount`, `simdsPerMultiprocessor`,
`waveSize`, `registersPerMultiprocessor`, `sharedBytesPerBlock`, ... and the
dispatch law built on them) describe the backend that will run the kernel,
which is what `activeBackend()` names. On the cpu backend that is the host:
online cores as multiprocessors (one block per worker thread), one scheduler
each, a wave of the host's SIMD lanes in 32-bit words, physical RAM,
integrated. Quantities a host has no counterpart for (register file, shared
memory, blocks per multiprocessor, clocks) answer 0 = unknown, never an
invented figure. A card that happens to be installed is not consulted, so a
cpu-backend `Autotune.deviceKey()` varies with the CPU and not with the GPU
beside it.

## Methods

| Signature | |
|---|---|
| `static boolean supports(Capability cap)` ⚑ | Whether the active device advertises `cap` natively; when false, fall to core (which floors to the portable software path) |

⚑ = `@EntryPoint`

## See also

- [KernelBuffer](KernelBuffer.md), [KernelStream](KernelStream.md) — the memory and queue handles launches use
- Source: [`runtime/src/cajeta/xpu/Device.cajeta`](../../../runtime/src/cajeta/xpu/Device.cajeta)
