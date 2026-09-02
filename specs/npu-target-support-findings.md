# NPU target support — research findings

Companion artifact to `npu-target-support-spec.md`. This file records **what was
measured**, so the spec can state requirements without re-litigating evidence.

- **Author:** session julian-4d, 2026-09-01
- **Status:** RESEARCH COMPLETE — all four tracks reported. §1-§6 measured on this machine;
  §7 file:line recon; §8-§10 primary-source research.
  Nothing in §1–§6 is inferred; every claim was run on
  `proton` (AMD Ryzen AI Max+ 395, Strix Halo) against kernel `7.0.0-30-generic`.
- **Reproduce:** `specs/schemas/npu-probe.py`

---

## 1. Why this matters

The requirement is a runtime that **identifies and adapts** to NPUs generally — not a
Strix Halo special case. The load-bearing question is whether the OS gives us a
vendor-neutral way in. It does, partially, and the shape of that partial answer
determines the whole design.

## 2. The portable entry point (MEASURED)

**`DRM_IOCTL_VERSION` works on `/dev/accel/*` and needs no vendor userspace.**

This is the finding the detection layer is built on. No XRT, no Ryzen AI stack, no
vendor SDK, no root — the calling user needs only `render` group membership (or an
ACL entry, which is what this machine has).

```
/dev/accel/accel0  → name='amdxdna_accel_driver' v0.7.0  desc='AMD XDNA DRM implementation'
/dev/dri/renderD128 → name='amdgpu'              v3.64.0  desc='AMD GPU'
```

The same generic ioctl (`_IOWR('d', 0x00, struct drm_version)`) identifies both an NPU
and a GPU. That gives us a uniform first step: enumerate `/dev/accel/*`, call
`DRM_IOCTL_VERSION`, and dispatch on the returned driver name.

**Consequence for the spec:** device *identification* is portable and cheap. Device
*characterization* is not (§4).

## 3. The accel driver family inventory (MEASURED)

`drivers/accel` in this kernel ships modules for four families, and the distribution
ships uAPI headers for six:

| Driver | Hardware | Module present | uAPI header |
|---|---|---|---|
| `amdxdna` | AMD XDNA / XDNA 2 (Ryzen AI) | yes | yes |
| `ivpu` | Intel NPU (Meteor Lake onward) | yes | yes |
| `qaic` | Qualcomm Cloud AI 100 | yes | yes |
| `habanalabs` | Habana Gaudi | yes | yes |
| `ethosu` | ARM Ethos-U | — | yes |
| `rocket` | Rockchip NPU | — | yes |

Six families behind one subsystem is what makes "identify and adapt" tractable rather
than aspirational. It is also the honest ceiling: anything not in `drivers/accel`
(Apple ANE, Qualcomm Hexagon on Windows, most Android NPUs) is outside this path
entirely and needs a separate provider.

## 4. Commonality and divergence across drivers (MEASURED)

Every accel driver has the **same conceptual shape** — allocate a buffer, submit a
command, wait for completion, maintain caches — but shares **no ioctl beyond
`DRM_IOCTL_VERSION`**.

| Concept | `amdxdna` | `ivpu` | `qaic` | `rocket` | `habanalabs` |
|---|---|---|---|---|---|
| Allocate | `CREATE_BO` | `BO_CREATE` | `CREATE_BO` | `CREATE_BO` | `MEMORY` |
| Submit | `EXEC_CMD` | `SUBMIT`, `CMDQ_SUBMIT` | `EXECUTE_BO` | `SUBMIT` | `CS` |
| Wait | syncobj + `seq` | `BO_WAIT` | `WAIT_BO` | implicit | `WAIT_CS` |
| Cache maint. | `SYNC_BO` | — | — | `PREP_BO` / `FINI_BO` | — |
| Capability | `GET_INFO` | `GET_PARAM` | `MANAGE` | — | `INFO` |

Two observations that shape the design:

1. **There is no generic capability ioctl.** Each driver invents its own vocabulary —
   `amdxdna` answers `QUERY_AIE_METADATA`, `ivpu` answers `DRM_IVPU_PARAM_TILE_CONFIG`
   and `DRM_IVPU_PARAM_CAPABILITIES`, `ethosu` answers `ETHOSU_DEV_QUERY_NPU_INFO`.
   Portability must come from **cajeta's interface**, not from the kernel. Each driver
   needs a hand-written capability provider behind a common trait.
2. **Explicit cache maintenance is the norm, not a Strix quirk.** Both `amdxdna`
   (`SYNC_BO`, with `SYNC_DIRECT_TO_DEVICE` / `SYNC_DIRECT_FROM_DEVICE`) and `rocket`
   (`PREP_BO` / `FINI_BO`) expose it. The runtime's memory model must assume
   non-coherent DMA and place explicit sync points at handoff boundaries.

Also noted: `ivpu` has `BO_CREATE_FROM_USERPTR`, a zero-copy path with no `amdxdna`
equivalent. Capability negotiation has to cover *import strategy*, not just dtypes.

## 5. Full device characterization without vendor userspace (MEASURED)

`specs/schemas/npu-probe.py` opens the device, identifies the driver generically, then
issues `DRM_IOCTL_AMDXDNA_GET_INFO` with `DRM_AMDXDNA_QUERY_AIE_METADATA`. Output on
this machine:

```
driver      : amdxdna_accel_driver  v0.7.0
description : AMD XDNA DRM implementation
AIE version : 1.1
array       : 8 cols x 6 rows, col_size=504 B
  core  rows=4 start=2  dma_ch=2  locks=16  events=4
  mem   rows=1 start=1  dma_ch=6  locks=64  events=6
  shim  rows=1 start=0  dma_ch=2  locks=16  events=4
```

Derived topology: **32 core (compute) tiles**, **8 mem tiles carrying 48 DMA
channels**, **8 shim tiles carrying 16 DMA channels to DDR**.

This is the single most important result for the DMA requirement. The DMA channel
topology — the resource a spatial dataflow mapping must schedule against — is
**directly enumerable at runtime with no vendor stack**. A cajeta backend can query
the fabric it is about to target rather than hard-coding a device model.

## 6. DMA and cross-engine synchronization (MEASURED)

Symbol inspection of `amdxdna.ko` shows dma-buf support in **both** directions plus
timeline fencing:

```
import  : dma_buf_get, dma_buf_attach, dma_buf_map_attachment_unlocked,
          drm_gem_shmem_prime_import_sg_table
export  : dma_buf_export, drm_gem_map_dma_buf, drm_gem_shmem_get_sg_table
fencing : dma_resv_add_fence, dma_resv_reserve_fences, dma_resv_wait_timeout,
          dma_fence_chain_find_seqno, dma_fence_chain_ops
```

`dma_fence_chain_find_seqno` is specifically the **DRM timeline syncobj** mechanism,
and the uAPI corroborates it: `CREATE_HWCTX` returns a `syncobj_handle`, `EXEC_CMD`
returns a `seq`, and command types are `SUBMIT_EXEC_BUF` / `SUBMIT_DEPENDENCY` /
`SUBMIT_SIGNAL`.

**CORRECTED 2026-09-01 — the uAPI header declares more than the running kernel
implements.** An earlier draft of this document concluded from the header that a
`GPU → NPU → GPU` fence chain needs no CPU round-trip. That is **false on the kernel
installed here**. `amdxdna_ctx.c:591-605` (v7.0) handles only `AMDXDNA_CMD_SUBMIT_EXEC_BUF`
and returns `-EINVAL` for `SUBMIT_DEPENDENCY` and `SUBMIT_SIGNAL`:

```c
	switch (args->type) {
	case AMDXDNA_CMD_SUBMIT_EXEC_BUF:
		return amdxdna_drm_submit_execbuf(client, args);
	}
	return -EINVAL;
```

Explicit dependency/signal support exists **only in AMD's out-of-tree driver**
(`amd/xdna-driver`), where `submit_dependency` resolves DRM syncobj handles + timeline
points via `drm_syncobj_find_fence()`. XRT surfaces it as `xrt::fence`.

**Lesson for the spec: never infer capability from a uAPI header.** The header ships the
union of what the ABI reserves; the loaded module decides what works. Capability
detection must probe by attempting the operation (§8.4).

**What IS true on this kernel:** `amdxdna` advertises `DRIVER_SYNCOBJ | DRIVER_SYNCOBJ_TIMELINE`
(`amdxdna_pci_drv.c:229-244`) and publishes its completion fence with
`DMA_RESV_USAGE_WRITE` on every BO (`aie2_ctx.c:1020-1035`), so **amdgpu will implicitly
wait for the NPU**. But amdxdna never calls `drm_sched_job_add_implicit_dependencies()`
on submit, so **the NPU will NOT wait for the GPU**. Ordering is free in one direction
and yours to enforce in the other.

**Counter-evidence that bounds the claim:** the existence of `SYNC_BO` means coherency
is *not* automatic. What dma-buf buys is **zero-copy handoff with explicit sync**, not
a live buffer two engines read and write concurrently. The spec must not promise the
latter.

## 7. Cajeta's current state (RECON, file:line verified)

Track 4 complete. Every claim carries a citation into the tree; the recon corrected two
of its own earlier conclusions, which are noted as corrections rather than silently fixed.

### 7.1 There are TWO driver stacks, and the shipping one is C

The C++ drivers under `src/cajeta/xpu/{nvidia,amd,vulkan,cpu}/*Driver.cpp` are
**compiler/test-only and never linked into a user program**
(`runtime/native/cajeta_xpu_dispatch.c:232-236`, echoed at `src/cajeta/xpu/cpu/CpuDriver.h:8-11`).
The runtime that actually ships is the **C** code in `runtime/native/cajeta_xpu_*.c`
(9,865 lines), compiled to LLVM bitcode at compiler-build time and linker-merged into
every user module (`src/CMakeLists.txt:661-683`).

**A new backend is therefore a doubled implementation** — a C++ side for compiler and
tests, plus a C side that ships as embedded bitcode. This must be in the plan's effort
budget from unit 1.

There is **no link-time GPU dependency anywhere**: no `find_package(Vulkan|CUDA|HIP)`,
no `target_link_libraries(... vulkan|cuda|amdhip)`. Vulkan is a compile-time
`__has_include(<vulkan/vulkan.h>)` gate (`cajeta_xpu_vulkan.c:10-14`); absent headers
compile the whole path to stubs (`:3683-3685`) — which once silently skipped a CI lane
(`src/cajeta/xpu/vulkan/VulkanDriver.h:44-51`). **An NPU backend must adopt the same
soft-dependency discipline**, and must fail loudly rather than stub silently.

### 7.2 DMA already has a designed shape — and zero implementation

`docs/specification/xpu/CajetaXPU.md:852-866` specs cross-backend interop with exactly
three transports: **host staging** (always works, one round-trip), **external memory**
(`VK_KHR_external_memory_fd` + `cuImportExternalMemory`/`hipImportExternalMemory`), and
**DMABUF on Linux** — "works for NVIDIA (open-kernel driver), AMD, and Intel". It is
scoped to `cajeta.xpu.interop`, an optional sub-module shipping as
`libcajeta-xpu-interop.so` so "plain single-backend consumers don't pay for it"
(`:875-885`). §9.2 (`:900-925`) adds a `coexist_with_native` device flag enabling
`VK_KHR_external_memory` / `_external_semaphore` / `_timeline_semaphore`.

**None of it exists** — zero hits for `xpu.interop`, `xpu::interop`, `importExternal`.
Likewise `KernelBuffer.alloc_async` (`:844-850`) is documented and unimplemented.

So the DMA requirement has a designed shape to **adopt or supersede** rather than
invent, and the spec must state its relationship to two named unbuilt siblings.

### 7.3 Existing portable detection is thinner than the docs claim

`cajeta_xpu_query_raw_device` — the entry point feeding the whole `DeviceProfile`
machine model — is **HIP-only** (`runtime/native/cajeta_xpu_driver.c:776`): it hard-calls
`cajeta_xpu_hip_init_locked()` and returns 0 if HIP isn't up; attribute ordinal `10002`
(`:799`) is AMD-specific. Both `runtime/native/cajeta_xpu_abi.h:98` and
`src/cajeta/xpu/core/DeviceProfile.h:25-26` claim it reads "hipDeviceProp/**cudaDeviceProp**"
— **it does not**. On a non-AMD box `cajeta gpu-profile` silently returns an estimated
gfx1151-shaped default (`DeviceProfile.h:47-60`).

**The template to copy is Vulkan, not `DeviceProfile`.** Real feature negotiation lives
at `cajeta_xpu_vulkan.c:348-720`: `vkGetPhysicalDeviceFeatures2` with `pNext` chains
enabling only advertised bits — ray-query quad as an all-four-or-none group (`:373-457`),
`VK_EXT_shader_atomic_float` (`:403-422`, NVIDIA **faults with `VK_ERROR_DEVICE_LOST`**
without it), `shaderInt8/16/64` (`:469-496`, RADV **crashes** rather than rejecting),
subgroup size control reading `min/maxSubgroupSize` (`:497-524`). Queue selection prefers
a family with `timestampValidBits != 0` (`:321-344`) because 3 of 5 families on the
reference device report 0. That negotiate-then-enable discipline is what §4's
per-driver capability providers should imitate.

### 7.4 The sharpest constraint: `LoweringTarget` cannot opt out of the wave model

`CajetaXPU.md:1088-1097` already anticipates NPUs: they share the
`KernelBuffer`/`KernelStream`/`Event` shape but "diverge sharply on the kernel surface
(most NPUs don't have wave-tiered execution — they're VLIW or systolic)" and would
"opt out of the wave model."

**`LoweringTarget` has no mechanism to opt out.** `waveWidth`, `waveShuffle`,
`quadSwap`, `workgroupBarrier` sit on the vtable with GPU-shaped defaults, and the CPU
backend **fakes** them rather than escaping them. This is the single largest piece of
existing-code surgery the work implies, and it is a prerequisite, not a nicety.

### 7.5 `cajeta-llvm` fork state

- **LLVM 23.0.0git — trunk, not a release branch** (`cmake/Modules/LLVMVersion.cmake:4-13`).
- **HEAD is on `cajeta-spirv`, not `main`**; `main` is a pure upstream mirror, 0 ahead.
- Fork is **42 ahead / 2597 behind** `upstream/main`; last upstream fetch ~2026-06-21.
  History fully linear, zero merge commits. Total delta 40 files, +1895/−14.
- **`llc --version` target list: `amdgcn, nvptx, nvptx64, r600, spirv, spirv32, spirv64,
  x86, x86-64`. Nothing else — there is no AIE/NPU backend in this fork.**
- **Zero local changes** to AMDGPU, NVPTX, MLIR, `offload/`, clang, flang, lld, lldb.
  The only non-SPIR-V divergence is JITLink/COFF x86-64 (Windows host JIT).
- Fork-registered extension `SPV_KHR_quad_control` is new (`SPIRVSymbolicOperands.td:351`,
  capability `QuadControlKHR = 5087`); `SPIRVFixupMergePlacement.cpp` is 86 lines,
  entirely fork-local.

Three hygiene defects surfaced that are **not** NPU work but will bite anyone touching
this fork:

1. **Release tag `cajeta-llvm-23-r6` was cut off the upstream mirror branch**
   (`ce465594e239`), which is not an ancestor of `cajeta-spirv` — **that release artifact
   contains none of the fork's SPIR-V work.**
2. **Doc contradicts code**: `UPSTREAM-PRS.md` says the cooperative-matrix Vulkan
   memory-model derivation is "capability-derived (not a name scan)"; on HEAD
   `SPIRVModuleAnalysis.cpp:158-177` is literally
   `starts_with("llvm.spv.cooperative.matrix.")`. The clean version exists only on the
   unmerged branch `pr/spirv-coopmatrix-vulkan-memory-model` (`93920336ab8e`).
3. **`UPSTREAM-PRS.md` is stale** — covers PRs #202046–202050 while 20+ later commits
   are covered by no PR branch.

### 7.6 Two latent bugs to carry into the spec

1. **Vulkan slice offsets are unaligned.** `cajeta_xpu_vulkan.c:1131-1134` self-documents
   that `VkDescriptorBufferInfo.offset` must be a multiple of
   `minStorageBufferOffsetAlignment` and that this is **not enforced** — "not yet
   device-verified". Directly relevant if NPU buffers reuse the slice machinery; NPU DMA
   descriptors have their own stricter alignment rules.
2. The HIP-only profile query in §7.3.

---

## 8. DMA and zero-copy across vendors (RESEARCH, primary sources)

Track 3 complete. Sourced from pinned kernel/XRT/UMD/Khronos source, not blog posts;
the researching agent lost search-engine access partway, so this is **strong on what the
code does and weak on field reports**. Gaps are listed in §10.

### 8.1 XRT *can* import a foreign dma-buf — structurally supported, vendor-untested

This was the load-bearing unknown. The answer is **yes**, and the chain is unguarded:

- `xrt::bo(device, export_handle)` where `export_handle` is a **raw `int32_t` fd**, not
  an opaque cookie (`xrt_bo.h:139-143, 253`).
- The NPU shim passes it through verbatim — `import_fd()` returns the fd unchanged for
  same-process (`xdna-driver/src/shim/device.cpp:2487-2508`).
- It becomes a generic `DRM_IOCTL_PRIME_FD_TO_HANDLE` (`platform_host.cpp:243-270`).
- `amdxdna_gem_prime_import()` does plain `dma_buf_attach()` +
  `dma_buf_map_attachment_unlocked(DMA_BIDIRECTIONAL)`. **Nothing validates provenance.**

There is also a **second import path that bypasses XRT entirely**, already in the uAPI on
this disk: `CREATE_BO` with `type = AMDXDNA_BO_SHMEM` and an `amdxdna_drm_va_tbl`
carrying `dmabuf_fd` with `num_entries = 0`.

**Caveat that keeps this from being a green light:** AMD's only import test is NPU→NPU,
same driver. No AMD doc, sample, or report of `amdgpu` → `amdxdna` exists. Treat as
**structurally supported, vendor-untested** — §10 lists it as the one thing worth proving
before building on it.

Getting the fd from ROCm: `hipMemGetHandleForAddressRange(..., hipMemRangeHandleTypeDmaBufFd, ...)`
gated on `hipDeviceAttributeDmaBufSupported`, or `hsa_amd_portable_export_dmabuf()`.

### 8.2 The userptr path is probably the better design on an APU

`xrt::ext::bo(device, userptr, size)` lowers to `CREATE_BO` with a VA-entry table; the
kernel pins those pages and wraps them in an internal dma_buf. Combined with
`hipHostMalloc`, that yields **one allocation addressable by CPU, GPU and NPU with no
export/import at all** — sidestepping provenance, mmap, HMM and migration hazards
together. The spec should evaluate this **before** the dma-buf path on unified-memory parts.

### 8.3 Coherency: machine-verified non-coherent, and the flush is worse than expected

`/sys/bus/pci/drivers/amdxdna/0000:c4:00.1/device_type` = **0** = `KMQ`, and XRT keys
directly off it: `pdev_kmq::is_cache_coherent()` returns **`false`**
(`src/shim/kmq/pcidev.cpp:44`); `pdev_umq` returns `true`. Coherency is a
**per-generation, runtime-queryable property** — Strix/Strix Halo are non-coherent,
newer parts are not.

Two sharp edges in `SYNC_BO` (`amdxdna_gem.c:944-959`): **`direction` is ignored** (both
values do the same flush-and-invalidate), and **for imported BOs the entire buffer is
flushed regardless of `offset`/`size`**. Sync on a shared GPU buffer is therefore
**O(total bytes), not O(dirty bytes)** — which can erase the copy you saved.

Cross-vendor, the presence of a sync ioctl is a reliable tell: **Intel `ivpu` is snooped
and has no cache-sync ioctl at all**; **Rockchip has `PREP_BO`/`FINI_BO`**. Never assume
shared physical memory implies coherence; always call the backend's sync hook and let it
be a no-op where hardware snoops.

### 8.4 Import support is patchy, and the obvious probe is a trap

| NPU | dma-buf import | `DRIVER_SYNCOBJ` | Implicit fence on shared BOs |
|---|---|---|---|
| AMD `amdxdna` | yes | yes + TIMELINE | `USAGE_WRITE` on all BOs |
| Intel `ivpu` | yes | **no** | `BOOKKEEP` on data BOs — **invisible to consumers** |
| Qualcomm `qaic` | yes (lazy, direction-correct) | no | **none** |
| ARM `ethosu` | **NO — `-EINVAL`** | no | n/a |
| Rockchip `rocket` | **NO — `-EINVAL`** | no | n/a |

**Do not probe with `DRM_CAP_PRIME`** — it is hardcoded true on every DRM/accel node
(`drm_ioctl.c:246-248`), so it tells you nothing. **Probe by attempting the import.**

Intel's `BOOKKEEP` choice (`ivpu_job.c:893-896`) is deliberate and load-bearing:
`DMA_RESV_USAGE_BOOKKEEP` is explicitly the class that does **not** participate in
implicit synchronization, so a GPU consumer will not wait for the Intel NPU.

### 8.5 No standard can be the portable abstraction, and the reason is structural

`DRIVER_COMPUTE_ACCEL` is **mutually exclusive** with `DRIVER_RENDER`/`DRIVER_MODESET`
(`Documentation/accel/introduction.rst`). NPUs live on `/dev/accel/*`, which **by
construction cannot host a Vulkan or OpenCL driver**. Vulkan/OpenCL/Level-Zero external
memory therefore get you the GPU/CPU half of a pipeline and never the NPU half. The only
common denominator is **the dma-buf fd itself**.

Level Zero is not cross-vendor in practice — the loader's Linux discovery is a hardcoded
Intel list. Copy the API *shape* (import/export descriptors chained on allocation,
capability query), not the API.

**The one candidate that could change this:** `VK_ARM_data_graph` + `VK_QCOM_data_graph_model`
add `VK_QUEUE_DATA_GRAPH_BIT_ARM` and `VK_PIPELINE_BIND_POINT_DATA_GRAPH_ARM` — a driver
could expose the NPU as **a queue family on an ordinary `VkDevice`**, collapsing this
whole problem into normal Vulkan memory management. Unratified, vendor-prefixed, no
shipping hardware confirmed. Worth watching, not worth betting on.

**Prior-art vocabulary to copy: LiteRT**, whose `LiteRtTensorBufferType` enumerates
`HostMemory / Ahwb / Ion / DmaBuf / FastRpc / GlBuffer / OpenClBuffer / VulkanBuffer /
MetalBuffer` plus a vendor-custom range, with accelerators declaring what they accept via
`LiteRtTensorBufferRequirements`. IREE has the right shape but no dma-buf. ONNX Runtime
and ExecuTorch have no portable concept at all.

### 8.6 Unified memory does not remove the import — and one BIOS setting can break it

Verified on this box: NPU is **IOMMU group 27**, iGPU is **group 21**. Different
translation contexts, so **the import is the act of creating the second mapping**.

`amdgpu` sets `apu_prefer_gtt` when VRAM < GTT (`amdgpu_ttm.c:2159-2162`). Measured here:
**512 MiB VRAM vs 96 GiB GTT**, so the flag is **true** and `hipMalloc` returns ordinary
page-backed system memory — page-backed sgt on export, no migration on import.

**Raising the UMA carveout above the GTT size flips that flag**, `hipMalloc` lands in the
carveout, and importing into the NPU triggers a **VRAM→GTT pin — a real copy that defeats
the entire exercise.** This must be recorded as a supported-configuration constraint.

Import cost is **O(pages)**: `amdxdna_insert_pages()` runs a per-page `handle_mm_fault()`
loop — ~262,144 iterations for 1 GiB. It is cached at two levels (DRM core keys on
`(drm_file, dma_buf)`; the XRT shim caches `bo_info`). **Import once at pool setup; never
per frame.**

---

## 9. Compiler and IR viability — COMPLETE, and it decides the deliverable

### 9.1 The one-sentence answer

**There is no "NPU target" that can be added to `cajeta-llvm`.** Targeting an NPU splits
into two independent problems, and only one is a compiler-backend problem:

- **(a) generating scalar/vector code for one tile's core** — an ordinary LLVM backend
  problem, already solved out-of-tree for AMD AIE (Peano) and upstream for Hexagon;
- **(b) placing kernels on tiles and routing data between them** — **not expressible in
  LLVM IR or SPIR-V**, with no upstream MLIR abstraction. This is where all the
  difficulty lives.

### 9.2 The architectural verdict

> **A general-purpose imperative language cannot meaningfully target a spatial dataflow
> array as a whole program. It can target one tile of one.**

The dividing line, which survives every piece of evidence gathered:

> **If the accelerator has a program counter and an LLVM backend, a general-purpose
> language reaches it as an ordinary compiler target. If it is a spatial dataflow fabric,
> nobody has solved general code.**

Evidence, strongest first:

- **The one paper that does it lifts to a tensor graph.** Brown & Rodríguez Canal (EPCC),
  *"Lifting to tensors when compiling scientific computing workloads for AI Engines"*
  (arXiv:2605.03566, CCGrid 2026) takes **Fortran** to a Ryzen AI NPU via
  Flang → fir/hlfir → `omp` → their `device` dialect → **`tensor` + `tosa`** (the lift) →
  their own `hlaie` dialect → `aie`/`aievec`/`aiex` → LLVM IR. Their words: *"The
  cornerstone of our approach is to lift the abstraction level from OpenMP to the MLIR
  tensor dialect."* Critically, **they bought generality with a programmer assertion** —
  the `!$omp target parallel do` pragma *is* the proof of iteration independence that
  licenses the tensor rewrite. Results are good (9–14 lines of Fortran matching 150–215
  line hand-written IRON+C++ kernels) but every workload is tensor-shaped: softmax, relu,
  saxpy, dot, l2norm, gemm, two stencils.
- **The strongest counterexample is narrower than it sounds.** Mojo compiles to the
  Qualcomm Hexagon LLVM NPU backend — but only **kernels** target the NPU, and Hexagon is
  a multithreaded VLIW DSP *with a program counter*, not a spatial array.
- **CGRA literature agrees with numbers.** Walter et al. (arXiv:2502.19114): mappers take
  a DFG extracted from C/C++; CGRA-ME *"does not support any nested loops… only maps the
  innermost loop"*; no toolchain auto-unrolls; no pointers, no dynamic memory.
- **Every adjacent system draws the same line.** CIRCT's dynamic path still errors on
  irreducible control flow and non-static memrefs; Exo forbids non-affine indexing and
  value-dependent control flow; Halide's `enum Arch` has no NPU; TVM *deleted* its Ethos-U
  backend in 2025.
- **A vendor conceded it in hardware.** Huawei's Ascend 950 **added a SIMT execution
  mode**; `triton-ascend`'s default `compile_mode` is literally `"unstructured_in_simt"`.
- **AMD says it outright** (MLIR-AIR, arXiv:2510.14871, 22 AMD authors): *"General-purpose
  compilers abstract away parallelism, locality, and synchronization, limiting their
  effectiveness on modern spatial architectures."*

### 9.3 The hardware ceiling

An AIE-ML compute tile has **16 KB of program memory and 64 KB of data memory** (eight
8 KB banks), **no cache, no virtual memory, no MMU**, 20-bit pointers, and **at most two
stream inputs and two outputs**. Three 16 KB input buffers already exhaust data memory.

**16 KB of program memory is roughly one nontrivial function with its inlined callees.**
That is the real ceiling on "run my language on the NPU," and it is a hardware fact, not
a toolchain limitation.

### 9.4 SPIR-V: extending it would be the wrong move

**No NPU consumes SPIR-V as its network IR** — Intel (proprietary blob), Qualcomm
(QNN/MLIR), Arm Ethos (TOSA→Vela), AMD AIE (xclbin/CDO), Rockchip (`.rknn`): unanimous.

Khronos *did* build an NPU path, and **how they built it is the answer**. `SPV_ARM_graph`
(rev 3, 2026-06-19) adds *"a new section after 11 Function definitions where graph
definitions and entry points reside"* — a parallel universe inside the SPIR-V
**container**, disjoint from functions. The spec states: *"This extension does not define
any operations for use within graphs"*; the ops arrive from a **TOSA extended instruction
set** (66 operations) via `OpExtInst`. Arm's own Vulkanised 2026 slide: **"Whole-tensor
execution model • No SIMT shader code • Dispatch does not specify workgroup count."**

Decisive datapoint: `VK_QCOM_data_graph_model` — the one extension naming a real discrete
NPU — **does not use SPIR-V to express the network at all**; models are pre-compiled by
QNN into an opaque blob. Qualcomm adopted the API surface and rejected the SPIR-V payload.

And the finding that matters most here, verified against `llvm-project@main`:

- **The LLVM SPIR-V backend does NOT support `SPV_ARM_graph`/`SPV_ARM_tensors`.**
- **The MLIR SPIR-V dialect DOES** — `SPIRVGraphOps.td`, `SPIRVTosaOps.td` (66 ops),
  `TensorArmType`, plus a complete upstream `-tosa-to-spirv-tosa` lowering pass.

So an end-to-end TOSA → SPIR-V-graph path exists upstream **on a completely different
codepath from the LLVM SPIR-V backend cajeta uses**. Writing our own SPIR-V extension
would duplicate an existing ARM-vendor, unratified extension that only reaches Arm's
in-GPU neural accelerators. **Do not extend SPIR-V for NPU purposes.**

### 9.5 LLVM and MLIR upstream

- **No NPU backend upstream, none proposed.** LLVM 24 release notes mention no AI
  accelerators.
- **Hexagon is half a backend**: 2,249 HVX (`V6_`) intrinsics and **zero** HMX/matrix
  intrinsics. The vector unit is upstream; the matrix engine — the actual NPU part — is not.
- **`llvm.matrix.*` is going nowhere** (still "experimental… in flux"), and **there is no
  tensor type in LLVM IR** — the TLX RFC (2021) never landed.
- **Upstream MLIR has 49 dialects and not one NPU or spatial-dataflow dialect.** Every
  vendor NPU dialect lives in a fork.
- **Linalg-on-tensors is the de-facto neutral level.** Qualcomm's Hexagon-MLIR goes
  torch-mlir/Triton → Linalg → scf/vector/memref/arith → LLVM IR, defining **no new dialect**.
- **IREE targets exactly one NPU** (AMD AIE), experimentally; `iree-amd-aie` has **zero releases**.

### 9.6 The AMD AIE stack is real, open, and Vitis-free

`mlir-aie` is Apache-2.0. **Peano became the default core backend in v1.4.1 (2026-08-11)**;
v1.4.0 added an **HRX (amdxdna) host-runtime backend giving XRT-free deployment**. Vitis is
not required for AIE2/AIE2P.

Peano (`Xilinx/llvm-aie`, branch `aie-public`) is an **LLVM 21** fork shipping as a pip
wheel, with triples `aie2-none-unknown-elf` and **`aie2p-none-unknown-elf` (XDNA2 = Strix
Point)**, and **standalone use is documented**:
`clang++ -O2 --target=aie2-none-unknown-elf …`. It is **not upstream** and shows no sign
of becoming so.

The toolchain flow: `.mlir` → routed physical MLIR → split per `aie.core` → `llc` → per-core
`.o` → `clang`/lld with generated linker script → per-core `.elf` → CDO → PDI → xclbin,
plus an NPU instruction stream.

**But nothing is frozen.** From the community design discussion: *"nowhere between them is
there a frozen interface contract. Every boundary is a source dependency pinned to a
matching commit."* There is **no PTX-equivalent for AIE**; AMD's roadmap lists exploring
one as "Later." Requires kernel **6.17+** — this machine is on 7.0.0-30.

### 9.7 The three options

| | Approach | Cost | Reward |
|---|---|---|---|
| **A** | Emit LLVM IR for `aie2p`, let **Peano** do codegen; hand-generate the surrounding `aie`-dialect MLIR for placement/ObjectFifo/DMA | **Lowest — no backend work** | One tile. "cajeta kernels inside an IRON design," not "cajeta on the NPU" |
| **B** | Add a parallel-loop construct carrying an **independence guarantee**, lift to `linalg`/`tosa`, then to a cajeta spatial dialect → `aie` | High — language + raising pass + own dialect | The only demonstrated route to the **whole** NPU (the Edinburgh recipe) |
| **C** | Target Hexagon / Coral instead — upstream LLVM backends | Zero backend work | Hexagon upstream exposes HVX but **not HMX** — vector unit, not matrix engine |

**Recommended sequence: A first, then B.** A is cheap, proves the whole pipeline end to
end, and `cajeta-llvm` already does most of the work. B is where real offload lives, and
it should not start until A has proven the runtime, DMA and dispatch path.

## 10. Capability description — prior art, and the case against

### 10.1 Three standards converged on one design
Vulkan (`VkCooperativeMatrixPropertiesKHR`), OpenCL (`cl_khr_cooperative_matrix`, April
2026, Arm/Intel/Qualcomm) and SYCL (`matrix_combinations`) all landed on **enumerating
supported `(M, N, K, types, saturation)` tuples** rather than feature bits. SYCL's
discriminated union expressing continuous **and** discrete ranges is **worth copying
outright**. Caveat: Kévin Petit of Arm contributes to all three and authored
`VK_ARM_data_graph` — one person across three bodies weakens this as *independent*
corroboration without weakening the design.

### 10.2 CUDA is a proven anti-model
**Zero hits for "tensor core" across all 145 `cudaDevAttr*` entries**; the real surface is
a prose table, and it is **non-monotonic** — CC 9.0 has FP64 tensor cores, 10.3 does not;
8.0 has INT4, 9.0 does not. **Every "capability ≥ X implies Y" shortcut is wrong.** Never
derive a capability from a generation number or from another capability's presence.

### 10.3 Fabric topology already has a vocabulary — in HSA
`hsa_amd_memory_pool_link_info_t` gives per-hop **min/max bandwidth, min/max latency, link
type** (XGMI/PCIE/QPI), atomics support and NUMA distance — the richest quantitative fabric
vocabulary in the survey, and directly applicable to an NPU sharing LPDDR behind an IOMMU.
HSA also defines **`HSA_DEVICE_TYPE_AIE = 3`**. Counter-note: OpenCL's spec says *"the
device type is purely informational and has no semantic meaning."*

### 10.4 The counter-argument — the spec must answer this
**NNAPI — the only cross-vendor NPU capability model any OS ever shipped, with per-op
supportability queries — was deprecated in Android 15.** Windows ML ships vendor EPs via
Windows Update; ONNX Runtime ships plugin EPs; UXL's answer is a driver-porting kit.
**Four platform owners independently chose to route around cross-vendor capability
description rather than solve it.** The gap is real and unoccupied — but unoccupied for
reasons that deserve understanding. Khronos is currently running an AI Ecosystem Research
Project asking this exact question.

---

## 11. Implications for cajeta

1. **Detection splits in two**: portable identification (`DRM_IOCTL_VERSION`) over
   per-driver capability providers modelled on Vulkan's negotiate-then-enable discipline
   (§7.3), not `DeviceProfile`.
2. **Capability is probed, never inferred** — not from uAPI headers (§6), not from
   `DRM_CAP_PRIME` (§8.4), never from version ordering (§10.2).
3. **Capability records enumerate tuples** (§10.1); copy SYCL's continuous-or-discrete union.
4. **Model fabric topology on `hsa_amd_memory_pool_link_info_t`** (§10.3).
5. **The device model is discovered** — tile and DMA-channel counts are queryable (§5).
6. **Explicit-sync memory model**; sync is O(buffer) on imported BOs (§8.3).
7. **Assume no cross-device fences**; budget a CPU sync at every GPU↔NPU boundary.
8. **Prefer NPU-as-producer** — export near-universal, import patchy.
9. **On unified-memory parts, evaluate userptr before dma-buf** (§8.2).
10. **Buffer-type vocabulary follows LiteRT** (§8.5).
11. **Budget every backend twice** (§7.1).
12. **`LoweringTarget` needs a wave-model opt-out** (§7.4) — prerequisite surgery.
13. **Amend, don't just implement, `cajeta.xpu.interop`** (§7.2 vs §8.5).
14. **Do NOT extend SPIR-V for NPU purposes** (§9.4). The extension exists, is ARM-vendor
    and unratified, lives in the MLIR SPIR-V dialect rather than the LLVM backend cajeta
    uses, and reaches only Arm's in-GPU accelerators.
15. **Do NOT attempt an LLVM NPU backend** (§9.1). Use Peano for tile codegen; the hard
    problem is placement and routing, which is not an LLVM IR problem.
16. **Separate the two deliverables.** Portable *detection* spans six families and is
    achievable now; portable *code generation* spans about one, and reaches one tile.
17. **The spec must answer §10.4** — four platform owners abandoned this problem.
18. **Sequence A then B** (§9.7). A proves runtime/DMA/dispatch with no backend work.

## 12. Open questions

Research is **complete**. Remaining unknowns are empirical and belong in the plan as
spikes, not as further research:

1. **No end-to-end proof an `amdgpu` dma-buf imports into `amdxdna`.** Worth an afternoon:
   `hipMemGetHandleForAddressRange` → `xrt::bo(dev, fd)` → trivial kernel → read back.
2. **Latent HMM hazard** — `-EFAULT` for `VM_IO|VM_PFNMAP` VMAs (§8.1).
3. Whether MoviTools is licensable externally (§9.3) — a question for Intel, not research.
4. Whether AMD intends to upstream Peano — no 2025–2026 RFC found; fork is two years old.
5. Import latency derived from code, never benchmarked.
6. **Windows NPU paths under-researched** — no confident answer on whether anything there
   accepts a compiler IR.
7. Not investigated: `habanalabs` dma-buf, Apple ANE, NVIDIA Grace/NvSci.
8. **§8–§10 are largely primary-source only** — web search budgets were exhausted, so
   community corroboration is thin. Code-level claims are correspondingly strong.

---

## Appendix — reproduction

```bash
python3 specs/schemas/npu-probe.py          # identify + characterize every /dev/accel/*
cat /sys/bus/pci/drivers/amdxdna/0000:c4:00.1/device_type   # 0 = KMQ = NOT coherent
ls /usr/include/drm/*_accel.h               # uAPI inventory
```

Requires `render` group membership.
