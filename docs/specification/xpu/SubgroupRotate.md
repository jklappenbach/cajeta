# Cross-lane cyclic shift: `Wave.rotate`

`Wave.rotate(value, delta)` is a wave-cooperative cyclic shift. Each lane reads
`value` from the lane `delta` positions ahead, modulo the wave width, which is
lane `(laneId() + delta) mod width()`.

```
uint32 mine = ...;
uint32 ahead = Wave.rotate(mine, 1);   // my right neighbour's value (wrapping)
```

It is the neighbour-exchange and stencil primitive. Shifting a value one lane over
is the building block of wave-level prefix patterns, sliding windows, and
butterfly communication, without a round-trip through shared memory.

## What it's for

- **Stencils across lanes.** Each lane needs its neighbour's datum, as in finite
  differences and 1-D convolution windows mapped onto a wave.
- **Rotating a wave-held tile.** A cyclic permutation of per-lane registers.
- **Hand-rolled scans.** Combine `rotate`/`shuffle` with an operator to build a
  prefix without shared memory.

## How it lowers — native where it exists, portable everywhere

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | a single **`OpGroupNonUniformRotateKHR`** at Subgroup scope (`SPV_KHR_subgroup_rotate`, `GroupNonUniformRotateKHR` capability). |
| **AMD** | `ds_bpermute` — the divergent-index intra-wave gather (each lane reads from `((laneId+delta) mod width)*4`). |
| **NVIDIA** | `shfl.sync` with a per-lane source index. |
| **CPU** | the software-wave shuffle. |

The Vulkan path needed a fork addition to the SPIR-V backend. The opcode, the
`GroupNonUniformRotateKHR` capability, and the `SPV_KHR_subgroup_rotate`
extension all already exist, but the only emission path was the OpenCL
`sub_group_rotate` builtin, which is gated off for the Vulkan/Shader flavor. Cajeta's fork
adds the `llvm.spv.subgroup.rotate` intrinsic and its GlobalISel selection so the
Shader flavor reaches the op. This is the shader-clock, ray-query and cooperative-matrix
pattern. Like shader clock, this is fork-carried rather than an upstream PR, because
upstream has no in-tree frontend that would emit such an intrinsic.

The non-Vulkan backends do not need the fork. `rotate` has a width-agnostic
default built on the existing `laneId`/`width`/shuffle seams, so NVIDIA/AMD/CPU
get it for free. AMD overrides the default to `ds_bpermute` because its
`shuffleSync` is `readlane`, which requires a uniform source lane, and rotate's
source is per-lane divergent.

## Caveats

- **Width-relative.** The rotation wraps at the hardware wave width (32 or 64 on
  a GPU, the host SIMD width on CPU). Write width-agnostic code. A rotate by
  `delta` reads `(laneId + delta) mod width`, never an absolute lane.
- **Cross-lane → maximal reconvergence.** Like `shuffle`/`ballot`/`reduce`, a
  kernel that uses `rotate` requests maximal reconvergence on Vulkan so the op
  sees the source-converged lanes (see `CajetaXPU.md §6.3`).
- **`uint32` value (v1).** `value` and `delta` are `uint32`.

---

**Rules.** `Wave.rotate(value, delta) -> uint32` is device-only (inside an
`@Kernel`) and wave-cooperative, reading from lane `(laneId + delta) mod width`.
Vulkan uses the native `OpGroupNonUniformRotateKHR` through a `cajeta-spirv` fork intrinsic,
`llvm.spv.subgroup.rotate`, which is fork-carried and not upstreamed. Every other backend uses `ds_bpermute`,
`shfl` or the software shuffle. The op is device-verified bit-identical on RADV
(native) and gfx1151 (ds_bpermute) (`XpuWaveDeviceTests.*RotateRunsOnDevice`).
It is runnable in `samples/tour/gpu` (the `waveRotate` section). See `Wave`
(`runtime/.../core/Wave.cajeta`) for the rest of the wave surface.
