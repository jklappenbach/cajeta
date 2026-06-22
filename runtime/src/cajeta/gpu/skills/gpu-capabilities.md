---
id: gpu-capabilities
applies-to: [cajeta/gpu/Device, cajeta/gpu/Capability, cajeta/gpu/Capabilities, cajeta/gpu/KernelArg]
title: GPU capability model — runtime gating, compile-time traits, KernelArg
description: How to gate GPU paths — Device.supports(Capability) at run time vs Capabilities traits at codegen vs the KernelArg marker for @Kernel params.
---

# GPU capability model

Three distinct mechanisms answer "can this run here?", at three different times. Pick by
*when* the question is answered, not by name similarity:

- **Run time, host side** — `Device.supports(Capability.X)`: ask the device the runtime
  actually selected whether it advertises feature X, then branch host code to a fast
  path or a fallback. Use for features with a working software floor (ray query).
- **Compile time / codegen** — the marker interfaces in `Capabilities` (`TensorCoreF16`,
  `WaveBallot`, …) used as a `@Kernel` template constraint `<Target: TensorCoreF16>`.
  Gates *which kernel even compiles* for a target. Use for features with no software
  floor (tensor-core MMA, wave intrinsics).
- **Type admissibility** — `KernelArg`: the marker a type must satisfy to appear as a
  `@Kernel` parameter. Not a capability at all; lives here because it also constrains
  kernels.

These do **not** overlap: `Device.supports` cannot gate compilation, and a `Capabilities`
trait cannot be queried at run time. There is no bitmask/flags type, no
`Capabilities.all()`, no device-enumeration API — `Device` is the single active device.

## Members and roles

- `Device` — `final`, **non-instantiable** (private ctor); a namespace of static host
  queries. v1 surface is one method: `static boolean supports(Capability cap)`.
- `Capability` — `enum` of runtime-probed features. v1: `RayQueryNative` (0),
  `RayQueryRtCore` (1). The **ordinal is a stable native ABI contract**
  (`__cajeta_xpu_device_supports` switches on it) — append only, never reorder.
- `Capabilities` — a file of empty **marker interfaces** (`TensorCoreF16`,
  `TensorCoreBF16`, `TensorCoreFP8`, `WaveBallot`, `WaveShuffle`, `AsyncCopy`,
  `AtomicFloatAdd`). Compile-time traits; vendor namespaces (`cajeta.xpu.nvidia`, …)
  register implementers at compiler startup per `--xpu-backend` / `--xpu-arch`.
- `KernelArg` — an empty marker interface; the canonical name the typechecker looks up to
  decide if a type may be a `@Kernel` parameter.

## How they relate

`Capability` is the *runtime* counterpart of the `Capabilities` *compile-time* traits:
traits decide which kernel binary exists; `Device.supports` decides which of the built
paths the host dispatches on the device it got. Backend selection (CUDA → HIP → Vulkan →
CPU, honoring `CAJETA_XPU_BACKEND`) happens at first device touch; `supports` reports on
whatever was chosen, so the same binary adapts at run time.

Ownership/lifecycle: nothing here crosses an ownership boundary. `Device` holds no state
and is never constructed; `Capability` is a by-value enum (resolves to its i32 ordinal at
the native call); the marker interfaces carry no data and are never instantiated. No `#`
transfers, no `close()`, no drop concerns in this component.

## Worked example — both gates together

```cajeta
import cajeta.gpu.Device;
import cajeta.gpu.Capability;
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.GpuThread;
import cajeta.gpu.TensorCoreF16;   // from Capabilities

public class Gemm {
    // Compile-time gate: this kernel only compiles for a Target with tensor cores.
    @Kernel
    public static void gemmMma<Target: TensorCoreF16>(GpuBuffer<float32> c,
                                                      GpuBuffer<float32> a,
                                                      GpuBuffer<float32> b) {
        // ... Tensor.mmaF16F32<16,16,16>(...) ...
    }

    // Runtime gate: branch the HOST dispatch on what the active device advertises.
    public static void trace() {
        if (Device.supports(Capability.RayQueryNative)) {
            // build a hardware acceleration structure; OpRayQuery on the GPU
        } else {
            // portable software BVH path — same source, runs even on the CPU backend
        }
    }
}
```

`GpuBuffer<float32>` satisfies `KernelArg`, which is why it is admissible above; so do
primitives, POD structs, `Texture<...>`, `Sampler`, and Vulkan `@PushConstant` structs.
A non-conforming type as a `@Kernel` parameter is a typecheck error. (The structural
check that admits any POD shape without an explicit annotation is not wired yet — v1 only
needs the marker to exist.)

## Gotchas

- `Device.supports` is a **host** query — calling it inside an `@Kernel`/`@Device` body
  is wrong; it chooses *which* kernel to launch, not device-side control flow.
- `RayQueryNative` false does **not** mean ray query is unavailable — the `RayQuery` path
  still runs via the software BVH walk; `supports` only picks the fast path.
- A `Capabilities` trait used outside a `@Kernel` `<Target: …>` constraint does nothing —
  the bodies are empty markers, resolved only at codegen.
- Adding a capability: append the enum constant **and** the matching probe in
  `runtime/native/cajeta_runtime.c`; never insert in the middle (ordinal is the ABI).

For the buffer types that flow through kernels see `cajeta/gpu/GpuBuffer`; for the
device-side thread/coordinate ops inside a kernel see `cajeta/gpu/GpuThread`.
