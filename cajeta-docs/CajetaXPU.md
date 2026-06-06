# CajetaXPU — Accelerator Substrate

This document specifies Cajeta's accelerator code-generation and
runtime layer — the layer that `cajeta.math`, `cajeta.render`,
`cajeta.prism`, `cajeta.torch`, and anything else that wants to push
work off the CPU plugs into.

The library is `cajeta.xpu`. It is shaped around three peer GPU backends
today and is named to admit more tomorrow without renaming the prefix.

> **Update (2026-05-31): a fourth backend — CPU — and a runtime dispatcher are
> now implemented.** Three peer *GPU* backends (NVIDIA/AMD/Vulkan) remain the v1
> focus described below, but the same portable `@Kernel` source also compiles
> for the **CPU** (`--xpu-backend=cpu`, a grid→threads host lowering), and a
> **runtime backend dispatcher** lets one binary bundle several backends and pick
> the best available at launch — `CUDA → HIP → Vulkan → CPU` — falling to the CPU
> when no accelerator is present ("run anywhere, degrade to CPU"). The dispatcher
> lives in the C runtime (`runtime/native/cajeta_runtime.c`), the sole launch path
> for compiled programs. This realizes Goal 1.1.2's "change a target flag" promise
> and extends it to a *runtime* choice. Design, decisions, and the staged bring-up
> log: **[`cajeta-cpu.md`](../cajeta-cpu.md)**. Runnable demo:
> **`samples/Tour/xpu/`**.

---

## 0. Why "xpu"?

**XPU** is the industry's umbrella term — coined by Intel, picked up
broadly — for *any* programmable processing unit beyond the CPU: GPUs,
NPUs (neural processing units), TPUs (tensor processing units), DPUs
(data processing units), and FPGAs. The point of the abbreviation is
that host code shouldn't have to care which specific category of
silicon is on the other side of the dispatch boundary — only that work
was shipped off-core and a result will come back.

Cajeta adopts the term for the same reason. Today `cajeta.xpu.*` ships
three peer backends, and every one of them is a GPU:

- **cajeta.xpu.nvidia** — NVIDIA GPUs via LLVM NVPTX + CUDA driver
- **`cajeta.xpu.amd`** — AMD GPUs (consumer + datacenter) via LLVM
  AMDGPU + HIP runtime
- **`cajeta.xpu.vulkan`** — any Vulkan-capable device (NV, AMD, Intel
  Arc, Apple via MoltenVK, Android, SteamDeck) via SPIR-V codegen +
  Vulkan loader

The namespace, the capability traits, the launch syntax, the memory
model — all of it is GPU-shaped. That is the v1 focus and we are not
pretending otherwise.

What the `xpu` prefix buys us is room. When NPU accelerators (Apple
Neural Engine, Qualcomm Hexagon, Intel Meteor Lake NPU), TPU-class
silicon (Google Coral, Tesla Dojo), or programmable fabrics (Xilinx /
Intel FPGA, AMD XDNA) become Cajeta backends, they slot in alongside
`cajeta.xpu.nvidia` / `amd` / `vulkan` without renaming anything user
code references — the address-space types, the capability traits, the
`Buffer<T>` / `Stream` / `Event` interfaces, the `@kernel` attribute.
New backends bring new capability traits and possibly new sub-
namespaces; existing code that touches only `cajeta.xpu.core.*` keeps
working on the new silicon at the cost of a recompile.

So `xpu` reads as "this prefix is for accelerators in general; today
it covers GPUs; tomorrow it covers more." A GPU kernel written in 2026
spends most of its time inside `cajeta.xpu.nvidia`, `cajeta.xpu.amd`,
or `cajeta.xpu.core` — the umbrella prefix is just forward-compatible
naming.

---

## 1. Goals and non-goals

### 1.1 Goals

1. **One library, multiple backends.** `libcajeta-xpu.so` carries the
   NVIDIA, AMD, and Vulkan code paths. Their respective driver
   libraries (`libcuda.so.1`, `libamdhip64.so`, `libvulkan.so.1`) are
   `dlopen`'d on demand; any of them being absent at startup just
   removes the corresponding device family from `xpu.devices()`.
2. **A Cajeta program that says "run this kernel on the GPU" should be
   compilable for any of the three backends by changing a target
   flag,** as long as it uses only features in the shared subset (§3).
3. **ML / HPC / scientific users who need tensor cores, MFMA, warp-
   level reductions, async copies, and CUDA-graph-style scheduling get
   them without going through a portability layer that hides
   hardware.** The vendor namespaces expose the full surface.
4. **A single Cajeta binary may target one, two, or three backends;**
   runtime device discovery picks one without dragging the unselected
   drivers into the process unless asked.
5. **The borrow checker keeps its teeth across the host/device
   boundary:** no use-after-free of a buffer whose only remaining
   user is an in-flight kernel.
6. **Graphics pipelines (raster, ray tracing, mesh shaders) live one
   layer up in [`cajeta.render`](CajetaRender.md), built on the Vulkan
   substrate here.** XPU itself is compute-only.

### 1.2 Non-goals

- **No HLSL / GLSL / WGSL / Slang frontend.** Cajeta is the source
  language.
- **No Metal / DX12 / WebGPU backend in v1.** Not ruled out; the
  shared MIR is structured so a DXIL or WGSL backend could be added
  later without restructuring the frontend.
- **No automatic kernel autotuning.** Tile sizes, block shapes, and
  pipeline depths are written by the kernel author or by a higher-
  level library (`cajeta.math`, Prism, Torch). XPU exposes
  introspection; it does not search.
- **No CUDA-source compatibility.** Cajeta is not a CUDA replacement
  at the syntax level. Existing CUDA `.cu` files do not compile.
- **No graphics surface here.** Vertex / fragment / mesh / ray
  pipelines belong to [`cajeta.render`](CajetaRender.md), which
  consumes `cajeta.xpu.vulkan` as its dispatch substrate.

### 1.3 What's distinctive about Cajeta's combination

The toolchain mechanics (LLVM device codegen → PTX/cubin assembly →
driver dispatch) are standard, and well-trodden: a general-purpose
language whose own LLVM backend lowers kernels to NVPTX and launches
them via the driver is exactly what Julia (`CUDA.jl`), Numba
(`@cuda.jit`), and Rust-CUDA already do. Where Cajeta is staking
less-common ground is the *combination* of memory-safety with GPU as a
first-class, multi-backend language feature:

1. **Borrow-checking across the host/device launch boundary** — the
   deferred-borrow-until-`Stream.sync()` model (§3.5, §11). Rust-CUDA
   and cubecl inherit Rust's borrow checker for device-side code, but
   full lifetime tracking of a buffer borrowed by an in-flight launch
   until the next sync is not something the surveyed ecosystems model
   deeply. This is the genuinely novel part.
2. **Address-space-qualified types in the type system**
   (`Global<T>` / `Shared<T>` / …, §3.1.2) with backend-resolved
   numbering — most accelerator languages bury address spaces in the
   backend; few surface them as first-class, borrow-checked types.
3. **One memory-safe general-purpose language, one `xpu.core` surface,
   three peer backends** (NVPTX / AMDGPU / SPIR-V) with compile-time
   capability traits (§3.3) — Julia / KernelAbstractions get the
   portability; SYCL / DPC++ gets single-source; but capability-trait-
   bounded kernels *inside a borrow-checked language* is a particular
   synthesis.

So: the *how* (LLVM-NVPTX single-source) is established and battle-
tested — Julia is the proof that it works as a language feature, not
just a DSL. The *what* that's differentiated is doing it inside a
borrow-checked, address-space-typed, multi-backend general-purpose
language. The closest single comparison overall is Julia's `CUDA.jl` +
`KernelAbstractions.jl`, but with Rust-style static safety instead of
dynamic typing.

> **Calibration note.** This positioning reflects a survey current to
> early 2026 of a fast-moving space (Mojo, cubecl, and the Julia GPU
> stack are all evolving quickly). The borrow-across-launch claim in
> particular is the one most worth re-verifying before leaning on it in
> external messaging.

---

## 2. Architecture at a glance

```
                  Cajeta source (.cajeta)
                            |
                            v
          +--------------------------------+
          |  Frontend: lex / parse / type  |
          |  check / borrow check / mono   |
          +--------------------------------+
                            |
                            v
          +--------------------------------+
          |  cajeta.xpu.core               |
          |  - XPU MIR (mid IR)            |
          |  - capability traits           |
          |  - launch / dispatch nodes     |
          |  - address-space type system   |
          +--------------------------------+
                |           |            |
           NVIDIA   |   AMD    |   Vulkan
        lowering   |   lower  |   lower
                v           v            v
   +------------+   +------------+   +------------+
   |  LLVM IR   |   |  LLVM IR   |   |  LLVM IR   |
   |  + NVPTX   |   |  + AMDGPU  |   |  + SPIR-V  |
   |  intrinsic |   |  intrinsic |   |  + Khronos |
   +------------+   +------------+   |  extension |
        |                |           +------------+
        v                v                 |
   PTX / cubin       HSACO / ELF           v
   (per SM arch)     (per gfx)          .spv binary
        |                |                 |
        +-------------+--+-----------------+
                      |
                      v
            +---------------------+
            |  libcajeta-xpu.so   |
            |  CUDA driver API    |
            |  HIP runtime        |
            |  Vulkan loader      |
            |  (all dlopen'd      |
            |   on demand)        |
            +---------------------+
```

NVPTX, AMDGPU, and SPIR-V are all in-tree targets in LLVM 22, so the
build does not depend on the out-of-tree Khronos translator (it
remains a fallback for SPIR-V extensions LLVM is slow to pick up —
see §6.2).

---

## 3. Shared layer — `cajeta.xpu.core`

The portable surface — what every XPU program imports first, and what
every backend supports. Sized intentionally narrow: anything in
`xpu.core` must mean the same thing on NVIDIA, AMD, and Vulkan
hardware. Features that exist on only some hardware live in vendor
namespaces (§5–6).

### 3.0 Variance discipline

`xpu.core` is the surface two future backends must share with NVIDIA
without breaking. To keep that surface honest during the NVIDIA-first
phase, every candidate `xpu.core` API runs a three-column check before
landing: does it work on NVIDIA (the implementation that exists), and
does it work on AMD and Vulkan without invalidating the API shape (the
mental models). When a column would force a redesign, the API is
restructured *before* merge — not after — because the expensive case
is retrofitting across already-shipped kernels.

The full discipline — the axes of variance the three backends diverge
along, the hard pre-AMD checkpoints, and the per-PR process — lives in
[`CajetaXPU-Variance.md`](CajetaXPU-Variance.md). The table there is
the working register; rows get appended as NVIDIA-first work surfaces
new divergences.

### 3.1 Language surface

#### 3.1.1 Function attributes

```cajeta
@kernel                          // entry point, launched from host
@device                          // callable from kernel/device code
@host                            // host-only (default)
@host @device                    // emitted twice, callable from both
```

`@kernel` functions:

- Return `void`.
- Take only parameters of types that satisfy the trait
  `xpu.core.KernelArg` — primitives, POD structs, `Buffer<T>`,
  `Texture<...>`, `Sampler`, and `@push_constant` structs (Vulkan
  only).
- Cannot throw. Errors are reported through a per-launch status
  buffer; see §3.7.
- Are not virtual and cannot be overridden.

#### 3.1.2 Address-space-qualified types

```cajeta
xpu.Global<T>      // device global memory   (NV addrspace 1, AMD 1, SPIR-V StorageBuffer)
xpu.Shared<T>      // workgroup-shared       (NV addrspace 3, AMD 3 LDS, SPIR-V Workgroup)
xpu.Constant<T>    // read-only uniform      (NV addrspace 4, AMD 4, SPIR-V Uniform)
xpu.Private<T>     // per-thread private     (NV addrspace 5, AMD 5, SPIR-V Function)
xpu.Generic<T>     // flat / generic         (NV addrspace 0, AMD 0, SPIR-V Generic)
```

These are not pointer types — they are *qualified* references with
the same borrow semantics as Cajeta's normal references. The qualifier
is part of the type; a `xpu.Shared<float[256]>` cannot silently decay
to a `xpu.Generic<float[256]>` without an explicit cast that the
borrow checker accounts for.

#### 3.1.3 Launch syntax

```cajeta
@kernel
void saxpy(Buffer<float> y, Buffer<float> x, float a, uint32 n) {
    let i = xpu.thread.global_id_x();
    if (i < n) {
        y[i] = a * x[i] + y[i];
    }
}

// host side
let stream = xpu.Stream.default();
saxpy.launch(stream, grid: [(n + 255) / 256], block: [256])
     (y_buf, x_buf, 2.0f, n);
```

`launch` returns an `Event` (§3.5) and consumes a borrow of each
buffer argument for the lifetime of the launch.

### 3.2 Execution model

The shared model is the **wave-tiered** one — chosen because CUDA's
warp model, HIP's wavefront model, and Vulkan's subgroup model all
collapse onto it cleanly.

```
Grid       : 1D / 2D / 3D of Workgroups
Workgroup  : 1D / 2D / 3D of Threads, with shared memory + barrier
             (a.k.a. "block" on NV, "workgroup" on AMD/Vulkan)
Wave       : a hardware-scheduled subset of a workgroup, lock-stepped
             (a.k.a. "warp" on NV, "wavefront" on AMD, "subgroup" on Vulkan).
             Width is queryable at compile and run time.
Thread     : the unit of program execution
```

`xpu.wave.width()` is a `const` expression on a *target* basis:

| Target              | wave width                                       |
|---------------------|--------------------------------------------------|
| NVIDIA (all)        | 32                                               |
| AMD RDNA 1+         | 32 or 64 (dynamic; defaults to 32)               |
| AMD GCN / CDNA      | 64                                               |
| Vulkan subgroup     | queried at pipeline create; min/max from device  |

Kernels that need a fixed wave width declare it:

```cajeta
@kernel @wave(width: 32)
void my_reduction(...) { ... }
```

This lowers to `-mwavefrontsize32` on AMDGPU (RDNA only), is a no-op
on NVPTX, and on Vulkan emits a `LocalSize` decoration plus
`SPV_KHR_subgroup_uniform_control_flow`. A target that cannot satisfy
the request rejects the kernel at compile time (NVIDIA / AMD) or
pipeline-create time (Vulkan).

### 3.3 Capability traits

A *capability* is a feature that some devices have and others don't.
Cajeta exposes capabilities as compile-time traits so that conditional
compilation is type-checked, not preprocessor-driven.

```cajeta
trait xpu.cap.TensorCoreF16   { ... }   // NV SM 7.0+, AMD RDNA3+/CDNA, Vulkan coop_matrix
trait xpu.cap.TensorCoreBF16  { ... }   // NV SM 8.0+, AMD CDNA2+, Vulkan coop_matrix
trait xpu.cap.TensorCoreFP8   { ... }   // NV SM 8.9+ (Hopper), AMD CDNA3+, Vulkan (vendor-dep.)
trait xpu.cap.WaveBallot      { ... }   // universal
trait xpu.cap.WaveShuffle     { ... }   // universal
trait xpu.cap.AsyncCopy       { ... }   // NV SM 8.0+ (cp.async), AMD global_load_lds, Vulkan coop_matrix
trait xpu.cap.AtomicFloatAdd  { ... }   // SM 6.0+ / gfx9+, Vulkan VK_EXT_shader_atomic_float
```

A kernel that needs a capability bounds it:

```cajeta
@kernel
void gemm_mma<Target: xpu.cap.TensorCoreF16>(...) {
    xpu.tensor.mma_f16_f32::<16, 16, 16>(...);
}
```

`Target` is an implicit type parameter resolved at codegen. Each
backend implements a different set of capability traits per device;
see the matrix in §4.

### 3.4 Memory model

All three backends conform to the Vulkan memory model — a clean
superset of LLVM's "scoped atomics" model and of the PTX/HSA memory
models.

- Atomics carry an explicit scope: `Thread`, `Workgroup`, `Device`,
  `Queue`. `Queue` is only meaningful on Vulkan.
- `xpu.barrier.workgroup()` is the portable barrier — lowers to
  `bar.sync 0` (NVPTX), `s_barrier` (AMDGPU), or
  `OpControlBarrier(Workgroup, Workgroup, AcquireRelease)` (SPIR-V).
- `xpu.barrier.wave()` is the portable wave-level barrier; on most
  targets it is a no-op because waves are lock-stepped, but it is
  required on Volta+ (independent thread scheduling) and is mandatory
  before any `wave.shuffle.sync` op.

### 3.5 Streams, events, and ordering

```cajeta
class xpu.Stream    { ... }    // ordered queue of work
class xpu.Event     { ... }    // device-side fence handle
class xpu.Fence     { ... }    // host-observable signal
```

| Backend | Stream                       | Event                        | Fence                         |
|---------|------------------------------|------------------------------|-------------------------------|
| NVIDIA  | CUDA stream (`cuStream`)     | `cuEvent`                    | stream sync handle            |
| AMD     | HIP stream                   | `hipEvent`                   | stream sync handle            |
| Vulkan  | queue + command buffer chain | `VkEvent` or timeline value  | `VkFence` or timeline wait    |

The borrow checker treats `launch` as a borrow scope whose lifetime
ends at the next `Stream.sync()` or `Event.wait()` ordered-after the
launch. Concretely:

```cajeta
{
    let buf = xpu.Buffer<float>.alloc(n);
    saxpy.launch(stream, ...)(buf, ...);     // borrows `buf`
    // buf cannot be moved, freed, or aliased until...
    stream.sync();                            // ...this point.
    buf.free();                               // OK
}
```

Forgetting the sync at end of scope is a compile error — `buf` would
be dropped with a live borrow. The runtime also asserts that no
allocation it owns is freed while a stream still has a pending launch
referencing it.

### 3.6 The `Buffer<T>` and `Texture<...>` types

`Buffer<T>` is the unified handle to device memory. The only type all
three backends agree on for cross-cutting data structures.

A `Buffer<T>` holds:
- a backend-tagged storage handle (`CUdeviceptr` / `hipDeviceptr_t` /
  `VkBuffer`)
- a length in elements
- a borrow-tracked allocation owner

Construction goes through a backend-specific allocator (§7).
Indexing `buf[i]` is only legal inside a `@kernel` or `@device`
function and produces a borrow of element type. On host, you use
`buf.upload(...)`, `buf.download(...)`, or `buf.map(...)`.

`Texture<Format, Dim>` and `Sampler` are present in `xpu.core` for
compute-side image sampling. The writable twin is `Image2D` — a
`STORAGE_IMAGE` a kernel writes with `img.store(x, y, value)`
(`OpImageWrite`) and reads back on the host with `img.download(...)`;
see [`WritableImages.md`](WritableImages.md). Graphics-side texture work
(framebuffers, render targets, attachments) lives in
[`cajeta.render`](CajetaRender.md). Bindless textures are exposed via
`BindlessTexture<Format>` on all three backends but with different
underlying mechanisms (NVIDIA bindless / HIP / Vulkan descriptor
indexing).

### 3.7 Error reporting from kernels

Kernels cannot throw. Two mechanisms are provided:

1. **Per-launch status buffer.** Every launch implicitly allocates a
   small status buffer; `xpu.kernel.fail(code)` writes to it and
   triggers a host-side `XpuKernelError` on the next sync.
2. **Optional bounds-checking mode.** In `--xpu-debug` builds, all
   `Buffer<T>` indexing emits a bounds check whose failure path
   calls `xpu.kernel.fail(OutOfBounds)`. Off by default in release.

---

## 4. Capability matrix

Where each cell stands, per backend.

| Capability                       | `xpu.nvidia` (NVPTX) | `xpu.amd` (AMDGPU)       | `xpu.vulkan` (SPIR-V)                  |
|----------------------------------|----------------------|--------------------------|----------------------------------------|
| Compute kernels                  | yes                  | yes                      | yes                                    |
| Wave / subgroup shuffle, ballot  | `shfl.sync`          | `ds_swizzle` / dpp / permlane | `SPV_KHR_shader_subgroup`         |
| Wave reduce / scan               | `shfl`-based         | dpp-based                | `GroupNonUniformArithmetic`            |
| Tensor cores (F16)               | wmma / mma           | wmma (RDNA3+) / mfma     | `KHR_cooperative_matrix`               |
| Tensor cores (BF16)              | mma (SM80+)          | mma (CDNA2+)             | `KHR_cooperative_matrix`               |
| Tensor cores (FP8 E4M3 / E5M2)   | mma (SM89+)          | mma (CDNA3+)             | `KHR_cooperative_matrix` (vendor-dep.) |
| MFMA matrix instructions         | n/a                  | mfma (CDNA only)         | exposed via coop_matrix on supporting HW |
| Async copy (global → shared)     | `cp.async` / TMA     | `global_load_lds`        | KHR_cooperative_matrix has its own; otherwise emulated |
| Cluster-level barriers           | SM90+                | n/a                      | not exposed                            |
| Shared memory                    | yes                  | yes (LDS)                | yes (Workgroup storage class)          |
| Constant memory                  | yes                  | yes                      | UBO                                    |
| Bindless / descriptor indexing   | yes                  | yes                      | yes (`VK_EXT_descriptor_indexing`)     |
| Atomics on float                 | `atomicAdd` float    | `flat_atomic_fadd`       | `VK_EXT_shader_atomic_float`           |
| Raw device pointers              | yes (default)        | yes (default)            | `KHR_buffer_device_address` + `PhysicalStorageBuffer` |
| `malloc` inside kernel           | yes (device heap)    | yes                      | no                                     |
| `printf` inside kernel           | yes (`vprintf`)      | yes (hostcall)           | `KHR_non_semantic_info` debug printf   |
| Graph capture                    | yes (CUDA graphs)    | yes (HIP graphs)         | n/a (use secondary cmd buffers)        |
| Ray-tracing acceleration         | OptiX (external)     | HIP-RT (external)        | `VK_KHR_ray_tracing_pipeline`          |

The shared subset of `cajeta.xpu.core` is the rows where all three
columns say "yes" or an equivalent extension. Code that stays inside
that subset compiles for any backend with a target-flag change.

Rows where multiple backends say "yes" — even with different native
instructions — map to a single portable API in `cajeta.xpu.core.*`
whose lowering picks the right intrinsic per target. Rows where only
one backend says "yes" live in that backend's namespace
(`cajeta.xpu.nvidia.*`, `cajeta.xpu.amd.*`, `cajeta.xpu.vulkan.*`) —
see §5–6.

---

## 5. Native backends — `cajeta.xpu.nvidia` and `cajeta.xpu.amd`

The two native vendor backends share a shape: each emits LLVM IR
through an in-tree LLVM 22 target (NVPTX, AMDGPU), each links against
a vendor driver runtime (CUDA, HIP), and each exposes vendor-only
features its target hardware supports.

### 5.1 When to use the native backends

- ML training with peak throughput on a known device family.
- HPC: stencils, FFTs, particle methods, reductions where the last
  10% matters.
- CUDA / HIP graph capture, multi-stream pipelining, NCCL / RCCL
  collectives.
- Anything that needs vendor BLAS / DNN / FFT / collective libraries
  (cuBLAS, cuDNN, cuFFT, NCCL on NVIDIA; rocBLAS, MIOpen, rocFFT,
  RCCL on AMD).
- Anything specific to the vendor's microarchitecture: NVIDIA TMA,
  cluster barriers, FP8 E4M3 / E5M2; AMD's full MFMA shape menu, DPP,
  permlane, DPP8.

### 5.2 Code generation pipeline (NVIDIA)

```
Cajeta MIR
   |
   v   (NVIDIA-specific lowering pass)
LLVM IR with:
  - NV address spaces
  - calling conv `ptx_kernel`
  - target intrinsics for thread IDs, barriers, wave ops, MMA, TMA
   |
   v   (llc)
.ptx text  -> ptxas -> .cubin (per SM arch)
   |
   v
Fatbin: all SM arch variants in one container, embedded into the host
        .so or written to a sidecar .cajeta-fatbin file.
```

`--xpu-arch=sm_70,sm_80,sm_89,sm_90,sm_100,...` selects compute
capabilities. Multiple `sm_*` entries produce a multi-arch fatbin.

### 5.3 Code generation pipeline (AMD)

```
Cajeta MIR
   |
   v   (AMD-specific lowering pass)
LLVM IR with:
  - AMD address spaces
  - calling conv `amdgpu_kernel`
  - target intrinsics for thread IDs, barriers, wave ops, MFMA / WMMA
   |
   v   (llc)
ELF code object v5 (per gfx target)
   |
   v
.hsaco bundle: all gfx variants in one container, embedded into the
               host .so or written to a sidecar .cajeta-hsaco file.
```

`--xpu-arch=gfx906,gfx90a,gfx940,gfx1100,gfx1201,...` selects gfx
targets. Multiple entries produce a multi-arch bundle.

### 5.4 Intrinsic mapping (selected)

NVPTX:

| Cajeta builtin                  | NVPTX intrinsic                              |
|---------------------------------|----------------------------------------------|
| `xpu.thread.x()`                | `llvm.nvvm.read.ptx.sreg.tid.x`              |
| `xpu.workgroup.x()`             | `llvm.nvvm.read.ptx.sreg.ctaid.x`            |
| `xpu.barrier.workgroup()`       | `llvm.nvvm.barrier.sync(0)`                  |
| `xpu.wave.shuffle.sync(...)`    | `llvm.nvvm.shfl.sync.*`                      |
| `xpu.wave.ballot.sync(...)`     | `llvm.nvvm.vote.sync.ballot`                 |
| `xpu.tensor.mma::<M,N,K,..>(..)`| `llvm.nvvm.mma.m16n16k16.row.col.f16.f16` … |
| `xpu.async.copy(..)`            | `llvm.nvvm.cp.async.*` / TMA on SM90+        |

AMDGPU:

| Cajeta builtin                  | AMDGPU intrinsic                             |
|---------------------------------|----------------------------------------------|
| `xpu.thread.x()`                | `llvm.amdgcn.workitem.id.x`                  |
| `xpu.workgroup.x()`             | `llvm.amdgcn.workgroup.id.x`                 |
| `xpu.barrier.workgroup()`       | `llvm.amdgcn.s.barrier`                      |
| `xpu.wave.shuffle.sync(...)`    | `llvm.amdgcn.ds.bpermute` / `permlane*`      |
| `xpu.wave.ballot.sync(...)`     | `llvm.amdgcn.ballot`                         |
| `xpu.tensor.mma::<M,N,K,..>(..)`| `llvm.amdgcn.mfma.f32.16x16x16f16` (CDNA) or `llvm.amdgcn.wmma.f16.16x16x16.f16` (RDNA3+) |
| `xpu.async.copy(..)`            | `llvm.amdgcn.global.load.lds` / direct-to-LDS |

Where the two architectures express the same operation differently
(e.g. CDNA `mfma` vs RDNA `wmma` tile shapes), the Cajeta builtin is
overloaded by tile shape; the lowering pass picks the matching native
intrinsic, and rejects with a clear diagnostic if no native form
exists on the target.

### 5.5 Tensor core surface

The `xpu.tensor` module exposes cooperative matrix ops in two layers,
identical across NVIDIA and AMD:

```cajeta
// Layer 1: typed fragments + load/store/mma — closest to native
struct Fragment<Kind, Shape, T> { ... }   // Kind = A | B | Accumulator
xpu.tensor.load_a<16, 16, 16, f16>(addr, stride) -> Fragment<A, ..., f16>
xpu.tensor.store_c<16, 16, 16, f32>(addr, frag, stride)
xpu.tensor.mma<M, N, K, AT, BT, CT>(a: Frag, b: Frag, c: Frag) -> Frag

// Layer 2: tiled GEMM building block
xpu.tensor.gemm_tile<M, N, K, AT, BT, CT>(...)
```

Shape validity is checked at compile time against the target's
capability traits. A request for `mma<16, 16, 16, f16, f16, f32>` on
a device whose only mma shape is `16x16x4` is a compile error with a
suggested alternative.

### 5.6 Vendor-only surfaces

Each native backend exposes things its hardware uniquely supports:

`cajeta.xpu.nvidia.*`:
- **TMA** (Tensor Memory Accelerator) — `tma.copy`, `tma.copy.bulk`
- **Cluster barriers** — SM90+ thread block clusters
- **FP8 E4M3 / E5M2 mma** — Hopper-specific data paths
- **NVSHMEM** — symmetric-heap multi-GPU communication
- **CUDA Graph specifics** — node-level update / instantiation
- Vendor-library FFI under `cajeta.xpu.nvidia.{cublas, cudnn, cufft, nccl}`

`cajeta.xpu.amd.*`:
- **Full MFMA shape menu** — every shape on every CDNA architecture
- **RDNA permlane / DPP / DPP8** — full menu of cross-lane data paths
- **HIP Graph specifics** — node-level update / instantiation
- Vendor-library FFI under `cajeta.xpu.amd.{rocblas, miopen, rocfft, rccl}`

A kernel that imports anything from `xpu.nvidia` is implicitly tagged
`@backend(xpu.nvidia)` and is only emitted for NVIDIA targets; the
same holds for `xpu.amd` and `xpu.vulkan`. Portable kernels — those
touching only `xpu.core` — are emitted for every requested target.
The compiler keeps all kinds in the same translation unit; the linker
drops emit-disabled kernels per target.

### 5.7 Graph capture (native backends only)

```cajeta
let g = stream.begin_capture();
foo.launch(stream, ...)(...);
bar.launch(stream, ...)(...);
let exec = g.end_capture();
exec.launch(stream);              // launches the whole graph
```

Exposes CUDA Graphs and HIP Graphs through one API. Vulkan has no
direct equivalent; the closest is secondary command buffers (see §6).

---

## 6. Portable backend — `cajeta.xpu.vulkan`

The cross-vendor portable backend. Compiles to SPIR-V, dispatches
through the Vulkan loader. The same SPIR-V binary runs on NVIDIA, AMD,
Intel Arc, Apple (via MoltenVK), Android, and SteamDeck — provided
the device exposes the extensions the SPIR-V module declares.

### 6.1 When to use the Vulkan backend

- Software that must run on every vendor's hardware without per-
  vendor binaries.
- ML inference where ~80% of native perf is acceptable in exchange
  for a single shipping binary.
- Compute kernels embedded in a Vulkan-based renderer (see
  [`cajeta.render`](CajetaRender.md)). One device, one command stream
  shared with rasterization or ray tracing.
- Anything where the Vulkan ecosystem (RenderDoc, NSight Graphics,
  Radeon GPU Profiler, MoltenVK / Vulkan SDK) is the debugging
  surface.

### 6.2 Code generation pipeline

```
Cajeta MIR
   |
   v   (Vulkan-specific lowering)
LLVM IR with:
  - SPIR-V address spaces (Function / Workgroup / Uniform / StorageBuffer / Image / ...)
  - explicit descriptor-set / binding decorations
  - subgroup / cooperative-matrix intrinsics
   |
   v   (LLVM SPIR-V backend, mtriple=spirv-unknown-vulkan)
.spv  (one file per kernel module)
   |
   v   (spirv-opt / spirv-val in --xpu-debug)
final .spv
   |
   v
Embedded into the binary's resources, or written to a .cajeta-spv
sidecar (mmap-loaded by the XPU runtime).
```

When the in-tree SPIR-V backend cannot emit a needed extension opcode
(rare but happens for newly-published Khronos extensions), the Vulkan
lowering falls back to the Khronos `SPIRV-LLVM-Translator` for that
module. The choice is per-translation-unit and recorded in build
metadata.

### 6.3 Subgroup / cooperative matrix surface

`xpu.wave.*` lowers to `GroupNonUniform*` SPIR-V opcodes under
`SPV_KHR_shader_subgroup`. Where SPIR-V exposes things CUDA / HIP do
not (e.g. `GroupNonUniformQuadBroadcast` for shader quads), those
live in `cajeta.xpu.vulkan.wave` only — not in `cajeta.xpu.core`.

`xpu.tensor.*` lowers to `OpCooperativeMatrix*KHR` under
`SPV_KHR_cooperative_matrix`. The fragment shape is queried from
`vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` at pipeline
create. The same shape-validity check from §5.5 applies, but it runs
at pipeline-create time, not compile time, because the supported
shapes are not knowable from the SPIR-V binary alone.

**Maximal reconvergence.** A cross-lane `Wave` op (`shuffleSync`,
`ballotSync`, `reduceSum`) only sees the lanes the *source* structure implies
if the implementation reconverges maximally. The Vulkan path requests this
automatically: any kernel that lowers such an op gets
`OpExecutionMode … MaximallyReconvergesKHR` (`SPV_KHR_maximal_reconvergence`,
emitted from an `enable-maximal-reconvergence` function attribute — no fork).
The request is **gated on use** — a kernel with only per-lane queries
(`width`/`laneId`/`isFirstLane`) or no wave ops at all carries no extra device
requirement. NVIDIA/AMD/CPU need no equivalent: their ISA + LLVM convergence
already define wave-op convergence, so the seam (`onSubgroupOpsUsed`) is a
Vulkan-only override.

### 6.4 Host integration

```cajeta
// Stand-alone: create the device ourselves
let device = xpu.vulkan.Device.create({ physical_device: 0 });

// Or: hand us an existing VkDevice (e.g. from a renderer)
let device = xpu.vulkan.Device.from_handle(vkDevice, queueFamilyIndex);

let pipe = my_kernel.compile_for(device);
cmd.bind(pipe).dispatch(grid);
```

A user that already has a Vulkan engine (typical via
`cajeta.render`) and just wants to dispatch Cajeta compute kernels
into it gets the integration path — no competing instance, no extra
queue, no duplicated descriptor pool.

### 6.5 Hot reload

Because SPIR-V is data, not code, the Vulkan backend ships an
`xpu.vulkan.Reloader` that watches `.cajeta-spv` sidecar files on
disk, rebuilds the pipeline on change, and atomically swaps it on the
next frame. This is the loop game / interactive devs expect; it does
not exist on the native backends.

---

## 7. Vendor library FFI

Vendor BLAS / DNN / FFT / collective libraries live in their backend's
namespace as thin bindings — not portability shims:

```cajeta
import cajeta.xpu.nvidia.cublas;    // cuBLAS
import cajeta.xpu.nvidia.cudnn;     // cuDNN
import cajeta.xpu.nvidia.cufft;     // cuFFT
import cajeta.xpu.nvidia.nccl;      // NCCL collectives

import cajeta.xpu.amd.rocblas;      // rocBLAS
import cajeta.xpu.amd.miopen;       // MIOpen
import cajeta.xpu.amd.rocfft;       // rocFFT
import cajeta.xpu.amd.rccl;         // RCCL collectives
```

A small set of "obviously portable" wrappers lives in `cajeta.xpu.core`
and dispatches at runtime to whichever vendor lib is loaded:

```cajeta
cajeta.xpu.core.blas        // matmul, gemv, axpy, ...
cajeta.xpu.core.dnn         // conv, pooling, batch norm, attention
cajeta.xpu.core.fft         // 1D/2D/3D real and complex
cajeta.xpu.core.collective  // all-reduce, all-gather, reduce-scatter
```

The portable wrappers are the layer `cajeta.math.*`, `cajeta.prism`,
and `cajeta.torch` build on. The vendor namespaces are for kernels
written against a known device.

`cajeta.xpu.vulkan` has no vendor library FFI in this sense — its
"BLAS" is whatever the user writes as a SPIR-V kernel, or whatever the
portable `xpu.core.blas` wrapper dispatches into when Vulkan is the
active backend (typically a hand-written cooperative-matrix kernel).

---

## 8. Memory and allocator story

Each backend owns its allocator. Buffers do not migrate between
backends without explicit interop (§8.3).

| Backend       | Allocator                                            |
|---------------|------------------------------------------------------|
| NVIDIA / CUDA | `cuMemAlloc` / `cuMemAllocAsync` (stream-ordered)    |
| AMD / HIP     | `hipMalloc` / `hipMallocAsync`                       |
| Vulkan        | VMA (Vulkan Memory Allocator) wrapped as `xpu.vulkan.HeapPool` |

### 8.1 The `Buffer<T>` type

`Buffer<T>` is generic over a backend tag:

```cajeta
xpu.nvidia.Buffer<float>      // CUDA storage
xpu.amd.Buffer<float>         // HIP storage
xpu.vulkan.Buffer<float>      // VkBuffer + VkDeviceMemory
xpu.Buffer<float>             // alias for the active backend in a build
```

`xpu.Buffer<T>` is the type higher-level libraries (`cajeta.math`,
Prism, Torch, `cajeta.render`) write against. The build selects which
backend it resolves to. Code that must address multiple backends in
the same translation unit imports the qualified names.

### 8.2 Stream-ordered vs blocking allocation

NVIDIA and AMD both expose stream-ordered allocators — `cuMemAllocAsync`
and `hipMallocAsync` — which avoid sync-on-free for buffers tied to
one stream. `xpu.Buffer.alloc_async(stream, n)` opts into this on the
native backends; Vulkan falls through to its pool allocator with no
synchronization difference.

### 8.3 Cross-backend interop

Interop is the explicit, opt-in path for buffers to move between
backends. Three transports:

- **Host staging.** Always works. Cost: one round-trip to host memory.
- **External memory.** `VK_KHR_external_memory_fd` +
  `cuImportExternalMemory` / `hipImportExternalMemory`. Zero-copy when
  both backends are attached to the same physical device.
- **DMABUF on Linux.** Same idea, file-descriptor-based; works for
  NVIDIA (open-kernel driver), AMD, and Intel.

Interop lives in `cajeta.xpu.interop`, an optional sub-module that
links the runtimes of every backend a project depends on. Plain
single-backend consumers don't pay for it.

---

## 9. Library layout — `libcajeta-xpu.so`

```
libcajeta-runtime.so       always present, no XPU dependency
libcajeta-xpu.so           XPU runtime — all backends in one .so;
                           dlopens libcuda.so.1, libamdhip64.so,
                           and/or libvulkan.so.1 on demand. No per-
                           backend sub-library.
libcajeta-xpu-interop.so   optional; only needed when buffers cross
                           backend boundaries within one process.
```

A binary that uses only the NVIDIA backend still links
`libcajeta-xpu.so` but the HIP and Vulkan drivers are never opened —
no extra processes, no extra memory, no extra symbols visible in
`ldd` output. The same holds for AMD-only and Vulkan-only consumers.
A binary that uses two or three backends opens the respective drivers
on first use, paying the cost only when it asks.

### 9.1 Symbol namespace conflict avoidance

- All XPU symbols live in `cajeta::xpu::` (C++ name mangling) and
  `cajeta.xpu.*` (Cajeta module path).
- Backend-specific symbols live in `cajeta::xpu::nvidia::`,
  `cajeta::xpu::amd::`, `cajeta::xpu::vulkan::`.
- The shared core uses `cajeta::xpu::core::`.
- Symbols from `libcuda.so`, `libamdhip64.so`, and `libvulkan.so`
  are loaded via `dlopen` with `RTLD_LOCAL | RTLD_NOW`. They are
  never re-exported by Cajeta libraries.

### 9.2 Device-context coexistence

A real concern: NVIDIA's Vulkan driver and CUDA driver each create
their own context on the same physical GPU, and naive code can end up
with two contexts fighting for residency. The Vulkan backend's
`Device` object exposes a flag:

```cajeta
let d = xpu.vulkan.Device.create({
    physical_device: 0,
    coexist_with_native: true,        // default: false
});
```

When set, the Vulkan backend will:

- Enable `VK_KHR_external_memory`, `VK_KHR_external_semaphore`, and
  `VK_KHR_timeline_semaphore` at device creation.
- Prefer the Vulkan queue family that supports compute + transfer
  without graphics (so it competes least with native CUDA / HIP
  streams).
- Register the Vulkan device with `cajeta.xpu.interop` so that native
  buffers can be imported zero-copy when both target the same device
  UUID.

The default is `false` because most users want exactly one path.

### 9.3 Compile-time selection

```toml
# In a project's Cajeta.toml
[xpu]
backends = ["nvidia", "amd", "vulkan"]  # any subset
default  = "nvidia"                     # which one xpu.* aliases resolve to
nvidia.targets = ["sm_80", "sm_90", "sm_100"]
amd.targets    = ["gfx90a", "gfx940", "gfx1100"]
vulkan.target  = "vulkan-1.3"
vulkan.extensions = ["cooperative_matrix", "buffer_device_address"]
```

A project that only ever wants the Vulkan backend declares
`backends = ["vulkan"]` and the NVPTX / AMDGPU codegen passes are not
even instantiated by the compiler. The build is shorter, the binary
smaller, and the user cannot accidentally call a native-backend-only
API.

---

## 10. Toolchain integration

### 10.1 Compiler flags

```
--xpu-backend=nvidia|amd|vulkan|all   # one, several, or all
--xpu-arch=<list>                     # sm_80,sm_90,gfx90a,gfx1100,...
--xpu-vulkan=1.2|1.3
--xpu-extensions=<list>
--xpu-debug                           # bounds checks, spirv-val, ptxas -lineinfo
--xpu-emit=ir|asm|bin                 # stop early for inspection
```

### 10.2 Build artifacts

- `*.cajeta` → `*.cajeta-mir` (debug) → `*.ll` (debug) → backend output.
- NVIDIA: `*.ptx` + `*.cubin` per arch, fatbinned into `*.cajeta-fatbin`.
- AMD: `*.hsaco` per arch, bundled into `*.cajeta-hsaco`.
- Vulkan: one `*.spv` per kernel module, bundled into `*.cajeta-spv`.
- All bundle types embed into the host `.so` / `.exe` by default;
  `--xpu-emit-sidecar` keeps them out for asset-bundle workflows
  (typical for games).

### 10.3 Diagnostics

The `--diag-hints` "did you mean" pass extends with XPU-specific
suggestions:

- "`xpu.tensor.mma<16, 16, 16, f16, f16, f32>` is not available on
  `gfx906`; nearest available shapes: `<16, 16, 4>`, `<32, 32, 4>`."
- "`xpu.vulkan.RayQuery` requires `--xpu-extensions=ray_query` — add
  it to `Cajeta.toml` or this flag."
- "`@push_constant` is Vulkan-only; this kernel is being compiled for
  `--xpu-backend=all`. Move the struct to a uniform buffer or guard
  the kernel with `@backend(xpu.vulkan)`."
- "TMA is `xpu.nvidia`-only; the equivalent on AMD is
  `xpu.amd.global_load_lds`. The portable path is
  `xpu.core.async_copy` (lower throughput, broader compatibility)."

### 10.4 Debuggers and profilers

- NVIDIA: NSight Compute / NSight Systems / Compute Sanitizer. Cajeta
  emits line tables matching the source `.cajeta` file.
- AMD: rocprof / rocprofv2 / Radeon GPU Profiler. Same line tables.
- Vulkan: RenderDoc, NSight Graphics, Radeon GPU Profiler. SPIR-V
  carries `OpLine` mapping back to `.cajeta` source.

---

## 11. Borrow checker interaction (notable cases)

The borrow checker treats XPU launches as a deferred borrow with a
lifetime ending at the next sync. Three cases bite users in CUDA-land
and Cajeta should not let them through:

1. **Buffer freed while launch in flight.**

   ```cajeta
   let buf = xpu.Buffer<float>.alloc(n);
   kernel.launch(stream, ...)(buf, ...);
   buf.free();                       // ERROR: live borrow until stream.sync()
   ```

2. **Shared memory aliasing.**

   ```cajeta
   @kernel
   void f(xpu.Shared<float[256]> a) {
       let r1 = &mut a[..128];
       let r2 = &mut a[..];          // ERROR: overlaps r1
   }
   ```

3. **Cross-stream WAR / RAW.**

   A buffer written by `kernel_a` on `stream_a` and read by `kernel_b`
   on `stream_b` requires an explicit `stream_b.wait(event_from_a)`
   or the borrow checker rejects the launch. Cajeta does *not* infer
   cross-stream dependencies — that's a runtime cost the user must
   opt into.

These are checked uniformly across NVIDIA, AMD, and Vulkan.

---

## 12. Phasing

A reasonable order of implementation. Each phase is independently
useful; do not start a later phase until its predecessor has shipped
a working sample.

1. **`cajeta.xpu.core` MIR + capability traits.** No codegen yet —
   just types and the launch-site syntax that the borrow checker can
   reason about. Lower MIR to a CPU-emulation backend so kernels
   compile and run (slowly) on the host. Unblocks frontend work and
   tests.
2. **NVIDIA backend, compute only.** SAXPY, GEMM (no tensor cores),
   reduction. Single SM arch. CUDA driver runtime. End-to-end build:
   `cajeta` → `.cubin` → launch.
3. **AMD backend, compute only.** Same kernels, gfx90a or gfx1100.
   HIP runtime. Validate the shared layer truly is shared.
4. **NVIDIA + AMD tensor cores.** WMMA on NVPTX, MFMA on CDNA, WMMA
   on RDNA3+. A real GEMM that beats v2 GEMM.
5. **Vulkan backend, compute only.** Same compute kernels through
   SPIR-V. The first time portability is real.
6. **Vulkan cooperative matrix.** GEMM through SPIR-V; compare to
   native GEMM perf on the same hardware.
7. **Vendor library FFI.** cuBLAS / cuDNN bindings, then rocBLAS /
   MIOpen. The `cajeta.math.linalg` and `cajeta.math.nn` layers
   start consuming these.
8. **Interop sub-module.** External memory between backends on Linux.
9. **Graph capture (native backends).** CUDA Graphs, HIP Graphs.
   ML-training-grade scheduling.

Graphics phases (raster, ray tracing, mesh shaders) are tracked in
[`cajeta.render`'s phasing](CajetaRender.md#phasing), not here.

---

## 13. Open questions

- **`@kernel` vs `@compute`.** A kernel written for native backends
  that does `xpu.kernel.malloc(...)` cannot trivially become a Vulkan
  compute shader. Current answer: one attribute (`@kernel`), with a
  `@backend(xpu.nvidia)` / `@backend(xpu.amd)` / `@backend(xpu.vulkan)`
  annotation when the body uses backend-specific features.
- **MLIR as the IR layer.** A larger question for Cajeta as a whole:
  would switching the mid-IR from raw LLVM IR to MLIR (with the `gpu`
  dialect at this layer) pay off enough to justify the dependency? It
  would simplify §5–6 considerably. Out of scope for this doc; flagged
  for a separate decision.
- **WebGPU / Dawn as a fourth backend.** Browser / WGSL is a different
  shape than NVPTX, AMDGPU, or SPIR-V. Not in v1; if added, slots in
  as `cajeta.xpu.webgpu` alongside the others.
- **NPU integration.** Hexagon / Neural Engine / Meteor Lake NPU /
  XDNA. Each has its own runtime and execution model; they share
  `cajeta.xpu.core`'s `Buffer<T>` / `Stream` / `Event` shape but
  diverge sharply on the kernel surface (most NPUs don't have wave-
  tiered execution — they're VLIW or systolic). Likely lands as
  separate sibling namespaces (`cajeta.xpu.hexagon`,
  `cajeta.xpu.ane`, ...) that opt out of the wave model.
- **TPU-style accelerators.** Google's TPUs and similar silicon
  expose XLA-compatible HLO, not a CUDA-shaped surface. Out of v1;
  would need a separate compile path.

---

## 14. Summary

`cajeta.xpu` is one library with three peer backends — NVIDIA via
NVPTX, AMD via AMDGPU, Vulkan via SPIR-V — under a single namespace
(`cajeta.xpu.core`) shared across all of them. Vendor extensions live
in `cajeta.xpu.nvidia`, `cajeta.xpu.amd`, and `cajeta.xpu.vulkan`,
each named for what they actually are. The library ships as one `.so`
and `dlopen`s drivers on demand; binaries pay only for the backends
they use. Graphics pipelines live one layer up in
[`cajeta.render`](CajetaRender.md), built on `cajeta.xpu.vulkan`. The
prefix `xpu` is forward-compatible: when NPU / TPU / FPGA backends
arrive, they slot in alongside the GPUs without renaming anything the
rest of the codebase depends on.
