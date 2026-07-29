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

## Methods

| Signature | |
|---|---|
| `static boolean supports(Capability cap)` ⚑ | Whether the active device advertises `cap` natively; when false, fall to core (which floors to the portable software path) |

⚑ = `@EntryPoint`

## See also

- [KernelBuffer](KernelBuffer.md), [KernelStream](KernelStream.md) — the memory and queue handles launches use
- Source: [`runtime/src/cajeta/xpu/Device.cajeta`](../../../runtime/src/cajeta/xpu/Device.cajeta)
