# amd-wmma-mi-native-lds — spec

## 1. Definition

### 1.1 Purpose & scope
Rewrite the AMD cooperative-matrix LDS addressing — `coopMatrixLoad`, `coopMatrixStore`,
`fragCoord` in `AmdgpuKernelLowering`, plus the matching staging global→LDS write path — so
each lane's LDS read/write address matches the **RDNA3 WMMA hardware fragment register layout**
(which lane holds which `(k,n)`/`(m,k)` element), making **wide `ds_read_b128` fragment reads
bank-conflict-free**. This replicates what AMD Tensile's `LocalRead`/`LocalWrite` do, which is
the **measured** root cause of the f16 WMMA GEMM's 26.0 → 38.2 TFLOP/s gap on gfx1151. Scope: the
AMD WMMA cooperative-matrix lowering (fragment load/store/coord) and the cooperative staging
primitive, for the `16x16x16` f16 config first, structured to extend to bf16/int8. amdgpu-only.

### 1.2 Problem statement
`coopMatrixLoad`/`fragCoord` decompose the lane into the WMMA tile/K coordinate
(`lane16 = lane & 15` as row/col, `lane >> 4` as the K half) — the basic MI lane decomposition,
which torch also does — then form a **linear** address `tileIdx*stride + k` and rely on LLVM to
vectorize the element loads. This reads the *correct* elements (the GEMM is bit-exact,
`check_ok=true`) but is **bank-pathological when widened**, because it omits the **strided index
permutation** torch applies to `tileIdx` before the stride multiply (Tensile `perpPerm`, §1.3).
Measured on-device (rocprofv3, gfx1151, n2048):

| layout | reads | LDS bank conflicts | TFLOP/s |
|---|---|---|---|
| stride 66 (pad 2) — shipped | `ds_load_2addr_b32` (narrow) | **38.2%** | **26.0** |
| stride 72 (pad 8) | `ds_read_b128` (wide) | 75.5% | 16.6 |
| stride 80 (pad 16) = **torch's exact stride** | `ds_read_b128` (wide) | **79.2%** | 13.3 |

Widening the read *doubles* the conflicts under our linear lane→address mapping, so we are forced
to read **narrow** (b32-paired) to stay at 38% — leaving ~30% of peak unused. Torch uses the
*identical* stride 80 (confirmed by disassembling its shipped kernel: `ds_read_b128` offsets are
multiples of 160 bytes = 80 f16/row) yet reads **wide and conflict-free**, because its LDS
addresses match the hardware lane→element layout. The differentiator is the fragment lane mapping,
not stride/pad/buffer-count (all of which we matched to torch and measured *worse*).

### 1.3 Ground truth
- **Tensile `LraTileAssignment.py` (`LraTileAssignmentMFMA` / `LraTileAssignmentCode`, ~line 148)** —
  the authoritative per-lane local-read address. Verified by reading the source: it (a) decomposes
  the lane via `tReg = (tid % wavewidth) % MatrixInstN` (the MI tile coord — *we already do this*),
  then (b) applies **`perpPerm(tReg)`** (line 242), a **strided index permutation**
  `I → (I // permBlock)*permBlock + perm(I % permBlock)` with params `gNLCPermBlock` /
  `gNLCPerpStride`, gated on `perpStride > 1 && TLU == 0`, then (c) `tReg *= strideTile`
  (`= depthU + LdsPad`) plus block/wave offsets. **`perpPerm` is the de-conflicting transform we
  omit** — it is a *block-granular* index permutation applied to the coordinate **before** the
  stride multiply, so consecutive lanes land in distinct banks **while the per-lane K run stays
  contiguous** (wide read preserved). This is categorically different from our element-level XOR
  `Swizzled<T,S>`, which permutes *within* the contiguous run and therefore scalarizes wide reads.
- **Solution params** (shipped f16 96×128×64): `LdsPad=16`, `LdsBlockSizePerPad=128`B,
  `UnrollMajorLDS=true`, `LRVW=16`, → stride 80, confirmed from the disassembled kernel
  (`ds_read_b128` offsets are multiples of 160 B = 80 f16/row).
- **RDNA3 wave32 WMMA fragment layout** — the `amdgcn.wmma.f32.16x16x16.f16` operand semantics fix
  which lane holds which element; `perpPerm`'s `permBlock`/`perpStride` are derived from that
  geometry. The exact values for the f16 16×16×16 config are pinned GPU-free in the plan's unit 1.
- **Confirmed measurement** (this effort): torch stride 80, `ds_read_b128`, conflict-free; our
  linear (un-permuted) marshal at the same stride = 79.2% conflicts.

### 1.4 Constraints — lock-and-converge (NOT measure-ship-or-revert)
This effort **faithfully reconstructs torch's kernel and converges to its result**; it does **not**
treat each torch setting as a knob to A/B against 26.0 and revert. Torch's 38.2 emerges from all the
pieces *interacting* (stride 80 regressed in isolation precisely because `perpPerm` and PGR2 were
absent), so per-piece revert can never converge. Therefore:
- **Lock the confirmed-torch parameters (§1.6) as invariants.** Once a parameter is established from
  torch's source/binary, it is held fixed; we do **not** alter it to chase a local win, and we do
  **not** revert it because an intermediate (incomplete) build is slower than 26.0.
- **No revert.** If a build does not yet reach torch's result, we do **not** fall back to the
  stride-66 26.0 kernel. Instead we (a) tune only the **unresolved** parameters (§1.7), and (b) dig
  deeper into torch / hipBLASLt / rocBLAS source + the disassembled kernels to pin the next unknown.
  26.0 is the *reference to beat*, not a fallback to retreat to.
- **Correctness is non-negotiable throughout.** `check_ok` bit-exact (A=identity ⇒ C==B) at every
  step; an incorrect build is a bug to fix, never an accepted intermediate.
- **GPU-free-probe-first.** Every codegen change is gated by an ISA probe (wide `ds_read_b128`, no
  spill, no `Cannot select`) and, where derivable, a modeled bank-conflict count, before a device run.
- **Don't break unrelated kernels.** The new addressing is a mode on the cooperative-matrix lowering;
  other kernels and `Swizzled`/`BlockPadded`/existing coop tests stay byte-identical when it is off.
  (This is about not regressing *other* code — it is **not** a license to revert `gemmF16`.)
- **Honest convergence reporting.** Each step records the current state: TFLOP/s vs the 38.2 target,
  bank-conflict %, which parameters are locked, and which unknowns remain open.

### 1.5 Non-goals
- NVPTX / SPIR-V / CPU cooperative-matrix lowering (their fragment marshaling is out of scope).
- Datatypes beyond f16 for the first convergence (bf16/int8 are a structured extension afterward).
- Changing the cooperative-matrix *language surface* (`CooperativeMatrix<T,R,C,Use>.load/mma/store`)
  — this is a lowering-internal reconstruction; kernels keep the same API.
- (Note: **PGR2** and **ScheduleIterAlg** are **in scope** for this effort — they are required to
  converge to 38.2 and are later units, not separate deferred efforts. The no-revert rule spans them.)

### 1.6 Locked parameters (from the DISPATCHED torch kernel — rocprof-confirmed)
Read off the **actual dispatched solution name** captured by rocprofv3 on a live torch f16 matmul
(`Cijk_Ailk_Bljk_HHS_BH_Bias_HA_S_SAV_UserArgs_MT96x128x64_MI16x16x1_..._DTLA0_DTLB0_..._GRVWA8_GRVWB8_
..._LBSPPA3072_LBSPPB128_..._LPA8_LPB8_..._LRVW16_..._MIWT3_4_..._NLCA3_NLCB1_..._PGR2_PLR0_..._SIA1_
..._VWA1_VWB1_..._WG32_4_1`). These supersede the Equality-YAML values (which were a *different*,
non-dispatched solution):
- **Tile / instruction:** MacroTile 96×128, depthU 64, `MI [16,16,16,1]`, `MIWaveTile [3,4]`, WG32_4_1.
- **LDS layout:** `UnrollMajorLDS=true`, **`LdsPadB=8`** (`LPB8`), `LdsBlockSizePerPadB=128` B
  ⇒ **stride 72** (NOT 80). `LdsPadA=8`, `LdsBlockSizePerPadA=3072` B (A pad is sparse).
- **`DirectToLds=0` ⇒ `perpPerm` is OFF** (`KernelWriter.py:4378` needs DTL). The de-confliction is
  the BASE Tensile MI local-read **+** local-write address pairing, not a permutation.
- **Vector widths:** `GRVW=8`, `LocalReadVectorWidth=16`, `VectorWidth=1`. `NLCB1`/`NLCA3`.
- **Pipeline / schedule:** `PrefetchGlobalRead=2` (PGR2), `PrefetchLocalRead=0`, **`ScheduleIterAlg=1`**
  (not 3). VGPR=256, LDS≈62.5 KB.
- **THE TARGET METRIC:** torch's `SQC_LDS_BANK_CONFLICT = 0` (0.0%) at stride 72 — our addressing at the
  *same* stride 72 measures 75.5%. Reaching **0% conflicts** is the concrete, measured goal of §2.

### 1.7 Open unknowns (the tuning + research surface)
These are not yet pinned; they are what we tune and what we keep mining torch/BLAS source + kernels
to resolve (no-revert applies while they are open):
- The exact `perpPerm` activation (`perpStride>1 && TLU==0`) and `(gNLCPermBlock, gNLCPerpStride)`
  values for the f16 A and B fragments (U1).
- The precise PGR2 register-prefetch pipeline structure (prologue/steady/epilogue, registers live).
- The `ScheduleIterAlg=3` instruction interleave (ds_read / v_wmma / global_load / s_waitcnt order).
- Any further divergence the disassembly reveals (waitcnt placement, the global-load→LDS coalesce
  layout `gNLC`, scheduling of the transposed B write). Each resolved unknown moves to §1.6.

### 1.8 On-device findings (2026-06-28) — the bottleneck is PIPELINE DEPTH, not conflicts or occupancy
Resolved by measurement, in order; each overturned the prior hypothesis:
- **Bank conflicts are NOT the bottleneck.** Wide-write transposed-B staging reached **0.0%** LDS
  conflict (torch parity, from 38%), VALU halved — yet throughput was unchanged (~26). `perpPerm` was
  disproven earlier (DirectToLds-gated, off in torch); the block pad de-conflicts but doesn't pay off.
- **Occupancy is NOT the lever, and "2 WG/WGP" was a phantom.** The target came from the wrong kernel's
  LDS (MT128x48 = 30464 B). torch's actual **MT96×128 LDS = 50688 B = 1 WG/WGP, double-buffered, 256
  VGPR** — torch runs at **one** workgroup/WGP, same as us. Single-buffering to force 2 WG gave no gain.
- **Overlap (register prefetch) IS a real gain.** B next-panel prefetch + single buffer = **27.8** (from
  26.0). Full A+B prefetch spills at depthU64 (Scratch 128 → 16.9); B-only is the sweet spot.
- **The remaining lever is PLR (prefetch local read).** torch issues **112 `ds_load_b128`/kernel** —
  it prefetches LDS fragments far ahead into registers (256-VGPR budget) so the WMMA never stalls on
  ds_read. We read-then-mma → ~**49% SQ-busy** (not compute-bound; bubbles). PLR + double-buffer +
  SIA3 at 256 VGPR is torch's path to 38.2. **This supersedes the occupancy framing in §1.6/U4.**

## 2. Block-additive pad on the MI-decomposed read base (mechanism CRACKED)

> MECHANISM (from Tensile's own generated assembly — `TensileCreateLibrary` on the HHS logic YAML,
> commented `.s`). torch's per-lane `LocalReadAddr` for A and B is:
> ```
> base = (lane%16)·depthU + (waveBit)·512 + waveGroupOffset      ; MI tile decomposition
> LRA  = base·2 (bytes);  LRA += (LRA / 128bytes)·32bytes        ; BLOCK-ADDITIVE pad (16 f16 / 128 B)
> ```
> The de-conflictor is the **block-additive pad applied to the lane-dependent base** (plus the
> `(waveBit)·512` interleave) — i.e. `pad(base) = base + (base/Block)·Pad`, which is **exactly the
> shipped `BlockPadded<T,Block,Pad>` primitive (U1)** with `Block = 64 f16 (128 B)`, `Pad = 16 f16`.
> Our `gemmF16` uses a **linear literal stride** (`m·72`), which lacks the block-additive structure +
> wave-bit interleave → 75% conflicts; torch's block-additive form → 0%. (`perpPerm` is OFF; not it.)

`fragCoord` must emit the MI-decomposed base and then the **block-additive pad** (the `BlockPadded`
map), not a linear `(lane%16)·stride`, so the wide `ds_read_b128` reads measure **0% LDS bank
conflicts** (rocprof) — matching torch. This reuses the U1 primitive; the gap was applying it to the
fragment read base rather than open-coding a padded stride.

### Use cases
- **2.1** As the AMD backend, when I lower a `CooperativeMatrix.load` of an A or B f16 `16x16x16`
  fragment with the permutation enabled, then `fragCoord` emits the `perpPerm`-remapped tile
  coordinate and the read still lowers to `ds_read_b128` (ISA probe: `ds_read_b128 > 0`,
  `ds_read_u16 == 0`, no `Cannot select`, no spill — the permutation must not scalarize the read).
- **2.2** As the correctness oracle, when a permuted fragment is loaded from a tile written by the
  matching staging path (§3), then the values equal the un-permuted reference bit-for-bit (on-device
  `check_ok`, plus a single-thread CPU/JIT round-trip of the address map without the WMMA op).
- **2.3** As the developer, when I model the lane→bank pattern for the permuted read, then the
  modeled conflict count is far below the un-permuted linear marshal's at the same wide read width
  (GPU-free model; confirmed by rocprof in §5).
- **2.4** As a maintainer, when the permutation is OFF (default), then `fragCoord` is byte-identical
  to today (no regression to any existing kernel or the shipped 26.0 GEMM).
- **2.5** As the implementer, when `(permBlock, perpStride)` reduce to identity for a config, then
  `fragCoord` emits no extra ALU (the permutation degrades cleanly to today's behavior).

## 3. Matching staging (global→LDS write) addressing

The cooperative staging primitive must write the global panel into LDS in the **same** MI-native
layout the fragment read expects, so a staged value is read back correctly (round-trip identity),
and the staging store stays wide (`ds_store_b128` where the global vector width allows).

### Use cases
- **3.1** As the GEMM kernel, when I stage a B (and A) panel into an MI-native tile, then the store
  address uses the same layout map as the read, the round-trip is bit-exact (§2.2), and the store
  stays wide (ISA probe).
- **3.2** As the developer, when the staging write pattern is laid out for the MI read, then its own
  bank-conflict contribution does not reintroduce the conflicts the read fix removed (rocprof:
  total kernel LDS conflicts down, not merely shifted from read to write).

## 4. The MI-native LDS tile layout

The `(k,n)`→LDS-address map (incl. the `UnrollMajorLDS` ordering and the pad/swizzle) that makes the
fixed hardware lane pattern conflict-free for wide reads — expressed as a reusable property of the
cooperative tile, parameterized by the WMMA config, not hardcoded into one kernel.

### Use cases
- **4.1** As a kernel author, when I declare a cooperative tile in MI-native mode for a given WMMA
  config, then both the staging-write and the fragment-read addressing derive the layout
  automatically (no per-call stride/swizzle argument).
- **4.2** As the compiler, when the WMMA config changes (tile/datatype), then the layout is recomputed
  from the fragment geometry rather than re-hardcoded (extensibility check: the f16 path generalizes).

## 5. Integration & measurement

### Use cases
- **5.1** As the developer, when I apply MI-native addressing to `gemmF16` and probe GPU-free, then
  the fragment reads are `ds_read_b128`, no spill, no `Cannot select` (gate before any device run).
- **5.2** As the developer, when I run on-device, then rocprof reports the LDS bank-conflict % for the
  MI-native wide-read kernel **below** the 38% narrow baseline (attributing the read-side reduction),
  and `check_ok=true` at n256–n2048.
- **5.3** As the developer, when I measure n2048 throughput against the **38.2 target** (26.0 is the
  reference to beat, not a fallback), then I **keep the locked-torch config in place and do not
  revert**: if it is short of 38.2 I tune the open unknowns (§1.7) and mine torch/BLAS source deeper
  for the next missing piece, iterating until convergence. Progress is reported each step (TFLOP/s,
  bank-conflict %, locked vs open params). The effort is done when n2048 approaches 38.2 with
  `check_ok` bit-exact; a genuine hardware/codegen wall (not an unresolved unknown) is the only
  accepted stopping point short of that, and it is recorded with the evidence that it is a wall.
