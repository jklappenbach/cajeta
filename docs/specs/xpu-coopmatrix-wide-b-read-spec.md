# Wide Matrix-B Cooperative-Matrix Fragment Reads — Spec

## 1. Definition

### 1.1 Purpose
Make the **matrix-B WMMA fragment load issue WIDE `ds_read_b128`** instead of 64×
narrow `ds_read_u16`, halving the LDS-feed cost of the B operand in every f16/bf16/int8
cooperative-matrix GEMM on AMD. This is a **VGPR-neutral** lever (no kernel register
cost) and is one of the two compiler/layout levers that unblock the f16
register-blocked GEMM parity push (`[[project_gpu_f16_wmma_ceiling]]`), where the
matrix-B feed is currently half-rate.

### 1.2 Problem & root cause (read from `AmdgpuKernelLowering.cpp`)
`coopMatrixLoad` (line 866-870) marshals a WMMA fragment as 16 scalar per-element loads,
each addressed by `fragCoord` (line 937-966). The fragment element index is:
- **matrix-A** (`use=0`): `lane*stride + e` — consecutive `e` are **contiguous**, so LLVM
  vectorizes the 16 loads into `ds_read_b128` (4×128-bit). ✓ wide.
- **matrix-B** (`use=1`, **row-major**): `e*stride + lane` — consecutive `e` are **`stride`
  apart** → strided → the 16 loads stay 16× `ds_read_u16`. ✗ narrow (half-rate).

Measured by the gpu-f16 U1 probe: A `b128=6`, B `u16=64`.

**Key insight:** `fragCoord` already computes a **column-major** index (line 957):
`colMajor = col*stride + row = lane*stride + e` — which IS contiguous for B. So if the B
LDS tile is stored **transposed** (N-major, each N-column's K-values contiguous) and the
fragment is loaded **column-major**, the 16 B loads become contiguous → `ds_read_b128`,
**with no codegen change**. This reframes the effort from "a codegen fix" to "a kernel
LDS-layout technique" — to be confirmed by Unit 1 before committing to either path.

### 1.3 Scope
- The AMD WMMA cooperative-matrix B-fragment LDS read (f16 first; bf16/int8 share the
  fragment mapping so they inherit it).
- A **kernel-side technique**: store B transposed + padded in `Shared`, load column-major
  (the `layout` arg already on `CooperativeMatrix.load`). Possibly a small reusable
  padded/transposed-B staging helper.
- **Codegen fallback** only if Unit 1 shows the col-major path does NOT vectorize to
  `b128` (then widen the matrix-B read in `coopMatrixLoad` directly).
- Apply to `gemmF16` and measure the on-device delta.

### 1.4 Constraints
- 1.4.1 **Correctness-preserving.** The WMMA result is unchanged — col-major load of a
  transposed-stored tile reads the same logical B[k][n] values. `check_ok=true` at every
  swept size on-device.
- 1.4.2 **VGPR-neutral.** No added kernel register pressure (the whole point — the scalar
  register-prefetch already hit a VGPR ceiling, `[[project_gpu_f16_wmma_ceiling]]`).
- 1.4.3 **rocprof / ISA-verified.** The widening is shown in emitted ISA (`u16`→`b128`)
  and any shipped speedup is on-device-measured.
- 1.4.4 **No bank-conflict regression.** The transposed B *store* is where prior
  "transpose B" attempts regressed; **padding** the transposed tile (extra elems/row to
  break 32-bank periodicity) is required so the store stays conflict-free.

### 1.5 Non-goals
- 1.5.1 The matrix-A read (already wide) or the accumulator store.
- 1.5.2 NVPTX / SPIR-V (their cooperative-matrix loads are whole-tile HW ops, not
  per-element — not applicable). CPU is the software tile.
- 1.5.3 The other VGPR-neutral lever (`xpu-device-vectorized-staging`, queued separately).
- 1.5.4 A full gemmF16 parity rewrite — that's the parked `gpu-f16-register-blocked-gemm`
  plan, which this unblocks.

---

## 2. Wide matrix-B fragment read

- 2.1 *As the AMDGPU backend, when a kernel loads a matrix-B (`use=1`) f16 fragment from a
  B tile stored column-major (transposed) in LDS and loads it column-major, then* the 16
  per-lane element loads are contiguous and emit `ds_read_b128` (not 64× `ds_read_u16`) —
  asserted GPU-free in the emitted gfx1151 ISA.
- 2.2 *As the kernel author, when I stage B into a transposed + padded `Shared` tile and
  load with `CooperativeMatrix.load(tile, off, /*layout=*/1, strideK)`, then* the WMMA
  consumes the correct B operand (B[k][n] unchanged) — the col-major addressing reads the
  transposed store consistently.
- 2.3 *(Fallback) As the AMDGPU backend, when Unit 1 shows the col-major path does NOT
  vectorize, then* `coopMatrixLoad` is changed to emit a vectorized (`ds_read_b128`)
  matrix-B fragment read directly — the same observable outcome via codegen.

## 3. Conflict-free transposed B store

- 3.1 *As the kernel, when B is stored transposed in LDS with a **padded** K-stride
  (`Kpad = K + pad`, pad breaking 32-bank periodicity), then* the global→LDS store of the
  transposed tile is bank-conflict-free (no regression vs the row-major store), and the
  col-major fragment read is both wide AND conflict-free.
- 3.2 *As the perf engineer, when I A/B the transposed-padded B layout, then* rocprof shows
  the B fragment reads widened (`u16`→`b128`) and LDS-read pressure down, with no new
  store conflicts.

## 4. Measurement & acceptance

- 4.1 *As the perf engineer, when I apply the wide-B layout to `gemmF16`, then* I measure
  min_ns→TFLOP/s at n256–n2048 on HIP, A/B vs the variant-B baseline, all `check_ok=true`,
  with ISA + rocprof evidence that B reads widened.
- 4.2 **Acceptance:** GPU-free ISA test proving matrix-B emits `ds_read_b128`; on-device
  `check_ok=true`; a measured A/B showing the B-feed widening (ship the gemmF16 layout if
  it beats variant B, else record the residual with the ISA win still banked as a
  general-purpose primitive). Memory updated; unblocks `gpu-f16-register-blocked-gemm`.
