# XPU Device Vectorized Staging — Spec

## 1. Definition

### 1.1 Purpose
Let a Cajeta `@Kernel` stage operands **global → register → LDS** in **128-bit
vector chunks** instead of one scalar element at a time, so a single thread can move
many elements per instruction. This is the lever that makes a **deep-K** (depthU 64)
register-blocked f16 WMMA GEMM expressible in `@Kernel` source — the confirmed hard
prerequisite for matching PyTorch/hipBLASLt f16 throughput on gfx1151.

### 1.2 Problem
The torch-faithful replica (12 accumulators / 96×128 tile / wide-B / bounds-checked) is
**validated correct on-device** but slow at depthU 16: it is *latency-starved* (only 12
WMMA between barriers) and a 12-accumulator wave-tile forces the workgroup down to 4
waves (low occupancy). Hiding global-load latency at low occupancy requires a large batch
of independent WMMA per barrier — **depthU 64 (48 WMMA/barrier)**. But depthU 64 means
each thread stages **~112 elements per K-panel**, which as *scalar* code either:
- explodes VGPR (register-prefetch of 112 scalars/thread → spill / occupancy collapse —
  measured: the 28-register depthU-16 prefetch was already 5.8× slower), or
- serializes on per-element `ds_write_u16` LDS feed (half-rate; the same class of problem
  wide-B fixed for the *read* side).

The missing capability is a **wide store into `Shared<T>` (LDS)** to match the wide load
already proven from global buffers. The global side (`a.vload<8>(i)`) emits
`global_load_dwordx4` today; the LDS side does not yet emit `ds_write_b128`.

### 1.3 Key insight (reframes scope — verify before building)
Static `Shared<T>` tiles are **already registered as buffer bases** in the kernel
lowering (`bufferBases`/`bufferElems`, addrspace 3), specifically so `tile[i]` reuses the
ordinary buffer GEP/load/store path. The vectorized-loadstore gate
(`bufferBases.count(recv) && bufferElems.count(recv)`) therefore *already matches a Shared
local*, and `bufferElementPtr` is **addrspace-preserving** (1=global, 3=LDS). So
`sa.vstore(i, v)` / `sa.vload<N>(i)` on a static Shared tile should already lower to
packed addrspace(3) vector ops **with no new surface**.

The one suspected gap is **alignment**: `LoweringTarget::vectorLoad/vectorStore` set
`getABITypeAlign(elemTy)` — for `float16` that is **2 bytes**. A `<8 x half>` LDS access
at 2-byte alignment likely *scalarizes* to 8× `ds_write_u16` / `ds_read_u16` instead of
forming one `ds_*_b128`. So this is most likely a narrow **alignment-widening codegen
change** (when the access is provably N-aligned, set the access alignment to the vector
type's alignment) — not a new primitive. **Unit 1 is a GPU-free ISA probe that decides
kernel-only vs. codegen-change**, exactly as the wide-B effort did.

### 1.4 Scope
- Vectorized store **into `Shared<T>`** (LDS, addrspace 3): `tile.vstore(i, v)` emitting
  `ds_write_b128` (and the symmetric `tile.vload<N>(i)` emitting `ds_read_b128`) for
  `float16`/`bfloat16`/`int8`/`float32` element types and N giving a 128-bit access.
- Whatever minimal codegen change Unit 1 proves necessary (most likely: widen the access
  alignment on `vectorLoad`/`vectorStore` when the element index is a known multiple of N,
  so the packed op selects to a single wide LDS/global instruction).
- Apply it to the depthU-64 staging loop of the f16 GEMM replica and measure on-device.

### 1.5 Non-goals
- **No** async/DMA global→LDS path — `global_load_lds` Cannot-selects on gfx1151 (RDNA3.5
  lacks `VMemToLDSLoad`); that lever lives in `xpu-pipelined-gemm-primitives` and is
  hardware-unavailable here. This effort is the synchronous wide-staging path that *does*
  work on gfx1151.
- **No** swizzle changes (shipped separately); vectorized staging composes with it but
  does not modify it.
- **No** new f16 GEMM kernel structure — the 12-acc/96×128/wide-B replica is banked and
  correct; this effort only unblocks deep-K staging for it.

### 1.6 Constraints
- Additive over the existing `vload`/`vstore` surface and the one unified allocation/borrow
  model — no special-case bypass ([[feedback_one_unified_allocation_borrow_model]]).
- Portable seam: AMDGPU first (parity target); NVPTX/CPU inherit the default packed path;
  Vulkan/SPIR-V keeps its existing ≤4-component split fallback — never miscompiles.
- A vectorized store/load must produce **bit-identical results** to the scalar element
  loop it replaces (proven by a CPU oracle / on-device cross-check).

## 2. Vectorized LDS store/load surface

### 2.1 Requirements
`vstore`/`vload` must accept a static `Shared<T>` tile receiver and lower to packed
addrspace(3) memory ops. The element index is in **elements** (not bytes), matching the
scalar `tile[i]` convention. A 128-bit access width is the target (N=8 for f16/bf16, N=16
for int8, N=4 for f32).

### 2.2 Use cases
- **2.2.1** As a kernel author, when I write `tile.vstore(i, v)` on a `Shared<float16>`
  with a `Vector<float16,8>` value and `i` a multiple of 8, then the store lowers to a
  single `ds_write_b128`.
- **2.2.2** As a kernel author, when I write `Vector<float16,8> v = tile.vload<8>(i)` on a
  `Shared<float16>` with `i` a multiple of 8, then the load lowers to a single
  `ds_read_b128`.
- **2.2.5** Unit 1's ISA probe verifies all three WMMA-relevant element widths form a
  single 128-bit LDS op: `float16`/`bfloat16` at N=8, `int8` at N=16, `float32` at N=4.
  The codegen change (if any) stays type-generic; the probe asserts each width explicitly.
- **2.2.3** As a kernel author, when the element index is **not** provably N-aligned, then
  the access stays correct (it may legally fall back to narrower ops) and never
  miscompiles.
- **2.2.4** As a kernel author on NVPTX/CPU, the same source compiles through the inherited
  default packed path; on Vulkan it splits to ≤4-component ops as today.

## 3. Wide global→LDS staging in the f16 GEMM

### 3.1 Requirements
The replica's K-panel staging loop must move each thread's slice of A and B with wide
loads from the global buffers and wide stores into the LDS tiles, enabling depthU 64
without scalar VGPR explosion. The staged result in LDS must be byte-identical to the
existing scalar staging.

### 3.2 Use cases
- **3.2.1** As the GEMM kernel, when staging a K-panel, then each thread issues
  `b.vload<8>(globalIdx)` → `sb.vstore(ldsIdx, v)` (wide load + wide store), moving 8
  elements per instruction pair instead of 8 scalar ops.
- **3.2.2** As the GEMM kernel at depthU 64 with vectorized staging, when run on-device at
  n=2048, then `check_ok == true` (matches the exact-cross-check oracle).
- **3.2.3** As the GEMM kernel at depthU 64, when measured on-device, then throughput is
  reported honestly against variant B (20.6 TFLOP/s) and against torch f16 (~39 TFLOP/s),
  shipping only if it does not regress the committed bench.

## 4. Verification & honesty

### 4.1 Requirements
Every claim is measured: the ISA probe is GPU-free and asserts on `ds_*_b128` vs
`ds_*_u16` counts and on `vgpr_spill_count == 0`; correctness is an on-device exact
cross-check; throughput is reported as min-ns → TFLOP/s with the variant tag, and the
committed bench is only changed if the new path wins.

### 4.2 Use cases
- **4.2.1** As a reviewer, when I read the probe output, then I see the b128/u16 counts
  for both the global and LDS sides, so I can confirm the wide path formed.
- **4.2.2** As the developer, when a vectorized variant does **not** beat variant B, then
  the bench stays variant B and the residual is recorded (measure-ship-or-record).
