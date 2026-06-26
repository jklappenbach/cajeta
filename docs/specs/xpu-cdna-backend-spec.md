# XPU CDNA (Data-Center Card) Backend — Spec

## 1. Definition

### 1.1 Purpose
Make AMD **CDNA data-center GPUs** — gfx908 (MI100), gfx90a (MI200), gfx942 (MI300),
gfx950 (MI350) — first-class Cajeta `@Kernel` build targets: arch selection, codegen,
registration, and multi-arch bundling all accept and correctly produce code objects for
them, and `AsyncCopy` uses the **native direct global→LDS load** (`global_load_lds`) that
these arches uniquely support and the consumer RDNA APU (gfx1151) lacks
([[reference_gfx1151_no_vmem_to_lds]]).

### 1.2 Problem
The AMD backend was brought up and validated on a single consumer APU (gfx1151, RDNA3.5).
The arch is hard-defaulted to `"gfx1151"` across `createAmdgpuTargetMachine` /
`assembleHsaco` / `registerAmdgpuKernel`, and the only on-device validation is gfx1151.
CDNA cards differ from RDNA in ways that affect correctness, not just performance:
- **Wavefront size is always 64** on CDNA (RDNA is wave32 by default) — wave ops, workgroup
  tiling, and any wave-width assumption must reflect the device, not a baked constant.
- CDNA **has `VMemToLDSLoad`** (`global_load_lds`) — `AsyncCopy`'s native LDS-direct path,
  which `Cannot-select`s on RDNA, is exactly where it pays off (no VGPR staging buffer).
- CDNA matrix cores are **MFMA** (`v_mfma_*`), not RDNA **WMMA** (`v_wmma_*`).

Without CDNA enablement, a developer who builds a Cajeta `@Kernel` for an MI300 either gets
a gfx1151 code object that won't load, or a sync-staged copy that throws away the LDS-direct
hardware. The infrastructure for arbitrary/multi arch already exists (`--xpu-arch`,
`splitArchList`, `assembleHsacoBundle`); what is missing is verified CDNA codegen + the
native AsyncCopy path proven across the CDNA family.

### 1.3 Scope
The **backend foundation** for CDNA — everything a non-matrix `@Kernel` needs to build
correctly for the CDNA family and to use `AsyncCopy`'s native path:
1. CDNA arch enablement: codegen + registration + multi-arch bundling accept gfx908/90a/942/950.
2. `AsyncCopy` native LDS-direct (`global_load_lds`) verified across all four CDNA arches,
   with a `vmcnt`-based wait that enables N-stage overlap (vs RDNA's full-drain fallback).
3. Portability: the SAME `@Kernel` source compiles across the RDNA↔CDNA spectrum, each arch
   taking its correct tier (RDNA sync-staging vs CDNA LDS-direct), with a `note:` disclosure.

Because **no CDNA card is present on this development box**, all acceptance is **GPU-free
emit/ISA/IR verification**; on-device execution is gated to CDNA CI/hardware and deferred.

### 1.4 Constraints
- Pure additive over the existing AMD backend — gfx1151 behavior is byte-identical
  (its codegen/tests must not regress). The arch is a parameter, never a fork in the source.
- The native path stays **arch-gated** (`archHasVmemToLds`) — emitting `global_load_lds`
  where the subtarget lacks it `Cannot-select`s, so the gate is load-bearing, not advisory.
- Verification is GPU-free (the AMDGPU TargetMachine emits ISA for any arch without a
  device); each test names the arch it covers and whether it is emit-only or on-device.

### 1.5 Non-goals
- **MFMA matrix cores / CDNA `CooperativeMatrix`** (`v_mfma_*`). The AMD coop-matrix lowering
  targets RDNA WMMA; CDNA GEMM needs an MFMA path. That is a **named follow-up** the DC GEMM
  benches depend on — out of scope for this foundation. A non-matrix kernel using
  `CooperativeMatrix` on CDNA must fail loudly or take the software tier, never miscompile.
- **Async-mark group pipeline** (`asyncmark`/`wait_asyncmark`) — needs `GFX1250Plus`
  (RDNA4), not CDNA; a separate future arch follow-up.
- **DC-tuned f16/bf16/int8 GEMM benches** — the parity push the LDS-direct path enables;
  depends on the MFMA follow-up and on DC hardware to measure. Out of scope here.
- **Runtime device-arch auto-detection / auto-tiering** (detect the running card → pick the
  arch). The multi-arch fatbin already lets one build carry several arches; *selecting* among
  them at dispatch is the deferred "runtime tiering" depth, not this foundation.

---

## 2. CDNA arch enablement

Codegen, registration, and bundling must accept and correctly produce code objects for the
CDNA family, accounting for the wave64 / target-feature differences from RDNA.

### 2.1 Use cases
- 2.1.1 As a developer, when I pass `--xpu-arch=gfx942`, the AMD backend emits a valid gfx942
  code object for a non-matrix `@Kernel` (shared-memory reduction, saxpy, scalar) — verified
  by non-empty, arch-correct ISA.
- 2.1.2 As a developer, when I pass a mixed arch list `--xpu-arch=gfx942,gfx950,gfx1151`,
  `assembleHsacoBundle` produces a single fatbin carrying a per-arch code object for each,
  with no arch causing a codegen/link failure that drops the others.
- 2.1.3 As the backend, when I codegen for a CDNA arch, wave-width-dependent lowering
  (`Wave.width()`, wave reduce/shuffle, workgroup tiling) reflects **wave64**, matching the
  device — never a baked wave32 constant. (Cajeta already queries wave width at runtime; this
  is the requirement that CDNA codegen honors it.)
- 2.1.4 As the backend, when I codegen each CDNA arch, I emit the correct ROCm device-library
  control globals (`oclc_isa_version_<isa>`) and target features for that arch, so the code
  object is loadable on the matching device (verified structurally / by successful assembly).
- 2.1.5 As a developer building for CDNA-only, the AMD default-arch behavior is documented and
  overridable; the gfx1151 default is not silently imposed on a CDNA build request.

---

## 3. AsyncCopy native LDS-direct path on CDNA

`AsyncCopy.copy` lowers to `global_load_lds` (the LDS-direct, no-VGPR-staging load) on every
CDNA arch, with `commit`/`wait` giving a correct — and on CDNA, overlap-capable — group model.

### 3.1 Use cases
- 3.1.1 As a kernel author, when I `AsyncCopy.copy` a panel on any CDNA arch (gfx908/90a/942/950),
  the staging emits `global_load_lds_{ubyte,ushort,dword}` (1/2/4-byte) — the data goes
  global→LDS with no VGPR staging buffer — not a `global_load` + `ds_store` round-trip.
- 3.1.2 As a kernel author, `AsyncCopy.wait(0)` on CDNA drains the outstanding LDS-direct loads
  (a correct `s_waitcnt vmcnt(0)` / fence) so the following `Barrier.workgroup()` publishes the
  landed tile — bit-identical staging to the synchronous path, verified against a reference.
- 3.1.3 As a kernel author, when I run an N-stage prefetch pipeline on CDNA (issue stages ahead,
  `AsyncCopy.wait(N-1)` each step), the lowering keeps up to N-1 groups outstanding via a
  partial `vmcnt` wait — overlap the RDNA fallback can't express (it full-drains). The ISA
  shows a partial `vmcnt` wait, not only `vmcnt(0)`.
- 3.1.4 As the backend, a CDNA arch the LDS-direct load can't serve for a given element width
  (≠ 1/2/4 bytes — e.g. fp64) falls back to the synchronous strided copy, same as RDNA — correct,
  just unaccelerated.
- 3.1.5 As a kernel author, the native CDNA path and the RDNA sync fallback are **the same
  source** — choosing the path is the compiler's job per arch, never mine.

---

## 4. Portability, tiering & verification

### 4.1 Use cases
- 4.1.1 As a developer, one `@Kernel` using `AsyncCopy` compiles cleanly for the full arch
  spectrum (gfx908/90a/942/950 CDNA + gfx1100/1151 RDNA) — CDNA native, RDNA sync — with no
  arch producing a `Cannot-select` or other hard error.
- 4.1.2 As a developer, the build discloses the tier each arch took (a `note:` naming
  LDS-direct vs synchronous-staging), so the chosen path is visible, never silent.
- 4.1.3 As a maintainer, every CDNA claim is backed by a GPU-free emit/ISA/IR test that names
  its arch; nothing asserts on-device behavior that wasn't run, and the on-device gap (no DC
  card here) is explicit — the device tests exist but skip-with-reason until CDNA hardware/CI.
- 4.1.4 As a maintainer, the existing gfx1151 AMD test suite stays green (this is additive).

## 5. Acceptance / fidelity
- 5.1 All four CDNA arches emit valid, arch-correct ISA for the representative non-matrix
  kernels; a mixed CDNA+RDNA fatbin assembles.
- 5.2 `AsyncCopy.copy` emits `global_load_lds` on every CDNA arch; `wait` emits the correct
  drain, and the N-stage form emits a partial `vmcnt` wait (overlap) on CDNA.
- 5.3 One `AsyncCopy` source compiles across the full RDNA↔CDNA spectrum with the correct
  per-arch tier and a `note:` disclosure; gfx1151 behavior + tests unchanged.
- 5.4 On-device CDNA execution tests are authored and gate (skip-with-reason) on a CDNA device,
  ready to run under DC CI/hardware — they are NOT a blocker for this foundation's completion.
