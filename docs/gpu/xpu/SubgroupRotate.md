# Cross-lane cyclic shift: `Wave.rotate`

`Wave.rotate(value, delta)` is a wave-cooperative cyclic shift: each lane reads
`value` from the lane `delta` positions ahead, modulo the wave width —
i.e. from lane `(laneId() + delta) mod width()`.

```
uint32 mine = ...;
uint32 ahead = Wave.rotate(mine, 1);   // my right neighbour's value (wrapping)
```

It's the neighbour-exchange / stencil primitive: shifting a value one lane over
is the building block of wave-level prefix patterns, sliding windows, and
butterfly communication, without a round-trip through shared memory.

## What it's for

- **Stencils across lanes** — each lane needs its neighbour's datum (finite
  differences, 1-D convolution windows mapped onto a wave).
- **Rotating a wave-held tile** — cyclic permutation of per-lane registers.
- **Hand-rolled scans** — combine `rotate`/`shuffle` with an operator to build a
  prefix without shared memory.

## How it lowers — native where it exists, portable everywhere

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | a single **`OpGroupNonUniformRotateKHR`** at Subgroup scope (`SPV_KHR_subgroup_rotate`, `GroupNonUniformRotateKHR` capability). |
| **AMD** | `ds_bpermute` — the divergent-index intra-wave gather (each lane reads from `((laneId+delta) mod width)*4`). |
| **NVIDIA** | `shfl.sync` with a per-lane source index. |
| **CPU** | the software-wave shuffle. |

The Vulkan path needed a **fork addition** to the SPIR-V backend: the opcode, the
`GroupNonUniformRotateKHR` capability, and the `SPV_KHR_subgroup_rotate`
extension all already exist, but the only emission path was the OpenCL
`sub_group_rotate` builtin, gated off for the Vulkan/Shader flavor. cajeta's fork
adds the `llvm.spv.subgroup.rotate` intrinsic + GlobalISel selection so the
Shader flavor reaches the op (the shader-clock / ray-query / cooperative-matrix
pattern). Like shader clock, this is **fork-carried, not an upstream PR** —
upstream has no in-tree frontend that would emit such an intrinsic.

The non-Vulkan backends do **not** need the fork: `rotate` has a width-agnostic
default built on the existing `laneId`/`width`/shuffle seams, so NVIDIA/AMD/CPU
get it for free. (AMD overrides the default to `ds_bpermute` because its
`shuffleSync` is `readlane`, which requires a *uniform* source lane — rotate's
source is per-lane divergent.)

## Caveats

- **Width-relative.** The rotation wraps at the hardware wave width (32 or 64 on
  a GPU, the host SIMD width on CPU). Write width-agnostic code: a rotate by
  `delta` reads `(laneId + delta) mod width`, never an absolute lane.
- **Cross-lane → maximal reconvergence.** Like `shuffle`/`ballot`/`reduce`, a
  kernel that uses `rotate` requests maximal reconvergence on Vulkan so the op
  sees the source-converged lanes (see `CajetaXPU.md §6.3`).
- **`uint32` value (v1).** `value` and `delta` are `uint32`.

---

**Rules.** `Wave.rotate(value, delta) -> uint32` is device-only (inside an
`@Kernel`), wave-cooperative, reading from lane `(laneId + delta) mod width`.
Native `OpGroupNonUniformRotateKHR` on Vulkan (a `cajeta-spirv` fork intrinsic,
`llvm.spv.subgroup.rotate` — fork-carried, not upstreamed); `ds_bpermute` /
`shfl` / software-shuffle elsewhere. Device-verified bit-identical on RADV
(native) and gfx1151 (ds_bpermute) (`XpuWaveDeviceTests.*RotateRunsOnDevice`).
Runnable in `samples/tour/gpu` (the `waveRotate` section). See `Wave`
(`runtime/.../core/Wave.cajeta`) for the rest of the wave surface.
