# Wave reductions: `Wave.reduce{Sum,Max,Min,And,Or,Xor}`

A wave reduction combines one `uint32` per lane into a single value across all
active lanes of the wave; **every lane receives the same result**. It's the
wave-cooperative fold — the per-wave partial of a global sum/max, a vote tally,
a flag-merge — done in registers, without shared memory.

```
uint32 total = Wave.reduceSum(x);   // sum  across the wave
uint32 hi    = Wave.reduceMax(x);   // max  (unsigned)
uint32 lo    = Wave.reduceMin(x);   // min  (unsigned)
uint32 all   = Wave.reduceAnd(x);   // bitwise AND
uint32 any   = Wave.reduceOr(x);    // bitwise OR
uint32 par   = Wave.reduceXor(x);   // bitwise XOR
```

## What it's for

- **Hierarchical reductions** — each wave reduces its lanes in registers, then
  one lane per wave commits the partial (with `Wave.isFirstLane`) to a smaller
  cross-wave pass. The standard fast reduction shape.
- **`reduceMax`/`reduceMin`** — the wave-level step of softmax / normalization /
  bounding-box passes (the ML-relevant reductions).
- **`reduceOr`/`reduceAnd`** — "did any lane …" / "did every lane …" flag merges;
  pair with `Wave.ballotSync` for masks.
- **`reduceXor`** — parity / checksum folds.

## How it lowers — native on every backend

`Max`/`Min` are **unsigned** (the `uint32` surface). Each reduction is a single
hardware instruction on every backend — there is **no fork**: these use the
`GroupNonUniformArithmetic` family, which is already reachable from the
Vulkan/Shader flavor (the same path `reduceSum` uses).

| Op | Vulkan | AMD | NVIDIA | CPU |
|---|---|---|---|---|
| Sum | `OpGroupNonUniformIAdd` | `wave.reduce.add` | `redux.sync.add` | VFABI add-reduce |
| Max | `OpGroupNonUniformUMax` | `wave.reduce.umax` | `redux.sync.umax` | VFABI umax-reduce |
| Min | `OpGroupNonUniformUMin` | `wave.reduce.umin` | `redux.sync.umin` | VFABI umin-reduce |
| And | `OpGroupNonUniformBitwiseAnd` | `wave.reduce.and` | `redux.sync.and` | VFABI and-reduce |
| Or | `OpGroupNonUniformBitwiseOr` | `wave.reduce.or` | `redux.sync.or` | VFABI or-reduce |
| Xor | `OpGroupNonUniformBitwiseXor` | `wave.reduce.xor` | `redux.sync.xor` | VFABI xor-reduce |

(NVIDIA `redux.sync` requires sm_80+. The CPU "wave" is the host SIMD vector; the
reduction runs in the kernel's vectorized VFABI variant.)

**Why not `SPV_KHR_uniform_group_instructions`.** That OpenCL-only extension
(`OpGroupIMulKHR` …) was the obvious candidate, but its emission path is
`isShader()`-gated off, so it never reaches the Vulkan flavor. The
`GroupNonUniformArithmetic` Reduce family gives the same reductions, is
non-uniform-control-flow-safe, and is already Shader-reachable — so cajeta uses
it and needs no fork.

**Product is intentionally absent.** Neither AMD `wave.reduce` nor NVIDIA
`redux.sync` has a multiply reduction, so `reduceProduct` would need a
shuffle-tree fallback on those backends — a follow-on, not part of v1.

## Caveats

- **Consume the result unconditionally.** A reduction whose result is used only
  under divergent control flow can be predicated on the CPU VFABI path so the
  reduce sees only the active lanes. Store it for every lane (or use it before
  branching). On the GPU the op is genuinely wave-wide regardless.
- **Cross-lane → maximal reconvergence.** Like `shuffle`/`ballot`/`rotate`, a
  reducing kernel requests maximal reconvergence on Vulkan (see
  `CajetaXPU.md §6.3`).
- **`uint32` (v1).** Values are `uint32`; `Max`/`Min` are unsigned.

---

**Rules.** `Wave.reduce{Sum,Max,Min,And,Or,Xor}(uint32) -> uint32` are
device-only, wave-cooperative, every lane receiving the same result; `Max`/`Min`
unsigned. Native on Vulkan/AMD/NVIDIA/CPU (GroupNonUniformArithmetic /
wave.reduce / redux.sync / VFABI) — no fork, no extension. Device-verified
bit-exact on RADV + gfx1151 (`XpuWaveDeviceTests.*ReduceFamily*`); CPU exercised
in `samples/Tour/xpu` (`waveReduceOps`). `reduceProduct` is a documented
follow-on (no AMD/NVIDIA hardware multiply-reduce).
