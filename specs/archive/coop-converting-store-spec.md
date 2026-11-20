# coop-converting-store — a float32 accumulator fragment stored at 16 bits

> Status: **active**, written with and approved by Julian 2026-11-18 (cajeta-ml-gpu 23.2.20; decisions
> below). Plan: [coop-converting-store](../../agents/archive/coop-converting-store-plan.md).

## 1. Definition

`CooperativeMatrix<float32,16,16,2>.storeAs<U>(dst, offset, layout, stride)` writes an
accumulator fragment to a buffer of a narrower float type U (float16 or bfloat16),
converting each element as the scalar cast `(U) x` does, which rounds to nearest even.
It takes the same arguments as `store`, and the destination is a `KernelBuffer<U>` or a
`Shared<U>`.

The language has no way to do this today. A kernel that wants a 16-bit result walks the
fragment element by element (`get`, `row`, `col`) and stores each one. On the 4090
(sm_89, n4096, 2026-11-18) that is the whole of the measured gap to cuBLAS:

| per call, same process, fast clock | µs |
|---|---|
| `gemm128` with a float32 output (one `wmma.store.d` a fragment) | 773–774 |
| cuBLAS f16 (torch 2.14, reduced-precision reductions off) | about 780 |
| `gemm128Out16`, the same tile with an element-wise 16-bit store | 804–821 |

`gemm128Out16`'s PTX converts and stores each element on its own: 72 `cvt.rn.f16.f32` and
`st.global.b16`, 2 bytes a lane. Staging the fragment through shared memory and storing
16-byte vectors was tried. It cost a register (129, so one block a multiprocessor) and 16
barriers, and ran 1246–1301 µs, so the fix belongs in the lowering, not the kernel.

Decisions (Julian, 2026-11-18):

1. The spelling is an explicit `storeAs<U>`. It adds no fragment type and no implicit
   narrowing. `store` keeps its meaning: same element type, no conversion.
2. The first cut is native on nvptx and vulkan. amdgpu (WMMA and MFMA), cpu and the
   portable tile lower it correctly as per-element converts. Faster forms there come
   later, where measured.
3. (2026-11-18, after Unit 2) Vulkan lowers through the per-subgroup staging tile that
   spirv-kernel-parity 3 gave native SPIR-V tiles, per element, at both types. The
   fork has no cooperative-matrix convert intrinsic, so `OpFConvert` waits until
   proton's RADV rows show the staging costs.
4. (2026-11-18) Plain `store` and `load` on nvptx get the same run-time alignment
   branch as `storeAs`: wmma where each lane's run is aligned, per element otherwise.
   Off that alignment a tile faulted the context or put cells in the wrong place, with
   no diagnostic.

## 2. Features

### 2.1 The surface and its meaning

`storeAs<U>` is defined on a float32 accumulator fragment (Use 2) for U in {float16,
bfloat16}. Each stored element equals `(U) acc.get(q)` at its `(row(q), col(q))`, for
both layouts, at any stride, into global or shared memory. The reference interpreter
states this, and every backend is checked against it.

Use cases:
1. `acc.storeAs<float16>(c, off, 0, n)` and `acc.storeAs<bfloat16>(c, off, 1, n)` store
   what the element-wise loop stores, bit for bit, on every backend Phoenix and proton
   run.
2. A U outside {float16, bfloat16}, a non-accumulator fragment, or a non-float32
   accumulator is refused by name before codegen. `storeAs<float32>` is the same as
   `store` and is accepted.
3. Values that round, overflow to infinity, or are NaN and denormal convert exactly as
   the scalar cast does.

### 2.2 Native on nvptx

float16: the eight float32 accumulator registers convert in place to the four packed
f16x2 registers of a float16 accumulator fragment (`cvt.rn.f16x2.f32`), and
`wmma.store.d.sync...f16` stores the fragment. There is no shared staging and no barrier,
and the register count does not grow. PTX leaves fragment element order unspecified, so
the claim that a float32 element and the float16 element at the same index hold the same
cell is proven by a device value test, not assumed.

bfloat16 has no `wmma.store.d` form. It goes through the accumulator's element map
(the one the nvptx epilogues use), packing two column-adjacent elements into one 4-byte
store (`cvt.rn.bf16x2.f32`).

wmma moves each lane's contiguous run as one access, so it needs the run aligned and the
stride a multiple of it (§2.5 has the measured table), and a tile's offset is usually
known only at run time. So the form is chosen per store by a branch on the pointer and
stride, which every lane of the warp takes alike. A float16 tile on a 4-byte boundary
with an even stride takes `wmma.store.d.f16`. A row-major bfloat16 tile there takes the
packed pairs. Anything else takes the per-element store, so every placement stays
correct.

Use cases:
1. On sm_89, `gemm128Out16`'s interior tiles are aligned, so they store with
   `wmma.store.d...f16` and run no `st.global.b16`. Its registers stay at 128 or fewer
   with no spill.
2. The device value test on the 4090 covers row- and column-major, an odd stride and a
   shared destination, at both types, and reaches all three forms.

### 2.3 On vulkan

The accumulator goes through the per-subgroup staging tile in workgroup memory, and
each lane converts and stores the elements the staged map gives it, at float16 and
bfloat16 alike (decision 3). A native `OpFConvert` to a float16 cooperative matrix
followed by `OpCooperativeMatrixStoreKHR` needs a fork intrinsic and comes later, where
measured.

Use cases:
1. The SPIR-V passes spirv-val, and the vulkan backend matches cpu bit for bit on
   proton's RADV.

### 2.5 Aligned native loads and stores on nvptx

A native `store` or `load` of a 16x16 fragment uses wmma when each lane's contiguous run
is aligned and the stride in bytes is a multiple of it, and goes per element through the
lane-to-element map otherwise. The branch is uniform across the warp (decision 4).
Measured on the 4090 (2026-11-18, one process a placement, NvptxOperandMapTests):

| fragment | run | off the run's alignment | stride off its multiple |
|---|---|---|---|
| f32 / s32 accumulator | 8 bytes | faults | wrong cells, no error |
| f16 accumulator, f16 and bf16 operands | 4 bytes | faults | wrong cells, no error |
| row-major 8-bit A operand | 4 bytes | faults | faults |
| row-major 8-bit B operand | 1 byte | correct | correct |

The operand fragments' maps are measured too, and are the same for both load layouts,
so an unaligned operand load gathers per lane with no shared memory.

Use cases:
1. A float32 `store` at offset 11, stride 17 or offset 1, stride 18 writes every cell
   where the reference interpreter does, on the 4090; at offset 0, stride 16 it is one
   `wmma.store.d`.
2. A `load` at an unaligned offset reads every cell the interpreter reads.
3. The aligned GEMMs keep their registers and their wmma loads and stores.

### 2.4 The 16-bit-output GEMM kernels use it

`gemm128Out16`, `gemm128Bf16Out16`, `gemm256Out16` and `gemm256Bf16Out16` store their
interior fragments with `storeAs`. The edge path is unchanged.

Use cases:
1. On the 4090 at n4096, `gemm128Out16` runs level with the float32-output `gemm128`, in
   interleaved runs in one process, and op-gemm-f16 is level with or ahead of torch.
2. The hip and vulkan op-gemm rows on proton hold or improve, and every kernels leg stays
   green with its census.
