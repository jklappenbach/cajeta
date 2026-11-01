# Quad (2x2) cross-lane ops: `Quad`

A **quad** is the four work-items with consecutive lane ids `[laneId() & ~3 .. +3]`.
It is the 2x2 group the hardware forms for screen-space derivatives and small-tile
cooperation. `Quad` is the finer-grained sibling of `Wave`. A wave op spans
the whole subgroup (32 / 64 lanes), and a quad op spans exactly four.

```
uint32 base  = Quad.broadcast(value, 0);   // value held by quad lane 0
uint32 swapH = Quad.swapHorizontal(value); // partner across lanes 0<->1, 2<->3
boolean hit  = Quad.any(predicate);        // true if some lane of the quad passes
```

The surface (`runtime/.../core/Quad.cajeta`):

| Method | Meaning |
|---|---|
| `broadcast(value, index)` | read `value` from quad lane `index` (`0..3`) |
| `swapHorizontal(value)` | exchange with the `laneId ^ 1` partner (0<->1, 2<->3) |
| `swapVertical(value)` | exchange with the `laneId ^ 2` partner (0<->2, 1<->3) |
| `swapDiagonal(value)` | exchange with the `laneId ^ 3` partner (0<->3, 1<->2) |
| `all(predicate)` | quad-wide AND vote (`true` iff every lane passes) |
| `any(predicate)` | quad-wide OR vote (`true` iff some lane passes) |

## What it's for

- **2x2 stencils / tiny transposes.** Exchange the four texels of a quad without
  a shared-memory round-trip. The swaps are a free in-register transpose.
- **Per-quad voting.** `all`/`any` collapse a 2x2 predicate to one bool, for
  example "are all four texels of this quad inside the mask?". This is the
  compute analogue of a fragment-shader quad reduction.
- **Derivative-style cooperation** in a compute kernel without the fragment stage.

## How it lowers — native where it exists, portable everywhere

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | `OpGroupNonUniformQuadBroadcast` / `QuadSwap` (core, `GroupNonUniformQuad`) and `OpGroupNonUniformQuadAllKHR` / `QuadAnyKHR` (`SPV_KHR_quad_control`, `QuadControlKHR` capability). |
| **AMD** | the portable form — `ds_bpermute` from lane `(laneId & ~3) + index` / `laneId ^ (dir+1)` for broadcast/swap; a wave `ballot` quad-nibble test for `all`/`any`. |
| **NVIDIA** | the same portable form over `shfl` / `vote`. |
| **CPU** | the software-wave shuffle / ballot. |

The Vulkan path needed a fork addition to the SPIR-V backend. The backend already had
`OpGroupNonUniformQuadSwap` and the `GroupNonUniformQuad` capability. Cajeta's
fork (`cajeta-spirv`) adds the `OpGroupNonUniformQuadBroadcast`,
`OpGroupNonUniformQuadAllKHR` and `OpGroupNonUniformQuadAnyKHR` opcodes, the
`QuadControlKHR` capability, the `SPV_KHR_quad_control` extension, and the
`llvm.spv.quad.{broadcast,swap,all,any}` intrinsics with GlobalISel selection, so the
Vulkan/Shader flavor reaches the ops. The OpenCL quad builtins are
`isShader()`-gated off. Like shader clock and subgroup rotate, this is
fork-carried rather than an upstream PR, because upstream has no in-tree frontend that emits
such intrinsics.

The non-Vulkan backends do not need the fork. Every quad op has a
width-agnostic default built on the existing `Wave` shuffle / ballot / `laneId`
seams over the same `laneId & ~3` quad base, so AMD / NVIDIA / CPU get quad ops
for free. The portable result is bit-identical to the Vulkan native op, which
`XpuQuadDeviceTests` cross-checks.

## Caveats

- **Full quads.** The vote (`all`/`any`) is defined over the four lanes of the
  quad. With partial occupancy an inactive lane reads as `false`. Dispatch a block
  that is a multiple of 4 with full occupancy. This matches the native ops' `RequireFullQuadsKHR`
  semantics, and the portable nibble test follows the same rule.
- **Quad layout = consecutive lanes.** A quad is `laneId & ~3 .. +3`. Both the
  Vulkan native ops and the portable default use this layout, so the same source
  is correct at any wave width.
- **Cross-lane → maximal reconvergence.** Like the `Wave` cross-lane ops, a kernel
  that uses a quad op requests maximal reconvergence on Vulkan (see
  `CajetaXPU.md §6.3`).
- **`uint32` value (v1).** `broadcast`/`swap` carry `uint32`; the vote takes and returns
  `boolean`.

---

**Rules.** `cajeta.xpu.Quad` ops are device-only (inside an `@Kernel`) and
quad-cooperative over the four lanes `laneId & ~3 .. +3`. Vulkan uses the native
`OpGroupNonUniformQuad{Broadcast,Swap}` (core) and `…Quad{All,Any}KHR`
(`SPV_KHR_quad_control`) via `cajeta-spirv` fork intrinsics
(`llvm.spv.quad.*`, fork-carried and not upstreamed). Every other backend uses the portable shuffle/ballot
form. The ops are device-verified bit-identical on RADV (native) and gfx1151
(portable) (`XpuQuadDeviceTests.{amdgpu,vulkan}QuadOpsRunOnDevice`). They are runnable in
`samples/tour/gpu` (the `quad` section). See `Wave`
(`runtime/.../core/Wave.cajeta`) for the subgroup-wide surface.
