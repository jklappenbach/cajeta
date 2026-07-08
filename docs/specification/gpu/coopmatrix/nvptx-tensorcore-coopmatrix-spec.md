# Native NVPTX tensor-core cooperative matrix — spec

## Context

`CooperativeMatrix<T,Rows,Cols,Use>` (cajeta-gpu Part C) is the subgroup-collective
matrix-core tile primitive. Today it runs on three tiers:

- **Native** on **Vulkan** (`OpTypeCooperativeMatrixKHR` via `llvm.spv.cooperative.matrix.*`)
  and on **AMD** (`amdgcn.wmma.*` over RDNA3 WMMA cores — hand-marshalled fragments).
- **Portable** flat-tile everywhere else, including **NVIDIA** (a `[Rows*Cols x T]`
  Function-storage tile + a triple-loop multiply-add; correct, not tensor-core
  accelerated). Verified on the RTX 4090 by
  `XpuCooperativeMatrixDeviceTests.portableMatmulOnNvptxDevice`.

NVIDIA is the only GPU backend with **no native** cooperative-matrix path. NVPTX
has hardware tensor cores (`wmma`/`mma.sync`, sm_70+) and the matching NVVM
intrinsics are present in the `cajeta-llvm` fork (LLVM 23). This spec defines
wiring NVPTX cooperative matrix to those tensor-core intrinsics.

## Goal / why

Light up the RTX 4090's tensor cores for `CooperativeMatrix` so an unchanged cajeta
GEMM kernel runs on hardware matrix cores on NVIDIA — closing the last
native-tier gap among the GPU backends and giving the headline NVIDIA compute
capability (tensor cores) a real, on-device-verified path.

This is genuinely new lowering (it is **not** emit-proven today — there is no NVPTX
cooperative-matrix emit at all), so correctness must be established on-device, not
just by emit inspection.

## Capabilities (the "what")

1. **C1 — Native tier selection.** `NvptxTarget.coopMatrixTier(elem,rows,cols,use)`
   returns `Native` for the tensor-core configs NVPTX supports; `Portable`
   otherwise (so unsupported dtypes/shapes still run correctly on the flat tile).

2. **C2 — Fragment type.** `coopMatrixType` returns the exact LLVM literal-struct
   fragment type the NVVM `wmma.load` intrinsic produces / `wmma.mma`+`wmma.store`
   consume, keyed on element type and tile role (`use`).

3. **C3 — Load / store.** `coopMatrixLoad` / `coopMatrixStore` lower to
   `llvm.nvvm.wmma.m16n16k16.load.{a,b,c}.*` / `store.d.*` — the warp-collective
   load/store that handles the opaque fragment-to-lane layout internally (NVIDIA's
   layout is implementation-defined, so it MUST go through these intrinsics; the
   AMD-style hand-marshalling is not applicable).

4. **C4 — Multiply-add.** `coopMatrixMulAdd` lowers to
   `llvm.nvvm.wmma.m16n16k16.mma.*` — `D = A·B + C` on the tensor cores.

5. **C5 — Splat.** `coopMatrixSplat` builds the zero/initial accumulator fragment
   (a literal struct with every element set to the splat value).

6. **C6 — Warp-collective launch.** `prepareNativeCoopMatrix` records any kernel
   ABI requirement; the device test launches a full warp (block = 32) so the whole
   warp participates in the collective ops (sm_89 has tensor cores; no special
   target-feature is needed beyond the sm_80+ the target machine already targets).

7. **C7 — Tier coexistence.** Making NVPTX native for the supported configs must
   not break the existing portable-tier coverage: the portable NVPTX test is
   pinned to the portable tier (via `CAJETA_GPU_COOPMATRIX_IMPL=software`), and a
   new native test covers the tensor-core path.

## Supported configuration (v1 scope)

- **Shape:** 16×16×16 (`m16n16k16`) — the shape the cajeta coop tests and AMD path use.
- **Dtypes:** `float16` A/B → `float32` accumulator (the primary GEMM regime, and
  the only float config AMD/Vulkan expose on-device). `bfloat16` A/B → `float32` is
  a near-free add (same intrinsic family) and is included if it validates cleanly.
- **Layout:** **row-major** A and B (`layout == 0`). The NVVM `mma` intrinsic
  encodes A/B layout in its name and it must match the load layout; v1 targets the
  row-major GEMM (what the cajeta test exercises). Column-major operands and other
  shapes (e.g. m8n32k16, int8/u8) are explicit follow-ups, not v1.

## Non-goals (v1)

- Column-major operands, non-16×16×16 shapes, int8/u8 (`s8`/`u8`) tensor-core GEMM.
- LDS/shared-staged native loads (the `CoopStage` path) on NVPTX.
- Multi-tile K-accumulation tuning (the accumulator-persists-across-iterations
  pattern already works through the generic dispatch; it is validated only if it
  falls out for free).

## Use cases

- A cajeta `@Kernel` that declares `CooperativeMatrix<float16,16,16,0/1>` operands
  and a `<float32,16,16,2>` accumulator, issues one `mc.mma(ma, mb)`, and stores
  the result — the existing `kMatmulSource` — compiles through NVPTX and runs on
  the 4090's tensor cores, producing a bit-exact result for exact-integer inputs.
- The same source on the portable tier (forced via env) still runs and matches.

## Acceptance criteria

- **A1.** `coopMatrixTier` returns `Native` for 16×16×16 f16(/bf16)→f32, `Portable`
  otherwise; unit-checkable without a device.
- **A2.** The lowered PTX for `kMatmulSource` on NVPTX contains the `wmma.load.a`,
  `wmma.load.b`, `wmma.mma`, and `wmma.store.d` instructions (emit test).
- **A3.** On the RTX 4090, the native-tier GEMM is **bit-exact** against the integer
  reference for exact-integer f16 inputs (device test, block = 32), matching the
  Vulkan/AMD native results.
- **A4.** The existing portable NVPTX test still passes (pinned to the portable tier).
- **A5.** No regression: the NVPTX wave/ray-query/texture device tests and the
  AMD/Vulkan/CPU coop-matrix tests are unaffected.
- **A6.** Diagnosis of any device fault uses a C++ `CudaDriver` probe (JIT-runtime
  stderr is dead on this platform), not JIT stderr.

## References

- Parent plan: `plans/gpu/cajeta-gpu-plan.md` (Part C cooperative matrix).
- AMD reference impl: `src/cajeta/xpu/amd/AmdgpuKernelLowering.cpp` (the WMMA seams).
- Interface: `src/cajeta/xpu/lowering/LoweringTarget.h` (coopMatrix* seam),
  `src/cajeta/xpu/lowering/KernelLowering.cpp` (slot + op dispatch).
- NVVM intrinsics: `nvvm_wmma_m16n16k16_{load_{a,b,c},mma,store_d}_*` (fork LLVM 23).
