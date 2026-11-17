# datacenter-gpus — CDNA3 (MI300) and Hopper (H100) matrix cores

> Status: **active**, approved by Julian 2026-11-17; written with him that day (cajeta-ml-gpu 23.2.9;
> decisions below). Plan: [datacenter-gpus](../agents/datacenter-gpus-plan.md).

## 1. Definition

The kernel package adapts to the device it runs on, and its matrix-core tiles
are written once for every backend. Two datacenter parts fall outside that
today. Measured by compile-only probes on Phoenix (2026-11-17, cajeta main,
cajeta-ml 2d1946b):

- **gfx942 (MI300, CDNA3).** 112 of the package's 126 kernels build, all at
  wave 64. The 14 that do not are every matrix-core kernel: gemm16, gemm128,
  gemm256 and their bf16, i8, batched and split-K forms, DecodedGemm16,
  FusedGemm, flash attention and convBackWeight3x3. They declare
  `@Wave(width = 32)` because the amdgpu cooperative-matrix lowering is RDNA's
  WMMA, whose fragment map is 32 lanes, and CDNA has no WMMA. Its matrix cores
  are MFMA, on 64-lane waves. So on MI300 there is no f16, bf16 or i8 GEMM on
  matrix cores, and no flash attention. The backend also links
  `oclc_wavefrontsize64_off` for every arch, which is wrong on a wave-64-only
  part.
- **sm_90 (H100, Hopper).** Everything builds, and the cooperative tiles lower
  to Ampere-era `wmma.mma.sync m16n16k16`. There is no `wgmma` and no TMA bulk
  copy, which are the only path to cuBLAS's rate on Hopper. Results are
  correct, well short of cuBLAS.

Decisions (Julian, 2026-11-17):

1. MI300 first. Its gap is availability, not only speed.
2. CDNA's matrix cores are reached through the compiler: `CooperativeMatrix`
   lowers to MFMA on CDNA, chosen by arch, with a wave-64 fragment map. The
   kernels become wave-generic, so one source serves RDNA (WMMA, wave 32) and
   CDNA (MFMA, wave 64). There are no CDNA-only kernels.
3. Hopper goes to `wgmma` with TMA. That needs new language surface (a
   warpgroup fragment and a tensor-map descriptor), designed with Julian
   before it is built.
4. Hardware is rented once each part is compile-ready and value-checked on
   what Phoenix and proton can run. One session per part, about an hour each
   ($10-30 in all, Julian's call when).

## 2. Features

### 2.1 Wave-64 targets are first-class on amdgpu

A CDNA arch links the device libraries for its wave width, reports its own
DeviceProfile (wave 64, LDS, MFMA shapes), and a kernel that does not pin a
wave builds at the arch's native width.

Use cases:
1. A gfx942 build links `oclc_wavefrontsize64_on`, and a gfx1151 build keeps
   `_off`.
2. `DeviceProfile` answers gfx942's wave width, LDS bytes and matrix-core
   shapes, and the lowering reads them rather than RDNA's.

### 2.2 CooperativeMatrix on MFMA

On CDNA, `CooperativeMatrix<T,16,16>` load, store, `mma` and the epilogues
lower to MFMA (`v_mfma_f32_16x16x16_f16`, `_bf16`, `v_mfma_i32_16x16x32_i8`)
over a 64-lane fragment map. The same kernel source that runs WMMA on RDNA
runs MFMA on CDNA.

Use cases:
1. gemm16 at f16 and bf16 builds for gfx942 with MFMA instructions in its ISA
   and matches the host product.
2. The 16-bit wide-load fast path, which assumes WMMA's map, is not taken on
   CDNA; an MFMA fragment carries its own.
3. The reference interpreter's fragment semantics stay the definition, so the
   cpu backend checks the MFMA kernels' arithmetic without the hardware.
4. Before any rental, gem5's AMD GPU model runs the gfx942 gemm16 code object
   and its values match the host product, which proves the MFMA fragment map
   without the hardware (Julian, 2026-11-17).

### 2.3 Wave-generic matrix-core kernels

The package's matrix-core kernels stop pinning wave 32. Their tile shapes and
lane arithmetic read the wave width, so each builds on gfx1151 at 32 and on
gfx942 at 64, register- and LDS-budgeted per arch.

Decided with Julian 2026-11-17: a kernel with no `@Wave` builds at the target's
native width (32 on nvptx and RDNA, 64 on CDNA, the host SIMD width on cpu), and
`@Wave` is kept only for a kernel whose processing needs one width on every
target. The ten kernels that pin 32 today (flash attention, convBackWeight3x3,
and gemm128 and gemm256 at f16 and bf16, each with its 16-bit-output variant)
take the pin off, read `Wave.width()` on the device, and size their launch from
a host query of the width the loaded kernel was built at,
`kernel.waveWidth()`, which reads the kernel manifest. The cpu backend must run
them correctly at its own width.

Use cases:
1. On gfx942 all 126 kernels build, none spilling, and the census lists the
   matrix-core ones as native.
2. `gemm128.waveWidth()` on the host answers the width its device code was
   built at: 32 on sm_89 and gfx1151, 64 on gfx942, the vectorized width on cpu,
   and a pinned kernel's pin.
3. The unpinned kernels pass the kernels suite on cpu at the host's width.
4. On gfx1151, sm_89 and vulkan nothing regresses: same kernels, same rates.

### 2.4 Hopper warpgroup matrices and TMA

A warpgroup fragment (128 threads) and an asynchronous `wgmma` step, with TMA
bulk copies of tiles from global to shared memory through a tensor-map
descriptor. The surface is designed with Julian; the tile family gains a
Hopper member.

Use cases:
1. A Hopper GEMM member at f16 and bf16 builds for sm_90 with `wgmma` and
   `cp.async.bulk.tensor` in its PTX.
2. On sm_89 and earlier the member is not built, and the existing tiles run.
3. `ptxas -v` for sm_90a assembles every Hopper member with no spills, so the
   rented hour checks values and speed, not whether the code assembles
   (Julian, 2026-11-17).

### 2.5 Measured on the parts

Use cases:
1. MI300X: the census green, and the GEMM rows at n4096 against rocBLAS and
   torch ROCm.
2. H100: the same against cuBLAS and torch.
