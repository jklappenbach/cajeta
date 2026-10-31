# The cpu backend across x86 SIMD levels

Status: **approved** 2026-10-30 (Julian). Origin: llm-cpu-parity 4.3.1.7's ISA A/B and
Julian's ask that the cpu kernels adapt to Zen 3, Zen 5 and Intel.

## 1. Definition

The cpu backend compiles every `@Kernel` for the build host's exact CPU
(`createCpuTargetMachine`, `--cpu=native`) and the host module the same way.
There is one copy of each kernel and no runtime choice, so a binary built on
one machine runs only on machines with every feature of the builder's, and a
kernel's integer dot takes the host's one tier. Measured 2026-10-30 on proton
(Zen 5, a superset, so each build runs), the 8B Q4_K_M bench built for other
levels:

| built for | q4kQ8X8VecKernel | pp512 | tg128 | 4-seq decode |
|---|---|---|---|---|
| znver5 (AVX-512, VNNI) | 562 instructions, 64 `vpdpbusd` | 265-269 | 21.9 | 60.7-60.9 |
| alderlake (AVX2, AVX-VNNI) | 535, 64 `vpdpbusd` | 183-190 (0.69) | 21.1 (0.96) | 54-56 (0.90) |
| znver3 (AVX2) | 2908, 0 `vpdpbusd`, 448 `pmaddwd` | 21 (0.08) | 13.5-13.7 (0.62) | 12.8 (0.21) |

Two defects: a binary cannot move to a lesser host (it dies with an illegal
instruction), and on a host without VNNI a kernel's `dotAccum` takes the
portable per-lane form because `CpuKernelLowering.integerDotWide` accepts VNNI
only; the AVX2 tiers that `simd-fused-integer-madd` built (§4.11: 3 and 15
instructions against the portable 57) are never reached from a kernel.

This spec makes one binary run correctly on any x86-64 host from AVX2 up and
near that host's hardware, through per-kernel variants chosen at startup, a
real pre-VNNI tier inside kernels, a wider interleave where the vectors are
wider, and Intel's matrix unit where it exists. The mechanism is the
compiler's; the kernels that exploit a level are cajeta-llm's.

Non-goals: hosts below AVX2 (x86-64-v2 and older) are refused at startup
with the required level named, not served; per-ISA tuning of ordinary host
code, which is compiled once at the baseline; auto-vectorization changes.

## 2. Variants and dispatch

- **2.1** When a program is built with `--cpu=portable`, the host module is
  compiled at the x86-64-v3 baseline (AVX2, FMA, F16C, BMI2) and every
  `@Kernel` is emitted once per level of the variant set, each copy with its
  own `target-cpu`/`target-features`, each registered under the kernel's name
  with its level.
- **2.2** The variant set is, from lowest: `v3` (AVX2), `v3-vnni` (AVX2 +
  AVX-VNNI: Intel desktop from Alder Lake, Zen 5), `v4` (AVX-512 F/BW/VL/DQ +
  VNNI: Skylake-X, Ice Lake, Zen 4, Zen 5, Sapphire Rapids), `v4-amx` (v4 +
  AMX-TILE/INT8: Sapphire Rapids and later). `--cpu-variants=<list>` names a
  subset.
- **2.3** When the program starts, the runtime resolves each kernel name once to
  the highest registered level the host supports, from CPUID, XCR0 (the OS
  saves the ymm, zmm and tile state) and, for AMX on Linux,
  `arch_prctl(ARCH_REQ_XCOMP_PERM, XTILEDATA)`. `CAJETA_XPU_CPU_LEVEL=<level>`
  forces a lower level, for A/B runs and the tests in §7.
- **2.4** When a kernel is resolved, the cpu wave (`Device.waveSize`, the built
  wave the registration sets) is the chosen variant's: 8 at v3, 16 at v4.
- **2.5** When the host supports no registered level, startup fails with a
  message naming the host's level and the lowest built, before any kernel runs.
- **2.6** `--cpu=native` keeps today's one variant and size; it stays the
  default for a build that runs where it is built. `--cpu=<name>` keeps its
  meaning (one variant, that cpu).
- **2.7** The kernel manifest and the census record the levels a kernel was
  built at and the one it resolved to.
- **2.8** The cost is code size: the cpu kernel code is 23 MB of the 34 MB
  llm bench binary (614 functions), so four levels add roughly 70 MB.
  Measured, not estimated, at acceptance (7.4).

## 3. The pre-VNNI integer tier inside kernels

- **3.1** When a kernel's `dotAccum` lowers for a level with AVX2 and no VNNI,
  it takes `simd-fused-integer-madd`'s exact pre-VNNI form (widen, multiply,
  `vpmaddwd`; 15 instructions a 32-pair dot), never the portable per-lane form.
- **3.2** When the weight operand is provably within 0..128 (LLVM known bits:
  a masked nibble, a 6-bit assembly, a widened byte), it takes the saturating
  `vpmaddubsw` form (3 instructions), which is exact in that range (madd spec
  §4.8.1). A weight the lowering cannot bound takes 3.1.
- **3.3** The results of every variant are bit-identical for every int8 kernel:
  the cpu backend remains the GPU backends' oracle. Held by running the kernel
  selftests at each forced level (7.1).

## 4. Wider vectors

- **4.1** When the resolved level is v4 or above, a dense model's cpu int8
  Q4_K and Q6_K weights may be streamed in as a 16-row interleave (lane r of a
  512-bit vector is row 16g + r), with kernels on it, chosen at load from the
  resolved level.
- **4.2** The 16-row form is taken only where a load-time probe on this host
  measures it faster than the 8-row form (Zen 4 double-pumps 512-bit vectors
  and may not gain; Zen 5 and Intel server parts may).
- **4.3** Each row's result is bit-identical to the one-row kernel's, as the
  8-row form's is.

## 5. The matrix unit

- **5.1** When the resolved level is `v4-amx`, `CooperativeMatrix<int8,16,16>`
  on the cpu target takes the Native tier on AMX: `ldtilecfg`, `tileloadd`,
  `tdpbusd`, `tilestored` (the intrinsics are in this LLVM), so the existing
  int8 WMMA batch kernels run on the matrix unit without a new kernel.
- **5.2** An AMX tile multiplies 16 x 64 bytes by 64 x 16; the fragment's
  K of 16 is a quarter of it. The lowering either packs four consecutive
  `mma` steps into one tile operation or accepts the under-use; which is a
  measurement.
- **5.3** `Linear.matrixCoresPay` answers from a probe on the host, as it does
  for the GPU tiles, so the batched routes take AMX only where it is faster.
- **5.4** LIMIT: there is no AMX host here. The emitted IR's shape is verified
  through `llc -mattr=+amx-tile,+amx-int8`, as the madd spec verified
  AArch64; the tier is held off (`@Unlowered`-style, by level) until it is
  measured on real hardware, and nothing here is described as tested without
  saying which of the two it means.
- **5.5** AMX-BF16 (`tdpbf16ps`) for the bf16 tiles is a later step on the
  same tier.

## 6. Other architectures

- **6.1** The variant mechanism carries to AArch64 (levels: base NEON,
  `+dotprod`, `+i8mm`, `+sve`) once the toolchain LLVM registers the target;
  today it registers x86 and amdgcn only (madd spec §4.12). The dotAccum tiers
  for `sdot`/`usdot` exist. Not in this spec's acceptance.

## 7. Acceptance (measured on proton, quiet, llm's selftests and 8B bench)

- **7.1** One `--cpu=portable` llm build: the cpu leg green at each level
  forced through `CAJETA_XPU_CPU_LEVEL`; the int8 kernels' outputs bit-identical
  across levels.
- **7.2** The forced `v3` run at least 0.5 of the resolved run's pp512 and 0.85
  of its tg128 (a first bar, from the pre-VNNI tier's instruction counts; to be
  revised by measurement); the forced `v3-vnni` run keeps today's 0.69 / 0.96
  or better.
- **7.3** The resolved run within 2% of today's `--cpu=native` build on pp512,
  tg128 and the 4-seq decode: multiversioning costs one lookup a kernel at its
  first launch and nothing after.
- **7.4** The portable binary's size recorded beside the native one.
- **7.5** The 16-row interleave: measured against the 8-row form on proton
  (Zen 5) in the load-time probe, taken only where faster, bit-identical.
- **7.6** AMX: IR shape through `llc` only (5.4).

## 8. Decisions

Settled 2026-10-30 (Julian): scope is dispatch, the pre-VNNI tier, the
16-row interleave and AMX; spec now, built before llm-cpu-parity Unit 5.
Not chosen: dispatch and the tier alone; the tier alone with per-machine
builds; the spec alone for now.

Settled 2026-10-30 (Julian), with the recommendation in each case:

- **Q1** The baseline is x86-64-v3 (AVX2); older hosts are refused at
  startup. Not chosen: an SSE4.2 variant as llama.cpp ships.
- **Q2** `--emit=exe` stays `native`; a release passes `--cpu=portable`.
  Not chosen: portable by default.
- **Q3** Every kernel at every level; the size is measured first and the set
  narrowed only if it matters. Not chosen: variants only for kernels that use
  wide SIMD.
