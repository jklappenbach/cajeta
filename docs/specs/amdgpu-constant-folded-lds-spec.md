# amdgpu-constant-folded-lds — spec

## 1. Definition

### 1.1 Purpose & scope
Make the Cajeta AMDGPU backend generate LDS (workgroup / `addrspace(3)`) reads and
writes the way AMD Tensile does: with the per-lane base address computed **once**
(loop-invariant, hoisted) and the per-access offset emitted as a **compile-time
immediate** folded into the `ds_read`/`ds_write offset:` operand — instead of the
current per-access runtime address arithmetic. The scope is the LDS-addressing path in
`AmdgpuKernelLowering`/`KernelLowering` (the WMMA cooperative-matrix fragment read/write
and the direct/staging tile access), with the `BlockPadded<T,Block,Pad>` pad as the
first beneficiary. amdgpu-only.

### 1.2 Problem statement
The shipped `BlockPadded<T,Block,Pad>` primitive is correct but, measured on gfx1151,
costs **1.79× VALU** (943M vs 526M `SQ_INSTS_VALU`, rocprof) versus the literal-stride
kernel of the *identical physical layout* (bank conflicts identical: 356.329M vs
356.330M). The cause: `fragCoord`/`maybeSwizzle` emit the pad as runtime
`UDiv`/`Mul`/`Add` **per fragment per K-iteration**, because the address arrives as one
runtime value mixing the loop-invariant panel base, the per-lane term, and the
within-block K offset — so LLVM cannot hoist or fold it. Net: block-pad regresses to
14.7 vs the 26.0 TFLOP/s literal-stride baseline despite lower-or-equal conflicts.

### 1.3 Ground truth — what Tensile does
From the hipBLASLt Tensile source
(`ml/TheRock/.../tensilelite/Tensile`): the local-write and local-read offsets,
*including the `LdsBlockSizePerPad` pad*, are folded at kernel-generation time into the
`ds_*` instruction's immediate `offset:` (`KernelWriterAssembly.py:9709` —
`DSModifiers(offset=<const>)`; `LocalRead.py:312` — `applyPad` on a constant offset).
The per-thread base address is computed once at kernel entry from the lane id; each
unrolled K-iteration's `ds_read`/`ds_write` carries a constant immediate offset. **Zero
runtime VALU for LDS addressing.** This is the foundational lever under torch's f16
38.2 TFLOP/s; the other torch techniques (non-transposed `tlu` staging, PGR2, VGPR-tight
scheduling) build on top of constant-folded addressing.

### 1.4 The foldability identity (why this is sound)
For a WMMA fragment whose K-panel equals one pad block (`depthU == Block`, the natural
case), and a block-aligned panel base, additive block padding satisfies
`pad(panelBase + lane·stride + k) == pad(panelBase + lane·stride) + k` for every
`k ∈ [0, Block)` — because no pad-block boundary falls inside the panel. So the padded
per-lane base is loop-invariant (computable once) and the within-block K offset rides as
an unpadded constant. The compiler must emit the address in exactly this decomposed
shape so LLVM's LICM hoists the base and instruction selection folds the constant into
`ds_*offset:`.

### 1.5 Constraints
- **GPU-free-probe-first.** Each change is gated by an ISA probe BEFORE any device run:
  count `v_add*`/`v_lshlrev*`/`v_mul*` in the K-loop body, count `ds_read`/`ds_write`
  with an `offset:` immediate, confirm `ds_read_b128` width preserved, `.vgpr_spill_count
  == 0`, no `Cannot select`.
- **On-device confirmation.** rocprof `SQ_INSTS_VALU` must drop toward the literal-stride
  baseline; throughput measured at n2048 vs 26.0 (literal-stride) and 38.2 (torch).
- **No correctness or generality regressions.** The `Swizzled<T,S>` XOR path, all
  existing xpu probe/device tests, and non-block kernels stay byte-identical/green.
- **Honest measure-ship-or-record.** This primitive ALONE may only *match* 26.2
  (`Block=64` is the stride-66 layout, identical conflicts) — that is the expected floor;
  its value is removing the overhead so the conflict sweep and the access-pattern work
  (`tlu`/PGR2) become viable. A net throughput win is best-effort and recorded, not
  assumed.

### 1.6 Non-goals
- The `tlu` non-transposed B staging, PGR2 prefetch, and VGPR scheduling (separate
  efforts that consume this one).
- A general LICM/strength-reduction pass over arbitrary IR — scope is the LDS-addressing
  emission shape, not a new optimizer pass.
- NVPTX/SPIR-V/CPU backends (their LDS/shared addressing is out of scope).
- Changing the `BlockPadded` semantics or the bank-conflict outcome (same layout, same
  conflicts — only the *cost* of addressing changes).

## 2. Foldable LDS address emission

The backend must emit an LDS access address as `hoistableBase + constImmediate` (two
GEPs / an addressing pair), where `hoistableBase` is loop-invariant within the K-loop and
`constImmediate` is a compile-time constant, so LLVM lowers the access to
`ds_*… offset:<const>` with the base in a register computed once.

### Use cases
- **2.1** As the AMDGPU backend, when I lower an LDS access whose address is
  `invariantBase + constantOffset`, then I emit the constant offset as a distinct
  constant GEP (not folded into a runtime `add`), so instruction selection produces a
  `ds_* offset:` immediate (ISA probe: the access carries `offset:` and no extra
  `v_add` appears in the K-loop for that access).
- **2.2** As the backend, when the per-lane base of a fragment read is invariant across
  the K-loop, then it is computed once before the loop (LICM-hoisted), not recomputed per
  iteration (ISA probe: the lane-base arithmetic — `v_lshlrev`/`v_mul`/`v_add` — appears
  in the prologue, not the loop body).
- **2.3** As a developer, when a kernel has no constant-decomposable LDS offset, then the
  emission is unchanged from today (no regression to existing kernels).

## 3. BlockPadded constant-fold (the first consumer)

`BlockPadded<T,Block,Pad>` fragment reads/writes must fold per §1.4: pad the per-lane
panel base once (loop-invariant), and carry the within-block K offset as an unpadded
compile-time constant — eliminating the per-fragment `UDiv`/`Mul`/`Add`.

### Use cases
- **3.1** As the GEMM kernel, when I read a `BlockPadded` WMMA fragment with `Block ==
  depthU` and a block-aligned panel base, then the emitted ISA contains **no** pad
  `UDiv`/`Mul`/`Add` in the K-loop body — only the hoisted base and a `ds_read … offset:`
  immediate (ISA probe).
- **3.2** As a developer, when the within-block K offset is a loop variable, then the
  compiler still folds it once the K-loop is unrolled to constants (the natural WMMA
  case); if it cannot prove the offset constant, it falls back to the correct runtime pad
  (no miscompile).
- **3.3** As the correctness oracle, when the folded-pad kernel runs on device, then its
  result equals the runtime-pad kernel and the literal-stride kernel (bit-exact for the
  identity-A check) — the fold changes cost, never values.
- **3.4** As the `Swizzled<T,S>` user, when block-pad folding lands, then the XOR-swizzle
  path and its tests are untouched (the fold is specific to additive block padding).

## 4. Measurement & acceptance tiers

### Use cases
- **4.1** As the developer, when I land §2/§3, then a GPU-free ISA probe gates it: K-loop
  body has zero pad `UDiv`/`Mul`, `ds_read_b128` width preserved, fragment access carries
  `offset:`, no spill, no `Cannot select`.
- **4.2** As the developer, when I run on device, then rocprof `SQ_INSTS_VALU` for the
  block-pad kernel drops to within ~10% of the literal-stride baseline (526M), confirming
  the overhead is gone.
- **4.3** As the developer, when the overhead is gone, then n2048 throughput for the
  `Block=64` block-pad kernel reaches **≥ 26.0** (floor — no regression vs literal
  stride).
- **4.4** As the developer, when I sweep `(Block,Pad)` now that the pad is free, then I
  record the per-config bank-conflict % and throughput; if any config measures a net win
  over 26.2 it is shipped as the bench default (good tier), otherwise the floor result +
  the negative sweep are recorded honestly.
