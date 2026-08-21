# AMD `coopMatrixTier` classifies tiles in isolation, silently removing the
# all-f32 and all-bf16 GEMMs from every AMDGPU build

**Filed 2026-08-20** (found by cajeta-llama Unit 15's first attempt to run
the engine on real silicon; affects any `--xpu-backend=amdgpu` build whose
reachable code contains a `CooperativeMatrix` GEMM at f32 or narrow bf16 —
`cajeta.math.Ewise.matmulF32` and `Ewise.matmulBf16` today).

## Symptom

Building anything that reaches `Ewise.matmulF32` for AMDGPU emits, and
then omits the kernel entirely:

```
note: [xpu-kernel-skipped] matmulF32: no amdgpu device code — XPU kernel
lowering: unsupported construct — CooperativeMatrix.mma: a native
accumulator cannot consume software-tier operands — give all three tiles
the same dtype tier
note: [xpu-kernel-skipped] matmulBf16: no amdgpu device code — …: a
software accumulator cannot consume native-tier operands — …
```

The build SUCCEEDS. `Ewise.matmulF32Op` routes on operand placement
(`Placement.pair`), so a host-resident call still computes correctly via
the scalar fallback and nothing looks wrong until a caller places both
operands on device and expects the GEMM to exist.

## Cause

`AmdgpuKernelLowering::coopMatrixTier(elem, rows, cols, use)` decides a
tier from ONE tile's `(dtype, use)`:

| tile role | Native for | Portable otherwise |
|---|---|---|
| accumulator (`use == 2`) | `f32`, `i32` | — |
| A/B operand (`use == 0/1`) | `f16`, `bf16`, `i8` | — |

The role-awareness is deliberate and correct for the iu8 path — the
comment explains it keeps an int32 *operand* portable while an int32
*accumulator* is native, "the two share an LLVM type but not a config."

The gap is that a tier is a property of the **combination**, not of a
tile. An f32 accumulator is native only *as the accumulator of an
f16/bf16 GEMM*; there is no f32xf32 WMMA. Classified alone it reports
Native, the f32 A/B operands report Portable, the tiles straddle, and
`KernelLowering`'s `mma` guard rejects the kernel. Symmetrically, a
narrow bf16 accumulator reports Portable while bf16 operands report
Native.

Enumerating every combination the stdlib uses makes the pattern exact —
two of six straddle, and both are the all-one-dtype shapes:

| kernel | operands | accumulator | tiers | result |
|---|---|---|---|---|
| `matmulF16` | f16 | f32 | Native + Native | lowers |
| `matmulBf16Wide` | bf16 | f32 | Native + Native | lowers |
| `matmulI8` | int8 | int32 | Native + Native | lowers |
| `matmulF64` | f64 | f64 | Portable + Portable | lowers |
| **`matmulF32`** | **f32** | **f32** | **Portable + Native** | **SKIPPED** |
| **`matmulBf16`** | **bf16** | **bf16** | **Native + Portable** | **SKIPPED** |

## Why this is a defect and not the guard working as intended

`Ewise.matmulF32`'s own contract says the opposite of what happens:

> …no native matrix-core config, so it takes the **portable software
> tile-matmul tier** (a `note: [mma-tiering]`) — bit-identical to the CPU
> floor, and the SAME kernel auto-promotes to the hardware cores for the
> configs a backend exposes (f16/bf16/int8 WMMA on AMD).

That is precisely the designed behaviour: no native config → portable
tile → still runs on the GPU. The per-tile classifier prevents the
degrade it documents, and does so silently.

The mixed-tier guard itself is right and should stay: `XpuSpirvMixedTierTests`
pins graceful skipping in both straddle directions, and a genuinely mixed
kernel (f64 acc over f16 operands) has no meaning. The bug is upstream of
the guard — the tiers handed to it are wrong for these two shapes.

## Fix sketch

Decide the tier for a GEMM's tiles together rather than per tile: when the
A/B operand dtype has no native config on this target, the accumulator must
be Portable too, regardless of its own dtype (and symmetrically, a Portable
accumulator forces Portable operands). Options:

1. Give `coopMatrixTier` the operand dtype alongside the accumulator's, so
   the accumulator case can answer "native only if the operands are."
2. Resolve tiers for the three slots of an `mma` as a group in
   `KernelLowering`, demoting all three to Portable when any is Portable —
   this also subsumes the current guard for the legitimately-mixed cases.

(2) is the smaller change and keeps every backend's table as-is.

## Workaround until fixed

`CAJETA_GPU_COOPMATRIX_IMPL=software` forces the portable tile for ALL
cooperative matrices, so all three tiles agree and every kernel lowers.
This is the documented degrade seam ("validates the portable tier against
the native one on real silicon") — it costs the native WMMA path for the
kernels that would otherwise get it (f16/bf16-wide/int8).

## Acceptance

- An `--xpu-backend=amdgpu` build reaching `Ewise.matmulF32` emits NO
  `[xpu-kernel-skipped]` note, and emits the `[mma-tiering]` portable note
  instead, with no `CAJETA_GPU_COOPMATRIX_IMPL` override set.
- The same for `Ewise.matmulBf16`.
- `matmulF16` / `matmulBf16Wide` / `matmulI8` still take the NATIVE path on
  AMD (no `[mma-tiering]` note) — the fix must not demote the shapes that
  legitimately have a config. `AmdgpuCoopI8Tests` already pins this.
- A genuinely meaningless straddle still skips gracefully — the
  `XpuSpirvMixedTierTests` discipline, extended to AMD.
