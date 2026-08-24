# CooperativeMatrix epilogues: the accumulator must be touchable

**Status: ACTIVE — Option B DECIDED (Julian, 2026-08-24).** The two
fused verbs (§4) are the feature; Option A (§3) stays recorded as the
eventual general successor and is NOT being built now. Plan:
`agents/xpu-coopmatrix-epilogue-plan.md`. Filed from cajeta-llama's
threaded-forward-path 10.12.20, where this is the measured remaining
lever on the prefill GEMM.

**v1 backend decision** (narrowing §4.2): on the Vulkan native tier the
verbs do not get the scratch-spill fallback yet — the tier scan DEMOTES
the kernel's tiles to the portable tile (the straddle machinery, with
its note), which is correct and loud. The in-tier SPIR-V fallback is
future work if a Vulkan consumer appears.

## 1. Definition — the problem

A quantized GEMM is not `C = A·B`. It is `C = f(A·B)` where `f` applies
per-row and per-column scale factors that live outside the matrix — for
k-quants, a per-32-element (Q4_K) or per-16-element (Q6_K) sub-block
scale per OUTPUT ROW, times a per-token activation scale per COLUMN. The
matrix cores produce an int32 accumulator tile; `f` needs to touch its
elements, multiply each by `rowScale[r] * colScale[c]`, and accumulate
into a float running sum that persists across the K loop.

`cajeta.xpu.CooperativeMatrix` cannot express `f`. The accumulator is
OPAQUE — `load`/`splat`/`mma`/`store` are the whole surface — so the only
way to touch an element is to `store` the tile to LDS, barrier, apply the
scales with ordinary scalar code, barrier again, and `splat` the
accumulator back to zero for the next sub-block. cajeta-llama's Q4_K
prefill kernel pays that seam **8 times per 256-element block per output
tile** (16 barriers, 8 spills); the Q6_K twin pays it 16 times. This is
not a stylistic cost:

- The prefill GEMM measures **2069 ms of a 2873 ms prefill** (72%) at
  512 tokens on gfx1151, ~2.0 TMAC/s effective against a validated
  27 TMAC/s WMMA ceiling.
- The B-feed was ELIMINATED (10.12.16's tile-major resident int8 copy)
  and bought only 160 ms of 3015 — the feed was never the bottleneck.
- Widening the tile per workgroup was REFUTED on occupancy (10.12.15).
- llama.cpp's MMQ achieves **9.3 TMAC/s on the same silicon** doing the
  same Q4_K arithmetic, and its kernel shows exactly what closes the
  gap: scales pre-folded to per-(row, sub-block) half2 at stage time,
  then applied **per accumulator fragment element in registers** —
  `sum[..] += dmA.x*dsB.x*C.x[l]`, the min term folded into the same
  line — zero shared-memory spills, zero barriers, no re-splat
  (`mmq.cuh`, `vec_dot_q8_1_q8_1_mma`; verified in source at 5306f4b).

Their kernel can write that line because CUDA's `mma.h` tiles expose
`C.x[l]` with `get_i(l)`/`get_j(l)`. Ours cannot, **by API, not by
hardware**: cajeta's amdgpu backend marshals WMMA fragments by hand and
documents the exact mapping (`AmdgpuKernelLowering.cpp:891` — the
accumulator is `<8 x i32|f32>` per lane; lane L holds column `L & 15`,
rows `{2e + (L>>4)}`). The compiler knows where every element lives; the
language just provides no way to say "this element, times this scale".

The fix is additive API on `CooperativeMatrix`. Two shapes are on the
table; they are not mutually exclusive, but one should be built first.

### 1.1 What either option unlocks (the target kernel shape)

Per output tile, per 256-element block: header decode (LDS, 1 barrier) →
one `mma` per K-slice into the int accumulator → epilogue INTO A
REGISTER-RESIDENT float32 accumulator fragment → after the K loop, one
`facc.store` straight to global (the store-side addressing already
exists). LDS spills per block: **0** (from 8–16). Barriers per block:
**1** (from 17–18). The f32 accumulator costs 8 VGPRs/lane next to the
int accumulator's 8 — well inside budget at one wave per workgroup, and
it removes the LDS `acc`/`ct` arrays (2 KB) entirely.

## 2. Requirements

- **R1 — the seam in registers on amdgpu-native.** Apply
  `facc[r][c] += rowF[r] * colF[c] * acc[r][c]` (int32 `acc`, float32
  `facc`) with no workgroup-shared round trip and no barrier, on the
  native WMMA tier.
- **R2 — additive.** No change to existing
  `load`/`splat`/`mma`/`store` semantics or to any existing kernel.
- **R3 — every backend lowers or refuses LOUDLY.** Per the
  `amdgpu-coopmatrix-tier-straddle` lesson: a construct a backend cannot
  serve must demote the kernel to the portable tier (with the
  `[mma-tiering]` note) or fail the tier scan with a named diagnostic —
  never skip silently. Only amdgpu-native must be FAST; a portable
  fallback that spills to a scratch tile is semantically fine.
- **R4 — a float accumulator fragment persists across the K loop** in
  registers and stores to global at the end. (Already expressible:
  `CooperativeMatrix<float32,16,16,2>` + `splat` + `store`; R1's op is
  what writes into it.)
- **R5 — bit-agreement with the spill epilogue.** For the same element,
  the arithmetic is one fma chain either way; a native epilogue must be
  bit-identical to the reference LDS-spill kernel on the same inputs.
  This is the acceptance test's teeth (the 10.12.16 discipline: exact
  equality, not tolerance, when only the data path moves).
- **R6 — the mapping stays compiler-private OR is exposed only through
  queries.** No user kernel may hard-code lane→element geometry; a
  kernel that compiles must be correct on every backend that accepts it.

## 3. Option A — fragment element access (the general primitive)

The CUDA-tile shape: expose the lane's slice of the fragment, with index
queries so user code never assumes the mapping.

```cajeta
public final class CooperativeMatrix<T, uint32 Rows, uint32 Cols, uint32 Use> {
    /** Elements of this tile held by THIS lane (compile-time constant
     *  per backend/tier: 8 on amdgpu-native 16x16, Rows*Cols on the
     *  software tier's flat tile). */
    public uint32 elems();
    /** This lane's e-th element. */
    public T get(uint32 e);
    public void set(uint32 e, T v);
    /** The GLOBAL row/col this lane's e-th element sits at. */
    public uint32 rowOf(uint32 e);
    public uint32 colOf(uint32 e);
}
```

The Q4_K seam becomes (per K-slice `j`, scales staged in LDS as today):

```cajeta
uint32 e = 0;
while (e < mc.elems()) {
    uint32 ii = mc.rowOf(e);       // output row
    uint32 tt = mc.colOf(e);       // token
    facc.set(e, facc.get(e) + dLds[ii] * xsLds[tt]
        * (float32) (scLds[ii * 8 + j] * mc.get(e)));
    e = e + 1;
}
```

(`facc` and `mc` share `Use=2` 16x16 geometry, so their lane mappings
coincide — a guarantee the spec must state: **same (Rows, Cols, Use,
tier) ⇒ same element mapping**, which both backends already satisfy.)

### 3.1 Use cases

- **UC-A1** the Q4_K seam above; **UC-A2** the Q6_K seam (identical
  shape, one scale per K-slice).
- **UC-A3** the Q4_K dmin correction — a rank-1 `facc -= dm[r]*xs[c]*
  (Σ_j mn[r][j]·ps[c][j])` pass, expressible with the same loop.
- **UC-A4** bias adds, per-channel dequant epilogues, alpha/beta GEMM
  scaling — any elementwise epilogue.
- **UC-A5** causal/padding masks applied to score tiles (a future
  flash-attention prefill wants exactly this).
- **UC-A6** debugging: dump a fragment with true (r, c) labels.

### 3.2 Lowering, per backend

| backend/tier | `get`/`set` | `rowOf`/`colOf` |
|---|---|---|
| amdgpu native | extractelement/insertelement on the `<8 x _>` fragment — free | computable: `2e + (lane>>4)` / `lane & 15`, emitted inline |
| software tile (all backends) | flat-tile index — free | trivial (`e / Cols`, `e % Cols` per lane ownership) |
| vulkan SPIR-V native | possible (`OpAccessChain` element access exists) | **NOT EXPRESSIBLE** — SPIR-V KHR cooperative matrix defines the element↔position mapping as implementation-defined; no query op exists (NV's coopmat2 per-element callback is the NV-only answer) |
| nvptx native (future) | computable (documented PTX fragment layouts) | computable |

The Vulkan row is the design's one real wall: a kernel using
`rowOf`/`colOf` cannot run on the SPIR-V NATIVE tier at all. Under R3
the tier scan would demote such kernels to the portable tile on vulkan —
correct, loud, and slow. Everything cajeta-llama needs runs amdgpu-first,
but the language feature outlives the engine.

### 3.3 Risks

- The mapping-coincidence guarantee (int and float `Use=2` fragments
  align) becomes load-bearing API; it must be tested per backend.
- `elems()` varies by tier — user loops must use it, and a kernel that
  hard-codes 8 is wrong on the software tier. Lint-able, but a trap.
- Exposes the most general primitive; every future backend must either
  compute the mapping or demote. NV's SPIR-V history suggests
  per-element-with-indices IS where the ecosystem converges, but Khronos
  has not followed yet.

## 4. Option B — fused epilogue verbs (the narrow contract)

Keep the mapping fully private. Add the two operations the measured
kernels actually need, defined on WHOLE TILES so every backend can lower
them however it likes:

```cajeta
/** facc[r][c] += rowF[r] * colF[c] * this[r][c] — the k-quant GEMM
 *  seam. `rowF`/`colF` are Rows-/Cols-long workgroup-shared vectors. */
public void scaledAccumInto(CooperativeMatrix<float32, Rows, Cols, 2> facc,
        Shared<float32> rowF, Shared<float32> colF);

/** facc[r][c] += rowF[r] * colF[c] — the rank-1 correction (Q4_K's
 *  dmin term, folded per sub-block the way llama.cpp folds it). */
public void rank1Accum(Shared<float32> rowF, Shared<float32> colF);
    // defined on CooperativeMatrix<float32, Rows, Cols, 2>
```

The Q4_K seam becomes: 16 lanes fold `rowF[ii] = dLds[ii] *
(float32) scLds[ii*8 + j]` (llama.cpp's stage-time fold, one multiply per
row per K-slice), then one call:

```cajeta
mc.scaledAccumInto(facc, rowF, xsLds);
```

and the dmin pass becomes 8 `rank1Accum` calls per block (or one, with
the 8-term dot pre-folded per row) instead of the 256-element LDS pass.

### 4.1 Use cases

- **UC-B1/B2** the Q4_K and Q6_K seams — the two kernels this spec is
  filed for, and the reason the verb shape is `rowF ⊗ colF ∘ C`.
- **UC-B3** the dmin correction via `rank1Accum`.
- **UC-B4** general dequant epilogues: any int8/int4 GEMM whose scales
  factor per-row × per-column — which is every k-quant, every AWQ/GPTQ
  shape, and the per-expert scaling a future MoE GEMM needs.
- **NOT covered**: masks, non-multiplicative epilogues, element dumps —
  anything whose `f` does not factor. Those wait for Option A.

### 4.2 Lowering, per backend

| backend/tier | lowering |
|---|---|
| amdgpu native | per-lane: 8 unrolled fmas reading `rowF[2e+(lane>>4)]` and `colF[lane&15]` from LDS — llama.cpp's exact codegen, mapping never leaves the compiler |
| software tile | flat elementwise loop |
| vulkan SPIR-V native | **portable fallback**: spill to a scratch shared tile, apply, accumulate — semantically identical to today's hand-written seam, so the kernel still LOWERS on the native tier (mma stays native; only the epilogue round-trips). Fast SPIR-V can come later via NV coopmat2 where available |
| nvptx native (future) | per-lane fmas, like amdgpu |

No backend refuses; the semantic is whole-tile and mapping-free. R3's
loud-demotion machinery is not even needed — only a performance note
(`[mma-epilogue]`, mirroring `[mma-tiering]`) saying which lowering ran.

### 4.3 Risks

- Verb proliferation: the next epilogue shape that does not factor as
  `rowF ⊗ colF` means another verb (or finally Option A). The two verbs
  here are chosen because they cover every k-quant/AWQ/GPTQ/MoE dequant
  seam known today.
- `Shared<float32>` vectors as the scale carriers bakes in one staging
  idiom (it is the idiom both kernels already use for `dLds`/`xsLds`).

## 5. Comparison and recommendation

|  | Option A (element access) | Option B (fused verbs) |
|---|---|---|
| covers the measured need (R1) | yes | yes |
| portable to every backend | **no** — `rowOf`/`colOf` inexpressible on SPIR-V native; whole kernel demotes | yes — every backend has a correct lowering, amdgpu/nvptx a fast one |
| mapping exposure | via queries (R6 satisfied, but the coincidence guarantee becomes API) | none |
| future epilogues (masks, flash-attention score ops) | yes | no — needs new verbs or A |
| implementation surface | element/index intrinsics + per-backend mapping emission + tier-demotion wiring + the lint for hard-coded `elems` | two ops with one fast lowering (amdgpu) + one generic fallback |
| risk of silent wrongness | mapping-coincidence and `elems()` traps | essentially none — whole-tile semantics |

**Recommendation: build Option B first.** It closes the measured gap
with the smallest correct surface, lowers everywhere, keeps the fragment
mapping compiler-private, and its two verbs cover the known dequant
epilogue family (including MoE's). Option A stays on the table as the
general successor — the spec's use cases UC-A4/A5 (masks,
flash-attention score tiles) are real and will eventually force it, and
nothing in B blocks adding A later; B's amdgpu lowering IS A's mapping,
just unexposed. Deciding A now would spend the Vulkan-portability
argument and the coincidence-guarantee test burden before any use case
needs them.

## 6. Acceptance criteria (spec-level; the plan will itemize)

1. Compile-level pins per backend, both polarities (the
   AmdgpuCoopI8Tests discipline): the epilogue op LOWERS natively on
   amdgpu with no skip note; on a backend using the fallback, the
   `[mma-epilogue]` note names it; no configuration skips silently.
2. Runtime agreement: an epilogue-kernel vs spill-kernel comparison on
   real silicon, EXACT equality (R5) — plus the liveness guards (an
   all-zero pair proves nothing; the vacuous-green lesson).
3. The consumer measurement: cajeta-llama's Q4_K and Q6_K prefill GEMMs
   rewritten on the new op, A/B'd against the spill kernels with the
   established protocol (3+ idle-gated alternated rounds, identical next
   token, per-route timers). The projected mechanism removed: 8–16
   spills and 16–17 barriers per block per tile. No speedup number is
   promised here — 10.12.15/16 both taught that the win must be
   measured, not inferred from the mechanism.
4. Tier-scan behavior under R3 verified with a test that FIRES and a
   test that does not.
