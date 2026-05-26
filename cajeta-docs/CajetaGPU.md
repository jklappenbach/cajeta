# CajetaGPU — GPU Backend Architecture

This document specifies Cajeta's GPU code generation and runtime
layer. It is the layer that `cajeta.ml`, `cajeta.prism`, `cajeta.torch`
and any game-side renderer plug into.

There are two backends, and they are deliberately separate:

- **Forge** — `cajeta.gpu.forge` — native code generation through
  LLVM's NVPTX and AMDGPU targets. Tight, low overhead, full hardware
  surface. Primary audience: ML training/inference, HPC, simulation.
- **Shade** — `cajeta.gpu.shade` — SPIR-V code generation, dispatched
  through Vulkan. Portable, single-binary-runs-anywhere, exposes the
  full Vulkan pipeline (compute, vertex, fragment, mesh, ray). Primary
  audience: game dev, cross-vendor compute, ML-inside-renderer.

They share Cajeta's frontend (lexer, parser, type checker, borrow
checker, generic instantiation) and a small mid-level GPU IR. They
diverge at the codegen and runtime layers, and they ship as separate
shared libraries that can coexist in one process without symbol or
context conflicts.

This document is the spec for that split: what each backend covers,
what is shared, what is intentionally not shared, and how a single
Cajeta program can target one, the other, or both.

---

## 1. Goals and non-goals

### 1.1 Goals

1. A Cajeta program that says "run this kernel on the GPU" should be
   compilable for either backend by changing a target flag, *as long
   as* it uses only features in the shared subset (§4).
2. ML / HPC users who need tensor cores, MFMA, warp-level
   reductions, async copies, and CUDA-graph-style scheduling get them
   without going through a portability layer that hides hardware.
3. Game dev users get first-class graphics pipelines (vertex /
   fragment / mesh / ray) and not just compute, plus shader hot-reload
   and a SPIR-V binary that ships in the game's asset bundle.
4. ML-inside-a-game (e.g. neural upscaler, neural NPC, physics
   surrogate) is a supported scenario via Shade's
   `VK_KHR_cooperative_matrix` path — one runtime, one device, one
   command stream.
5. A single Cajeta binary may link both backends; runtime device
   discovery picks one without dragging the other's driver into the
   process unless asked.
6. The borrow checker keeps its teeth across the host/device boundary:
   no use-after-free of a buffer whose only remaining user is an
   in-flight kernel.

### 1.2 Non-goals

- **No HLSL / GLSL / WGSL / Slang frontend.** Cajeta is the source
  language.
- **No metal / DX12 backend in v1.** Not ruled out; the Shade IR
  abstraction is structured so a DXIL backend could be added later
  without restructuring the frontend.
- **No automatic kernel autotuning.** Tile sizes, block shapes, and
  pipeline depths are written by the kernel author or by a higher-
  level library (Prism, Torch). Forge and Shade expose introspection;
  they do not search.
- **No CUDA-source compatibility.** Cajeta is not a CUDA replacement at
  the syntax level. Existing CUDA `.cu` files do not compile.

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
        |  cajeta.gpu.core               |
        |  - GPU MIR (mid IR)            |
        |  - capability traits           |
        |  - launch / dispatch nodes     |
        |  - address-space type system   |
        +--------------------------------+
                |                    |
       Forge    |                    |    Shade
       lowering |                    |    lowering
                v                    v
   +----------------------+   +-----------------------+
   |  LLVM IR             |   |  LLVM IR             |
   |  + NVPTX intrinsics  |   |  + SPIR-V intrinsics |
   |  + AMDGPU intrinsics |   |  + Khronos extension |
   +----------------------+   +-----------------------+
              |                          |
              v                          v
         PTX / cubin                  .spv binary
         HSACO / code obj
              |                          |
              v                          v
   +----------------------+   +-----------------------+
   |  libcajeta-forge.so  |   |  libcajeta-shade.so   |
   |  CUDA driver API     |   |  Vulkan loader        |
   |  HIP runtime         |   |  vkCmd* recording     |
   +----------------------+   +-----------------------+
```

Both backends emit through LLVM 20. NVPTX, AMDGPU, and SPIR-V are all
in-tree targets in LLVM 20, so the build does not depend on the
out-of-tree Khronos translator (it remains a fallback for SPIR-V
extensions LLVM is slow to pick up — see §6.3).

---

## 3. Shared layer — `cajeta.gpu.core`

This is the part of the GPU stack that lives in the main `cajeta`
compiler binary and the main `cajeta.runtime`. Both Forge and Shade
depend on it. Neither user code nor backend code touches the other
backend through this layer.

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
  `gpu.core.KernelArg` — primitives, POD structs, `Buffer<T>`,
  `Texture<...>`, `Sampler`, and `@push_constant` structs (Shade
  only).
- Cannot throw. Errors are reported through a per-launch status
  buffer; see §3.7.
- Are not virtual and cannot be overridden.

#### 3.1.2 Address-space-qualified types

```cajeta
gpu.Global<T>      // device global memory   (NV addrspace 1, AMD 1)
gpu.Shared<T>      // workgroup-shared       (NV addrspace 3, AMD 3 LDS)
gpu.Constant<T>    // read-only uniform      (NV addrspace 4, AMD 4)
gpu.Private<T>     // per-thread private     (NV addrspace 5, AMD 5)
gpu.Generic<T>     // flat / generic         (NV addrspace 0, AMD 0)
```

These are not pointer types — they are *qualified* references with the
same borrow semantics as Cajeta's normal references. The qualifier is
part of the type; a `gpu.Shared<float[256]>` cannot silently decay to a
`gpu.Generic<float[256]>` without an explicit cast that the borrow
checker accounts for.

#### 3.1.3 Launch syntax

```cajeta
@kernel
void saxpy(Buffer<float> y, Buffer<float> x, float a, uint32 n) {
    let i = gpu.thread.global_id_x();
    if (i < n) {
        y[i] = a * x[i] + y[i];
    }
}

// host side
let stream = gpu.Stream.default();
saxpy.launch(stream, grid: [(n + 255) / 256], block: [256])
     (y_buf, x_buf, 2.0f, n);
```

`launch` returns an `Event` (§3.5) and consumes a borrow of each
buffer argument for the lifetime of the launch.

### 3.2 Execution model

The shared model is the **wave-tiered** one — chosen because both
CUDA's warp model and Vulkan's subgroup model collapse onto it cleanly.

```
Grid  : 1D / 2D / 3D of Workgroups
Workgroup (a.k.a. "block")
        : 1D / 2D / 3D of Threads, with shared memory + barrier
Wave  (a.k.a. "warp" / "subgroup")
        : a hardware-scheduled subset of a workgroup, lock-stepped,
          width is queryable at compile and run time
Thread : the unit of program execution
```

`gpu.wave.width()` is a `const` expression on a *target* basis:

| Target           | wave width      |
|------------------|-----------------|
| NVIDIA (all)     | 32              |
| AMD RDNA 1+      | 32 or 64 (dynamic; defaults to 32) |
| AMD GCN / CDNA   | 64              |
| Vulkan subgroup  | queried at pipeline create; min/max from device props |

Kernels that need a fixed wave width declare it:

```cajeta
@kernel @wave(width: 32)
void my_reduction(...) { ... }
```

Forge lowers this to `-mwavefrontsize32` on AMDGPU (RDNA only) and is a
no-op on NVPTX. Shade lowers it to
`SPV_KHR_subgroup_uniform_control_flow` + a `LocalSize` decoration
chosen to match. A target that cannot satisfy the request rejects the
kernel at compile time (Forge) or pipeline-create time (Shade).

### 3.3 Capability traits

A *capability* is a feature that some devices have and others do not.
Cajeta exposes capabilities as compile-time traits so that conditional
compilation is type-checked, not preprocessor-driven.

```cajeta
trait gpu.cap.TensorCoreF16   { ... }     // NV SM 7.0+, AMD RDNA3+/CDNA
trait gpu.cap.TensorCoreBF16  { ... }     // NV SM 8.0+, AMD CDNA2+
trait gpu.cap.TensorCoreFP8   { ... }     // NV SM 8.9+ (Hopper), AMD CDNA3+
trait gpu.cap.WaveBallot      { ... }     // universal
trait gpu.cap.WaveShuffle     { ... }     // universal
trait gpu.cap.AsyncCopy       { ... }     // NV SM 8.0+, AMD via global_load_lds
trait gpu.cap.AtomicFloatAdd  { ... }     // SM 6.0+ / gfx9+
```

A kernel that needs a capability bounds it:

```cajeta
@kernel
void gemm_mma<Target: gpu.cap.TensorCoreF16>(...) {
    gpu.tensor.mma_f16_f32::<16, 16, 16>(...);
}
```

`Target` is an implicit type parameter resolved at codegen. Forge and
Shade each implement a different set of capability traits per device;
see §4.

### 3.4 Memory model

Both backends conform to the Vulkan memory model (which is a superset
of the LLVM "scoped atomics" model and a clean superset of the
PTX/HSA memory models).

- Atomics carry an explicit scope: `Thread`, `Workgroup`, `Device`,
  `Queue`. `Queue` is only meaningful on Shade.
- `gpu.barrier.workgroup()` is the portable barrier; Forge lowers to
  `bar.sync 0` (NVPTX) or `s_barrier` (AMDGPU), Shade to
  `OpControlBarrier(Workgroup, Workgroup, AcquireRelease)`.
- `gpu.barrier.wave()` is the portable wave-level barrier; on most
  targets it is a no-op because waves are lock-stepped, but it is
  required on Volta+ (independent thread scheduling) and is mandatory
  before any `wave.shuffle.sync` op.

### 3.5 Streams, events, and ordering

```cajeta
class gpu.Stream    { ... }    // ordered queue of work
class gpu.Event     { ... }    // GPU-side fence handle
class gpu.Fence     { ... }    // host-observable signal
```

- Forge maps `Stream` to a CUDA stream / HIP stream, `Event` to
  `cuEvent` / `hipEvent`, `Fence` to a stream sync handle.
- Shade maps `Stream` to a Vulkan queue + command buffer chain, `Event`
  to `VkEvent` (or timeline semaphore value), `Fence` to `VkFence` or
  timeline semaphore wait.

The borrow checker treats `launch` as a borrow scope whose lifetime
ends at the next `Stream.sync()` or `Event.wait()` on a stream that
ordered-after the launch.  Concretely:

```cajeta
{
    let buf = gpu.Buffer<float>.alloc(n);
    saxpy.launch(stream, ...)(buf, ...);     // borrows `buf`
    // buf cannot be moved, freed, or aliased until...
    stream.sync();                            // ...this point.
    buf.free();                               // OK
}
```

Forgetting the sync at end of scope is a compile error (`buf` would be
dropped with a live borrow). The runtime also asserts that no
allocation it owns is freed while a stream still has a pending launch
referencing it.

### 3.6 The `Buffer<T>` and `Texture<...>` types

`Buffer<T>` is the unified handle to device memory. It is the only type
that both backends agree on for cross-cutting data structures.

- Internally, a `Buffer<T>` holds:
  - a backend-tagged storage handle (CUdeviceptr / hipDeviceptr / VkBuffer)
  - a length in elements
  - a borrow-tracked allocation owner
- Construction goes through a backend-specific allocator (§7).
- Indexing `buf[i]` is only legal inside a `@kernel` or `@device`
  function and produces a borrow of element type. On host, you use
  `buf.upload(...)`, `buf.download(...)`, or `buf.map(...)`.

`Texture<Format, Dim>` and `Sampler` are richer types that Shade fully
supports (graphics pipelines need them) and that Forge supports for
compute via CUDA / HIP texture objects. Bindless textures are exposed
via `BindlessTexture<Format>` on both backends but with different
underlying mechanisms (NVIDIA bindless / HIP / Vulkan descriptor
indexing).

### 3.7 Error reporting from kernels

Kernels cannot throw. Two mechanisms are provided:

1. **Per-launch status buffer.** Every launch implicitly allocates a
   small status buffer; `gpu.kernel.fail(code)` writes to it and
   triggers a host-side `GpuKernelError` on the next sync.
2. **Optional bounds-checking mode.** In `--gpu-debug` builds, all
   `Buffer<T>` indexing emits a bounds check whose failure path calls
   `gpu.kernel.fail(OutOfBounds)`. Off by default in release.

---

## 4. Capability matrix (what each backend can do)

| Capability                       | Forge / NVPTX     | Forge / AMDGPU       | Shade / Vulkan SPIR-V                    |
|----------------------------------|-------------------|----------------------|------------------------------------------|
| Compute kernels                  | yes               | yes                  | yes                                      |
| Graphics pipeline (vert/frag)    | no                | no                   | yes                                      |
| Mesh / task shaders              | no                | no                   | yes (VK_EXT_mesh_shader)                 |
| Ray tracing pipeline             | OptiX (external)  | HIP-RT (external)    | yes (VK_KHR_ray_tracing_pipeline)        |
| Wave / subgroup shuffle, ballot  | shfl.sync         | ds_swizzle / dpp / permlane | SPV_KHR_shader_subgroup           |
| Wave reduce / scan               | shfl-based        | dpp-based            | GroupNonUniformArithmetic                |
| Tensor cores (F16)               | wmma / mma        | wmma (RDNA3+)        | KHR_cooperative_matrix                   |
| Tensor cores (BF16)              | mma (SM80+)       | mma (CDNA2+)         | KHR_cooperative_matrix                   |
| Tensor cores (FP8 / E4M3 / E5M2) | mma (SM89+)       | mma (CDNA3+)         | KHR_cooperative_matrix (vendor-dep.)     |
| MFMA / matrix instructions       | n/a               | mfma (CDNA only)     | exposed via coop_matrix on supporting HW |
| Async copy (global -> shared)    | cp.async / TMA    | global_load_lds      | KHR_cooperative_matrix has its own; otherwise emulated |
| Cluster-level barriers           | SM90+             | n/a                  | not exposed                              |
| Shared memory                    | yes               | yes (LDS)            | yes (Workgroup storage class)            |
| Constant memory                  | yes               | yes                  | UBO                                      |
| Bindless / descriptor indexing   | yes               | yes                  | yes (VK_EXT_descriptor_indexing)         |
| Atomics on float                 | atomicAdd float   | flat_atomic_fadd     | VK_EXT_shader_atomic_float               |
| Raw device pointers              | yes (default)     | yes (default)        | KHR_buffer_device_address + PhysicalStorageBuffer |
| `malloc` inside kernel           | yes (device heap) | yes                  | no                                       |
| printf inside kernel             | yes (vprintf)     | yes (hostcall)       | KHR_non_semantic_info debug printf       |
| CUDA / HIP graph capture         | yes               | yes (HIP graphs)     | n/a (use secondary cmd buffers)          |

The shared subset of `cajeta.gpu.core` is exactly the rows where all
three columns say "yes" or an equivalent extension. Code that stays
inside that subset compiles for any target with a target-flag change.

Within Forge specifically, rows where both NVPTX and AMDGPU say "yes"
map to a single portable API in `cajeta.gpu.forge.*` whose lowering
picks the right native intrinsic per target. Rows where only one
vendor column says "yes" live in that vendor's namespace
(`cajeta.gpu.forge.nv.*` or `cajeta.gpu.forge.amd.*`) — see §5.2.

---

## 5. Backend A — Forge (`cajeta.gpu.forge`)

### 5.1 When to use Forge

- ML training with peak throughput on a known device family.
- HPC: stencils, FFTs, particle methods, reductions where the last
  10% matters.
- CUDA / HIP graph capture, multi-stream pipelining, NCCL / RCCL
  collectives.
- Anything that needs `nvshmem`, `cuBLAS`, `rocBLAS`, `cuDNN`,
  `MIOpen`, `cuFFT`, `rocFFT`, etc. — Forge exposes vendor-library FFI.

### 5.2 Single library, vendor-namespaced surface

Forge ships as one library — `libcajeta-forge.so` — that supports
both NVIDIA and AMD devices. The CUDA driver (`libcuda.so.1`) and HIP
runtime (`libamdhip64.so`) are `dlopen`'d on demand; either or both
being absent at runtime is not a startup failure, it just removes the
corresponding device family from `gpu.devices()`.

The user-facing API is split into a portable surface and two
vendor-specific namespaces:

```
cajeta.gpu.forge.*         portable across NV and AMD
                           (thread / wave / tensor / barrier / buffer /
                            stream / event / module / launch)
cajeta.gpu.forge.nv.*      NVIDIA-only: TMA, cluster barriers, FP8
                           E4M3 / E5M2, CUDA graph specifics, NVSHMEM,
                           cuBLAS / cuDNN / cuFFT / NCCL FFI
cajeta.gpu.forge.amd.*     AMD-only: full MFMA shape menu, RDNA
                           dpp / permlane variants, HIP graph specifics,
                           rocBLAS / MIOpen / rocFFT / RCCL FFI
```

A kernel that imports anything from `forge.nv` is implicitly tagged
`@backend(forge.nv)` and is only emitted for NVIDIA targets; the same
holds for `forge.amd`. Portable kernels — those that touch only
`forge.*` — are emitted for every requested arch. The compiler keeps
both kinds in the same translation unit without complaint; the linker
drops emit-disabled kernels per target.

Runtime selection is automatic:

```cajeta
let device = gpu.devices().best();         // picks NV or AMD, whichever is present
let stream = gpu.Stream.on(device);
my_portable_kernel.launch(stream, ...)(...);   // works on either
my_nv_only_kernel.launch(stream, ...)(...);    // typed error if device is statically AMD;
                                               // WrongVendorError at launch time otherwise
```

A row in the capability matrix (§4) supported on both vendors maps to
a single API in `gpu.forge.*` whose lowering picks the right native
intrinsic per target — even when the underlying instructions differ
(e.g. `shfl.sync` on NV vs `ds_bpermute` / `permlane` on AMD). A row
supported on only one vendor lives in that vendor's namespace.

This shape draws from HIP and SYCL, both of which keep a portable core
and expose vendor advances through parallel namespaces rather than
hiding them behind a lowest-common-denominator API.

### 5.3 Code generation pipeline

```
Cajeta MIR
   |
   v   (per-target lowering pass)
LLVM IR with:
  - target-specific address spaces
  - calling conv `ptx_kernel` or `amdgpu_kernel`
  - target intrinsics for thread IDs, barriers, wave ops, MMA
   |
   v   (llc)
NVPTX:  .ptx text  -> ptxas -> .cubin (per SM arch)
AMDGPU: .o ELF code object (v5)
   |
   v
Fatbin: all arch variants in one container, embedded into the host .so
        or written to a sidecar .cajeta-cubin / .cajeta-hsaco file.
```

`-march=` selects compute capability (`sm_70`, `sm_80`, `sm_89`,
`sm_90`, `sm_100`, ...) or gfx target (`gfx906`, `gfx90a`, `gfx940`,
`gfx1100`, `gfx1201`, ...). Multiple `-march` flags produce a fatbin.

### 5.4 Intrinsic mapping (selected)

NVPTX:

| Cajeta builtin                  | NVPTX intrinsic                              |
|---------------------------------|----------------------------------------------|
| `gpu.thread.x()`                | `llvm.nvvm.read.ptx.sreg.tid.x`              |
| `gpu.workgroup.x()`             | `llvm.nvvm.read.ptx.sreg.ctaid.x`            |
| `gpu.barrier.workgroup()`       | `llvm.nvvm.barrier.sync(0)`                  |
| `gpu.wave.shuffle.sync(...)`    | `llvm.nvvm.shfl.sync.*`                      |
| `gpu.wave.ballot.sync(...)`     | `llvm.nvvm.vote.sync.ballot`                 |
| `gpu.tensor.mma::<M,N,K,..>(..)`| `llvm.nvvm.mma.m16n16k16.row.col.f16.f16` etc. |
| `gpu.async.copy(..)`            | `llvm.nvvm.cp.async.*` / TMA on SM90+        |

AMDGPU:

| Cajeta builtin                  | AMDGPU intrinsic                             |
|---------------------------------|----------------------------------------------|
| `gpu.thread.x()`                | `llvm.amdgcn.workitem.id.x`                  |
| `gpu.workgroup.x()`             | `llvm.amdgcn.workgroup.id.x`                 |
| `gpu.barrier.workgroup()`       | `llvm.amdgcn.s.barrier`                      |
| `gpu.wave.shuffle.sync(...)`    | `llvm.amdgcn.ds.bpermute` / `permlane*`      |
| `gpu.wave.ballot.sync(...)`     | `llvm.amdgcn.ballot`                         |
| `gpu.tensor.mma::<M,N,K,..>(..)`| `llvm.amdgcn.mfma.f32.16x16x16f16` (CDNA) or `llvm.amdgcn.wmma.f16.16x16x16.f16` (RDNA3+) |
| `gpu.async.copy(..)`            | `llvm.amdgcn.global.load.lds` / direct-to-LDS |

Where the two architectures express the same operation differently
(e.g. CDNA `mfma` vs RDNA `wmma` tile shapes), the Cajeta builtin is
overloaded by tile shape; the lowering pass picks the matching native
intrinsic, and rejects with a clear diagnostic if no native form
exists.

### 5.5 Tensor core surface

The `gpu.tensor` module exposes cooperative matrix ops in two layers:

```cajeta
// Layer 1: typed fragments + load/store/mma — closest to native
struct Fragment<Kind, Shape, T> { ... }   // Kind = A | B | Accumulator
gpu.tensor.load_a<16, 16, 16, f16>(addr, stride) -> Fragment<A, ..., f16>
gpu.tensor.store_c<16, 16, 16, f32>(addr, frag, stride)
gpu.tensor.mma<M, N, K, AT, BT, CT>(a: Frag, b: Frag, c: Frag) -> Frag

// Layer 2: tiled GEMM building block
gpu.tensor.gemm_tile<M, N, K, AT, BT, CT>(...)
```

Shape validity is checked at compile time against the target's
capability traits. A request for `mma<16, 16, 16, f16, f16, f32>` on a
device whose only mma shape is `16x16x4` is a compile error with a
suggested alternative.

### 5.6 Host runtime — `libcajeta-forge.so`

One library. The CUDA driver (`libcuda.so.1`) and HIP runtime
(`libamdhip64.so`) are both `dlopen`'d on first use; neither is a
link-time dependency of the user binary, and neither is required for
the process to start. If only one is present, only that device family
appears in `gpu.devices()`. If neither is present, Forge runs in
host-emulation mode (slow, intended for unit-test parity).

`Stream`, `Event`, `Buffer`, `Module`, `Function` in `gpu.forge.*`
are backend-tagged at construction. Operations on them dispatch
through a small vtable in `libcajeta-forge.so` to either the CUDA or
HIP implementation. The cost is one indirect call per operation —
negligible against kernel launch overhead, and inlined away when the
device family is statically known (the common case in tight loops).

The vendor-specific modules `gpu.forge.nv.*` and `gpu.forge.amd.*`
bypass the vtable and bind directly to driver entry points. Code that
wants raw CUDA semantics imports `gpu.forge.nv` and gets them; code
that wants raw HIP semantics imports `gpu.forge.amd`. The portable
surface in `gpu.forge.*` exists for code that should run on either.

### 5.7 Vendor library FFI

Vendor BLAS / DNN / FFT / collective libraries live in their vendor
namespace:

```cajeta
import cajeta.gpu.forge.nv.cublas;    // cuBLAS
import cajeta.gpu.forge.nv.cudnn;     // cuDNN
import cajeta.gpu.forge.nv.cufft;     // cuFFT
import cajeta.gpu.forge.nv.nccl;      // NCCL collectives
import cajeta.gpu.forge.amd.rocblas;  // rocBLAS
import cajeta.gpu.forge.amd.miopen;   // MIOpen
import cajeta.gpu.forge.amd.rocfft;   // rocFFT
import cajeta.gpu.forge.amd.rccl;     // RCCL collectives
```

These are thin bindings, not portability shims. A small set of
"obviously portable" wrappers lives in `cajeta.gpu.forge.blas` and
`cajeta.gpu.forge.dnn` and dispatches at runtime to whichever vendor
lib is loaded; `Prism` / `Torch` build on those.

### 5.8 Graph capture

```cajeta
let g = stream.begin_capture();
foo.launch(stream, ...)(...);
bar.launch(stream, ...)(...);
let exec = g.end_capture();
exec.launch(stream);              // launches the whole graph
```

Forge exposes CUDA graphs and HIP graphs through one API; Shade has no
equivalent (see §6.5 for the Vulkan analog).

---

## 6. Backend B — Shade (`cajeta.gpu.shade`)

### 6.1 When to use Shade

- A game or interactive app that needs graphics + compute in one
  pipeline.
- A piece of software that must run on NVIDIA, AMD, Intel Arc, Apple
  (via MoltenVK), Android, and SteamDeck without per-vendor binaries.
- Cross-vendor ML inference where 80% of native perf is acceptable in
  exchange for a single shipping binary.
- Anything where the Vulkan ecosystem (RenderDoc, NSight, Radeon GPU
  Profiler) is the debugging surface.

### 6.2 Code generation pipeline

```
Cajeta MIR
   |
   v   (Shade-specific lowering)
LLVM IR with:
  - SPIR-V address spaces (Function / Workgroup / Uniform / StorageBuffer / Image / ...)
  - explicit descriptor-set / binding decorations
  - subgroup / cooperative-matrix intrinsics
   |
   v   (LLVM SPIR-V backend, mtriple=spirv-unknown-vulkan)
.spv  (one file per shader stage / one per kernel)
   |
   v   (spirv-opt / spirv-val in --gpu-debug)
final .spv
   |
   v
Embedded into the binary's resources, or written to a .cajeta-spv
sidecar (mmap-loaded by the Shade runtime).
```

When the in-tree SPIR-V backend cannot emit a needed extension
opcode (rare but happens for newly-published Khronos extensions), Shade
falls back to the Khronos `SPIRV-LLVM-Translator` for that module. The
choice is per-translation-unit and recorded in build metadata.

### 6.3 Pipeline kinds

```cajeta
@compute                             // compute pipeline, like @kernel
@vertex                              // vertex shader
@fragment                            // fragment shader
@geometry                            // geometry shader  (legacy)
@tess_control / @tess_eval           // tessellation
@mesh / @task                        // mesh shader pipeline
@ray_gen / @miss / @closest_hit / @any_hit / @intersection / @callable
```

Each attribute corresponds to a SPIR-V execution model. `@kernel` is
an alias for `@compute` for source compatibility with Forge code.

A graphics pipeline is declared as a struct of stages:

```cajeta
class TerrainPipeline : GraphicsPipeline {
    @vertex   fn vs(...) -> VsOut { ... }
    @fragment fn fs(in: VsOut) -> FragOut { ... }

    @descriptor(set=0, binding=0) var height_map: Texture2D<f16>;
    @descriptor(set=0, binding=1) var sampler:    Sampler;
    @push_constant struct PC { camera: Mat4; lod: f32; }
}
```

The compiler emits one SPIR-V module per stage, plus a
`VkPipelineLayout` description in metadata.

### 6.4 Subgroup / cooperative matrix surface

`gpu.wave.*` lowers to `GroupNonUniform*` SPIR-V opcodes under
`SPV_KHR_shader_subgroup`. Where SPIR-V exposes things CUDA / HIP do
not (e.g. `GroupNonUniformQuadBroadcast` for shader quads), those are
in `cajeta.gpu.shade.wave` only — they are *not* in the shared
`cajeta.gpu.core` surface.

`gpu.tensor.*` lowers to `OpCooperativeMatrix*KHR` under
`SPV_KHR_cooperative_matrix`. The fragment shape is queried from
`vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` at pipeline
create. The same shape-validity check from §5.4 applies, but it runs
at pipeline-create time, not compile time, because the supported
shapes are not knowable from the SPIR-V binary alone.

### 6.5 Host runtime — `libcajeta-shade.so`

Links the Vulkan loader (`libvulkan.so.1`) and ships these objects:

- `gpu.shade.Instance`, `gpu.shade.Device` — wraps `VkInstance` /
  `VkDevice`. Users may bring their own — Cajeta does not require it
  to create the device, only to register one for kernel dispatch.
- `gpu.shade.Pipeline`, `gpu.shade.PipelineLayout`, `gpu.shade.DescriptorSet`
  — typed handles.
- `gpu.shade.CommandBuffer` — wraps `VkCommandBuffer`, exposes
  `record { ... }` blocks.
- `gpu.shade.Swapchain`, `gpu.shade.Surface` — windowed presentation.
- `gpu.shade.RenderGraph` — opt-in declarative graph that records into
  secondary command buffers; the Shade equivalent of CUDA graphs (§5.7).

A user that already has a Vulkan engine and just wants to dispatch
Cajeta kernels into it gets the integration path:

```cajeta
let device = gpu.shade.Device.from_handle(vkDevice, queueFamilyIndex);
let pipe   = my_kernel.compile_for(device);
cmd.bind(pipe).dispatch(grid);
```

No competing instance, no extra queue, no duplicated descriptor pool.

### 6.6 Hot reload

Because SPIR-V is data, not code, Shade ships a `gpu.shade.Reloader`
that watches `.cajeta-spv` sidecar files on disk, rebuilds the
pipeline on change, and atomically swaps it on the next frame. This is
the loop game devs expect; it does not exist on Forge.

---

## 7. Memory and allocator story

Each backend owns its allocator. Buffers do not migrate between
backends; a buffer created from Forge cannot be bound to a Shade
pipeline and vice versa.

| Path                  | Allocator                                            |
|-----------------------|------------------------------------------------------|
| Forge / CUDA          | `cuMemAlloc` / `cuMemAllocAsync` (stream-ordered)    |
| Forge / HIP           | `hipMalloc` / `hipMallocAsync`                       |
| Shade / Vulkan        | VMA (Vulkan Memory Allocator) wrapped as `gpu.shade.HeapPool` |

`Buffer<T>` is generic over a backend tag:

```cajeta
gpu.forge.Buffer<float>      // CUDA / HIP storage
gpu.shade.Buffer<float>      // VkBuffer + VkDeviceMemory
gpu.Buffer<float>            // alias for the active backend in a build
```

`gpu.Buffer<T>` is the type higher-level libraries (Prism, Torch,
cajeta.ml) write against. The build selects which backend it resolves
to. Code that *must* address both backends in the same translation
unit imports the qualified names.

Interop between buffers from different backends, when explicitly
requested, goes through one of:

- **Host staging.** Always works. Cost: one round-trip to host memory.
- **External memory.** `VK_KHR_external_memory_fd` + `cuImportExternalMemory`
  / `hipImportExternalMemory`. Zero-copy when both backends are
  attached to the same device.
- **DMABUF on Linux.** Same idea, file-descriptor-based; works for
  NVIDIA (open-kernel driver), AMD, and Intel.

Interop is in `cajeta.gpu.interop`, an optional crate that links both
runtimes. Plain `cajeta.gpu.forge` and `cajeta.gpu.shade` consumers
never pay for it.

---

## 8. Overlap — what is genuinely shared

There are two tiers of portability in Cajeta's GPU stack:

1. **Cross-backend** (`cajeta.gpu.core`): things that mean the same on
   both Forge and Shade. Intentionally narrow.
2. **Cross-vendor within Forge** (`cajeta.gpu.forge.*`): things that
   mean the same on NVIDIA and AMD through Forge's native codegen
   path, but are not necessarily portable to Shade. Broader than the
   cross-backend layer.

Anything that crosses the cross-backend layer must mean the same
thing on every backend. The rest stays in backend-specific (or
vendor-specific) modules where the names cannot collide.

**Cross-backend shared (`cajeta.gpu.core`):**

- The `@kernel` / `@device` / `@host` attributes.
- The address-space-qualified reference types (`gpu.Global`,
  `gpu.Shared`, `gpu.Constant`, `gpu.Private`).
- The execution model vocabulary: Grid / Workgroup / Wave / Thread.
- The `gpu.thread.*`, `gpu.workgroup.*`, `gpu.barrier.*` builtins —
  the universal ones only.
- The `gpu.wave.{shuffle, ballot, any, all, reduce}` operations that
  every target supports.
- The `gpu.tensor.{load_a, load_b, store_c, mma}` operations, with
  shape validity gated by capability traits.
- `Buffer<T>` as an *interface*, with backend-specific concrete
  implementations.
- `Stream`, `Event`, `Fence` as interfaces.
- The launch syntax `f.launch(stream, grid, block)(args)`.
- The capability traits in `gpu.cap.*` and their compile-time check.

**Cross-vendor within Forge (`cajeta.gpu.forge.*`):**

Everything in `cajeta.gpu.core` *plus*:

- Raw device pointers and pointer arithmetic.
- `malloc` / `free` inside kernels.
- CUDA / HIP graph capture (`Stream.begin_capture()`) — uniform API,
  vendor-specific implementation.
- `printf` inside kernels.
- The portable `gpu.forge.blas` / `gpu.forge.dnn` wrappers.

**Vendor-specific within Forge:**

- `cajeta.gpu.forge.nv.*` — TMA, cluster barriers, FP8 E4M3/E5M2,
  NVSHMEM, cuBLAS / cuDNN / cuFFT / NCCL.
- `cajeta.gpu.forge.amd.*` — full MFMA shape menu, RDNA-specific
  permlane / dpp / DPP8 variants, rocBLAS / MIOpen / rocFFT / RCCL.

**Shade-specific (`cajeta.gpu.shade.*`):**

- Graphics pipeline stages, descriptor sets, push constants.
- Vulkan extensions: mesh shaders, ray tracing, descriptor indexing,
  shader objects, fragment shading rate.
- SPIR-V `RenderGraph`, hot-reload, swapchain / surface objects.

Code that uses only the cross-backend layer is portable across Forge
and Shade. Code that uses `cajeta.gpu.forge.*` is portable across NV
and AMD via Forge. Code that imports `cajeta.gpu.forge.nv.*`,
`cajeta.gpu.forge.amd.*`, or `cajeta.gpu.shade.*` has named what it
is asking for, and the compiler enforces the choice.

---

## 9. Coexistence in one process

Forge and Shade are designed to coexist *without* sharing state.

### 9.1 Library layout

```
libcajeta-runtime.so            shared, always present
libcajeta-gpu-core.so           shared GPU MIR / launch dispatch / capability runtime
libcajeta-forge.so              Forge runtime — both NV and AMD in one .so;
                                  dlopens libcuda.so.1 and/or libamdhip64.so
                                  on demand. No per-vendor sub-library.
libcajeta-shade.so              Shade runtime (depends on -core)
libcajeta-gpu-interop.so        optional, depends on -forge and -shade
```

A binary that uses only Forge does not link `libcajeta-shade.so` and
no Vulkan symbols enter the process. The reverse holds for a pure
Shade binary. A binary that uses both links both, and they share only
`libcajeta-gpu-core.so` — which has *no* driver dependencies.

### 9.2 Symbol namespace conflict avoidance

- All Forge symbols live in `cajeta::gpu::forge::` (C++ name mangling)
  and `cajeta.gpu.forge.*` (Cajeta module path).
- All Shade symbols live in `cajeta::gpu::shade::` / `cajeta.gpu.shade.*`.
- The shared core uses `cajeta::gpu::core::` / `cajeta.gpu.core.*`.
- Symbols from `libcuda.so` and `libvulkan.so` are loaded via
  `dlopen` with `RTLD_LOCAL | RTLD_NOW`. They are never re-exported
  by Cajeta libraries.

### 9.3 Device-context conflict avoidance

A real concern: NVIDIA's Vulkan driver and CUDA driver each create
their own context on the same physical GPU, and naive code can end up
with two contexts fighting for residency. Shade's `Device` object
exposes a flag:

```cajeta
let d = gpu.shade.Device.create({
    physical_device: 0,
    coexist_with_forge: true,         // default: false
});
```

When set, Shade will:

- Enable `VK_KHR_external_memory`, `VK_KHR_external_semaphore`, and
  `VK_KHR_timeline_semaphore` at device creation.
- Prefer the Vulkan queue family that supports compute + transfer
  without graphics (so it competes least with Forge's CUDA streams).
- Register the Vulkan device with `cajeta.gpu.interop` so that
  Forge buffers can be imported zero-copy when both target the same
  device UUID.

The default is `false` because most users want exactly one path.

### 9.4 Compile-time selection

```toml
# In a project's Cajeta.toml
[gpu]
backends = ["forge", "shade"]        # one or both
default  = "forge"                   # which one `cajeta.gpu.*` aliases resolve to
forge.targets = ["sm_80", "sm_90", "gfx90a", "gfx1100"]
shade.target  = "vulkan-1.3"
shade.extensions = ["cooperative_matrix", "buffer_device_address", "mesh_shader"]
```

A project that only ever wants Shade declares `backends = ["shade"]`
and the Forge codegen pass is not even instantiated by the compiler.
The build is shorter, the binary smaller, and the user cannot
accidentally call a Forge-only API.

### 9.5 The "I want both at once" scenario

A concrete example: a game engine that uses Shade for all rendering
and one neural-net inference per frame, but the dev team wants to
train the network on a workstation using Forge.

- Training binary: `backends = ["forge"]`, links `libcajeta-forge*`.
- Game binary: `backends = ["shade"]`, links `libcajeta-shade*`.
- Both compile the *same* `inference_kernel.cajeta` file. Forge emits
  PTX/HSACO for it; Shade emits SPIR-V. The kernel must stay in the
  shared subset (§8) — it uses `gpu.tensor.mma` and `gpu.wave.shuffle`
  only, not anything backend-specific.
- The shared weights file is a plain `.npy` / `.cajeta-weights` blob;
  it is not GPU-resident in either case.

If the same binary ever needs both (e.g. a content tool that profiles
inference under both runtimes), `backends = ["forge", "shade"]` plus
the §9.3 coexist flag covers it.

---

## 10. Toolchain integration

### 10.1 Compiler flags

```
--gpu-backend=forge|shade|both
--gpu-arch=<list>               # sm_80,sm_90,gfx90a,gfx1100,...
--gpu-vulkan=1.2|1.3
--gpu-extensions=<list>
--gpu-debug                     # bounds checks, spirv-val, ptxas -lineinfo
--gpu-emit=ir|asm|bin           # stop early for inspection
```

### 10.2 Build artifacts

- `*.cajeta` -> `*.cajeta-mir` (debug) -> `*.ll` (debug) -> backend output.
- Forge: `*.ptx` + `*.cubin` per arch, fatbinned into `*.cajeta-fatbin`.
- Shade: one `*.spv` per shader stage, manifested into `*.cajeta-spv-bundle`.
- Both bundle types embed into the host `.so`/`.exe` by default, with
  a `--emit-sidecar` flag to keep them out for asset-bundle workflows
  (typical for games).

### 10.3 Diagnostics

The existing `--diag-hints` "did you mean" pass extends with GPU-specific
suggestions:

- "`gpu.tensor.mma<16, 16, 16, f16, f16, f32>` is not available on
  `gfx906`; nearest available shapes: `<16, 16, 4>`, `<32, 32, 4>`."
- "`gpu.shade.RayQuery` requires `--gpu-extensions=ray_query` — add it
  to `Cajeta.toml` or this flag."
- "`@push_constant` is Shade-only; this kernel is being compiled for
  `--gpu-backend=both`. Move the struct to a uniform buffer or guard
  the kernel with `@backend(shade)`."

### 10.4 Debuggers and profilers

- Forge: NSight Compute / NSight Systems / Compute Sanitizer (NVIDIA);
  rocprof / rocprofv2 / Radeon GPU Profiler (AMD). Cajeta emits line
  tables matching the source `.cajeta` file.
- Shade: RenderDoc, NSight Graphics, Radeon GPU Profiler. SPIR-V
  carries `OpLine` mapping back to `.cajeta` source.

---

## 11. Borrow checker interaction (notable cases)

The borrow checker treats GPU launches as a deferred borrow with a
lifetime ending at the next sync. Three cases are worth calling out
because they bite users in CUDA-land and Cajeta should not let them
through:

1. **Buffer freed while launch in flight.**

   ```cajeta
   let buf = gpu.Buffer<float>.alloc(n);
   kernel.launch(stream, ...)(buf, ...);
   buf.free();                       // ERROR: live borrow until stream.sync()
   ```

2. **Shared memory aliasing.**

   ```cajeta
   @kernel
   void f(gpu.Shared<float[256]> a) {
       let r1 = &mut a[..128];
       let r2 = &mut a[..];          // ERROR: overlaps r1
   }
   ```

3. **Cross-stream WAR / RAW.**

   A buffer written by `kernel_a` on `stream_a` and read by `kernel_b`
   on `stream_b` requires an explicit `stream_b.wait(event_from_a)` or
   the borrow checker rejects the launch. Cajeta does *not* infer
   cross-stream dependencies — that is a runtime cost the user must
   opt into.

These are checked the same way on Forge and Shade.

---

## 12. Phasing

A reasonable order of implementation. Each phase is independently
useful; do not start a later phase until its predecessor has shipped a
working sample.

1. **`cajeta.gpu.core` MIR + capability traits.** No codegen yet,
   just types and the launch-site syntax that the borrow checker can
   reason about. Lower MIR to a CPU-emulation backend so kernels
   compile and run (slowly) on the host. Unblocks frontend work and
   tests.
2. **Forge / NVPTX, compute only.** SAXPY, GEMM (no tensor cores),
   reduction. Single arch. CUDA driver runtime. End-to-end build:
   `cajeta` -> `.cubin` -> launch.
3. **Forge / AMDGPU, compute only.** Same kernels, gfx90a or gfx1100.
   HIP runtime. Validate the shared layer truly is shared.
4. **Forge tensor cores.** WMMA on NVPTX, MFMA on CDNA, WMMA on
   RDNA3+. A real GEMM that beats the v2 GEMM.
5. **Shade / Vulkan compute.** Same compute kernels through SPIR-V.
   The first time portability is real.
6. **Shade graphics pipeline.** A textured triangle. Then a textured
   model. Then `cajeta.gpu.shade.RenderGraph`.
7. **Shade cooperative matrix.** GEMM through SPIR-V; compare to
   Forge GEMM perf on the same hardware.
8. **Interop crate.** External memory between Forge and Shade on
   Linux.
9. **Mesh / ray tracing in Shade.** Game-dev-grade.
10. **CUDA / HIP graph capture in Forge.** ML-training-grade.

---

## 13. Open questions

- **Should `@kernel` and `@compute` be the same attribute?** Pro: one
  vocabulary for the user. Con: a kernel written for Forge that does
  `gpu.kernel.malloc(...)` cannot trivially become a Shade compute
  shader. Current answer: yes, same attribute, with a
  `@backend(forge)` annotation when the body uses backend-specific
  features.
- **Lifetime of a `Stream` across backend boundaries?** Specifically,
  if `cajeta.gpu.interop` lets a `gpu.shade.Stream` wait on a
  `gpu.forge.Event` via a timeline semaphore, do we expose that as a
  unified `gpu.Stream` interface or keep them separate? Leaning
  separate — the unification has not earned its complexity yet.
- **MLIR as IR layer.** A larger question for Cajeta as a whole: would
  switching the mid-IR from raw LLVM IR to MLIR (with the `gpu`
  dialect at this layer) pay off enough to justify the dependency? It
  would simplify §5 and §6 considerably. Out of scope for this doc;
  flagged for a separate decision.
- **WebGPU / Dawn as a third backend.** Browser/WGSL is a different
  shape than either Forge or Shade. Not in v1; if added, it slots in
  as `cajeta.gpu.spark` (working name) under the same coexistence
  model — separate library, no symbol overlap.

---

## 14. Summary

Forge and Shade are two different products in the same shape. They
share the parts of Cajeta that *should* be one thing — the language
surface, the execution model vocabulary, the borrow-checker semantics
across launches — and they diverge wherever the underlying hardware
or runtime model actually diverges. They ship as separate libraries,
have non-colliding namespaces, and only share an allocator when the
user explicitly asks for interop. A Cajeta project picks one, the
other, or both, at build time, with no source changes required for
code that lives in the shared subset.
