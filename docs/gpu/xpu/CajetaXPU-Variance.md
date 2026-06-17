# CajetaXPU — Variance Discipline

This is the working register for where `cajeta.xpu.nvidia`,
`cajeta.xpu.amd`, and `cajeta.xpu.vulkan` diverge — and the discipline
that keeps those divergences from leaking into `cajeta.gpu` as
NVIDIA-shaped assumptions during the NVIDIA-first implementation
phase.

The rule statement and pointer back to this doc live in
[`CajetaXPU.md`](CajetaXPU.md) §3.0.

> **A fourth backend — CPU — has since landed on the same seam** (the
> `LoweringTarget` vtable; see [`CajetaCPU.md`](CajetaCPU.md)). It held the
> three-column discipline with a single new fork: the **coordinate source**. A
> CPU has no hardware grid or coordinate intrinsics, so its kernel gains 9
> trailing `i32` coordinate params and `GpuThread`/`Workgroup` reads pull from those
> (the grid→threads model); buffers are flat `addrspace(0)`, the wave is width-1,
> barriers are deferred (`XPU-N01`). Everything else — the body walk, operators,
> control flow, `globalId = ctaid*ntid + tid` — stayed Core, confirming the
> measured surface a third time. The *launch* side added one runtime fork
> (Vulkan's descriptor-set ABI needs per-kernel parameter metadata to translate
> the uniform `kernelParams` argv), but that lives in the runtime dispatcher, not
> on `LoweringTarget`.

---

## 1. The three-column check

Every candidate `xpu.core` API passes a three-column check before
landing:

- Does this work on **NVIDIA**? (the implementation that actually
  exists)
- Does this work on **AMD** without invalidating the API shape?
  (mental model)
- Does this work on **Vulkan** without invalidating the API shape?
  (mental model)

### 1.1 Outcomes

- **All three pass** — lands in `xpu.core` as designed.
- **One column needs different parameters or semantics** —
  restructured so the variation becomes a parameter. Example: wave
  width is const-expr *per target*, not a literal `32`.
- **One column genuinely can't support it** — moves to the vendor
  namespace. Portable wrappers, if any, live in `xpu.core.compat`.

---

## 2. Axes of variance

Each row is a dimension where the three backends diverge. Every new
`xpu.core` API gets reviewed against the relevant rows. New
dimensions get appended as they surface during NVIDIA-first work; do
not reorder existing rows — they are referenced by number in PR
variance checks.

| #  | Dimension                  | NVIDIA                            | AMD                                  | Vulkan                                                | Implication for core                                                                          |
|----|----------------------------|-----------------------------------|--------------------------------------|-------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| 1  | Wave / subgroup width      | 32 const                          | CDNA 64; RDNA 32 or 64               | runtime query at pipeline-create                      | `xpu.wave.width()` is const-expr *per target*, never a literal                                |
| 2  | Launch arg model           | direct pass                       | direct pass                          | push-constant + descriptor partition, bound at record | `launch(...)` needs a backend-aware binding pass; "just pass the args" is an NV mental model  |
| 3  | Allocator                  | `cuMemAllocAsync` stream-ordered  | `hipMallocAsync` stream-ordered      | VMA, no stream-ordered equivalent                     | `alloc_async(stream)` either degrades or is vendor-only                                       |
| 4  | Atomic scopes              | GpuThread / Block / Device / System  | workgroup / agent / system           | Invocation / Subgroup / Workgroup / QueueFamily / Device | use Vulkan's set in core — it is the superset                                              |
| 5  | Memory model               | PTX scoped atomics                | HSA                                  | Vulkan memory model (superset)                        | spec barriers / fences against Vulkan semantics                                               |
| 6  | Sync primitives            | stream + event                    | stream + event                       | timeline semaphore + descriptor-pool tie-ins          | `GpuStream` / `Event` / `Fence` must round-trip through Vulkan without a leaky case              |
| 7  | Raw device pointers        | default                           | default                              | needs `KHR_buffer_device_address`                     | `&GpuBuffer<T>` → raw ptr decay is a capability, not a default                                   |
| 8  | Kernel-side malloc         | yes                               | yes                                  | no                                                    | vendor-only by definition                                                                     |
| 9  | printf in kernel           | yes                               | yes                                  | `KHR_non_semantic_info`                               | portable but extension-gated                                                                  |
| 10 | Graph capture              | CUDA graphs                       | HIP graphs                           | secondary command buffers (different shape)           | likely vendor-only, not core                                                                  |
| 11 | Per-arch codegen           | fatbin of SM archs                | bundle of gfx archs                  | one SPIR-V                                            | `--xpu-arch` is multi-valued for native backends only                                         |
| 12 | Borrow-check on launch     | deferred borrow until `GpuStream.sync()` | same                             | same                                                  | no friction — already aligns on all three                                                     |

---

## 3. Per-PR process

For every PR that adds or modifies an `xpu.core` API:

1. The PR description includes a **variance check** section — which
   rows from §2 apply, and the AMD / Vulkan mental-model answer for
   each.
2. If any row says "would force a redesign," restructure *before*
   merge — don't ship the NV-only shape and refactor across already-
   written kernels later.
3. New variance dimensions discovered during the check get appended
   to §2 in the same PR.

The variance check is a paragraph in the PR description, not a
separate review pass.

---

## 4. Hard pre-AMD checkpoints

Before the AMD pass begins, the following `xpu.core` surfaces must
already clear the three-column check — because they are the language-
surface bits where retrofitting across already-shipped NVIDIA kernels
would be most expensive.

- **`@Kernel` attribute + the `KernelArg` trait.** Must admit
  Vulkan's push-constant / descriptor partitioning, not just "pass
  the args."
- **Address-space qualifiers** (`Global` / `Shared` / `Constant` /
  `Private` / `Generic`). Must map cleanly to SPIR-V storage classes,
  not just NV addrspace numbers.
- **`launch(stream, grid, block)(args)` syntax.** The Vulkan binding
  pass is the hard part — either the syntax accommodates it or we
  end up with two launch syntaxes, which we don't want.
- **`GpuBuffer<T>` lifecycle** (alloc / free / upload / download / map).
  Must round-trip through Vulkan's bind-memory-to-buffer model
  without a leaky special case.
- **`GpuStream` / `Event` / `Fence`.** Must round-trip through Vulkan
  timeline semaphores.
- **`xpu.barrier.workgroup()` / `xpu.barrier.wave()`.** Must lower to
  `OpControlBarrier` with the correct scope.
- **Borrow-check rules** for launch borrow scope, shared-memory
  aliasing, and cross-stream WAR / RAW.

Lower-risk surfaces — capability traits, the `xpu.thread.x` family of
intrinsic builtins, the `xpu.core.{blas,dnn,fft}` portable wrappers —
can be more iterative. They are easier to move between namespaces
post-AMD without invalidating kernels.

---

## 5. Maintaining this doc

- Append new rows to §2 as the NVIDIA implementation surfaces new
  divergences. Do not reorder existing rows; PR variance checks
  reference them by number.
- When a row's implication changes — e.g. the AMD pass discovers we
  were wrong about how something maps — update the row in place and
  note the reason in the PR that changes it.
- Retire a row only when the dimension truly stops mattering across
  all three backends. For example, if Vulkan eventually gains stream-
  ordered allocation, row 3 can be removed.
