# NPU target support

**Status:** `draft` 2026-09-01 — awaiting Julian's review. Evidence in
`npu-target-support-findings.md`; every §N.N reference below points there.
Plan: `agents/npu-target-support-plan.md`.

---

## 1. Definition

cajeta gains the ability to **discover, characterize, and use neural processing units**
as first-class devices, across vendors, without a per-chip special case.

This spec deliberately defines **two capabilities, not one**, because the research shows
they have wildly different reach:

| | Capability | Reach |
|---|---|---|
| **I** | **NPU discovery and characterization** — identify NPUs, describe what they can do, expose them to the runtime and to `cajeta gpu-profile` | **Six driver families** today |
| **II** | **NPU execution** — get cajeta-authored code running on NPU silicon, with DMA-based data movement | **One family (AMD AIE), one tile at a time** |

A single "NPU support" capability would overpromise. §9.2 of the findings establishes the
hard limit: *a general-purpose imperative language cannot meaningfully target a spatial
dataflow array as a whole program; it can target one tile of one.* Capability I is broad
and achievable now. Capability II is narrow, deep, and vendor-specific.

### 1.1 Explicit non-goals

- **No LLVM NPU backend.** §9.1: placement and routing are not LLVM IR problems, and tile
  codegen is already solved by Peano. `cajeta-llvm` gains nothing here.
- **No SPIR-V extension work for NPUs.** §9.4: `SPV_ARM_graph` already exists, is
  ARM-vendor and unratified, lives in the **MLIR** SPIR-V dialect rather than the LLVM
  SPIR-V backend cajeta uses, and reaches only Arm's in-GPU neural accelerators.
  Existing `cajeta-llvm` SPIR-V work stays justified on GPU grounds alone.
- **No whole-program NPU offload.** See §5.3.
- **No Windows or macOS NPU support in this spec.** §12.6 — under-researched. Capability I
  is designed so a Windows provider can be added later without redesign.

### 1.2 Why do this at all — answering the counter-argument

§10.4 records that **four platform owners independently abandoned cross-vendor NPU
capability description**: NNAPI was deprecated in Android 15, Windows ML ships vendor
execution providers via Windows Update, ONNX Runtime ships plugin EPs, UXL ships a
driver-porting kit.

That history is a warning, and this spec answers it by **not repeating what they tried**.
Those four all attempted a *portable execution and operator-coverage* model — "will this
op run on this NPU?" — which is the part that failed. Capability I claims something
strictly smaller and verifiable: **enumeration and description**, not op-level
supportability. `DRM_IOCTL_VERSION` on `/dev/accel/*` already works today with no vendor
userspace (§2), and per-driver capability queries already exist and return real data (§5).
The narrow claim is defensible where the broad one was not.

If Capability I cannot be justified on those terms during review, it should be cut rather
than softened.

---

## 2. Capability I — NPU discovery and characterization

### 2.1 Portable identification

The runtime enumerates NPUs without any vendor userspace, SDK, or root privilege.

**Mechanism (§2, measured):** enumerate `/dev/accel/*`, issue the generic
`DRM_IOCTL_VERSION` (`_IOWR('d', 0x00, struct drm_version)`), dispatch on the returned
driver name. `render` group membership suffices.

**Use cases**

- **UC-1.1** A cajeta program on a machine with an NPU and no vendor stack installed
  enumerates it and learns its driver identity.
- **UC-1.2** A machine with no NPU enumerates zero devices and every downstream API
  reports absence, never an error.
- **UC-1.3** A machine with an NPU the user cannot open (no `render` membership) reports
  the device as *present but inaccessible*, distinctly from absent.
- **UC-1.4** A machine with two NPUs enumerates both, distinctly.
- **UC-1.5** An unrecognized driver name on `/dev/accel/*` is reported as an NPU of
  unknown family, with identification but no capabilities — never dropped silently.

### 2.2 Per-driver capability providers

There is **no generic capability ioctl** (§4). Every driver invents its own vocabulary, so
portability comes from cajeta's interface and each family needs a hand-written provider.

**Use cases**

- **UC-2.1** For `amdxdna`, the provider returns the AIE array geometry — columns, rows,
  core/mem/shim tile counts, per-tile DMA channels, locks, AIE version (§5).
- **UC-2.2** For a family with no provider yet, identification still succeeds and
  capabilities report as unavailable.
- **UC-2.3** A provider that fails mid-query degrades to identification-only rather than
  failing enumeration.
- **UC-2.4** Adding a provider for a new family requires no change to the identification
  layer or to any other provider.

**Design constraints, each from a specific finding:**

- **C-2.a Probe, never infer.** Capability is established by *attempting the operation*
  and recording the result. Not from uAPI headers — §6 records the running kernel
  rejecting two command types its header declares. Not from `DRM_CAP_PRIME` — hardcoded
  true on every node (§8.4). Not from version ordering — CUDA's capability surface is
  **non-monotonic** (§10.2).
- **C-2.b Negotiate then enable.** Model providers on the Vulkan `pNext` discipline
  (`cajeta_xpu_vulkan.c:348-720`), which enables only advertised bits — *not* on
  `DeviceProfile`, whose `cajeta_xpu_query_raw_device` is HIP-only despite headers
  claiming otherwise (§7.3).
- **C-2.c Enumerate tuples, not flags.** Follow the Vulkan/OpenCL/SYCL cooperative-matrix
  convergence (§10.1); adopt SYCL's discriminated union for continuous-or-discrete ranges.

### 2.3 Capability vocabulary

- **UC-3.1** A caller asks which data types a device supports and receives an enumerated
  set, never a version number to compare against.
- **UC-3.2** A caller asks for memory-fabric characteristics and receives per-hop
  bandwidth, latency, link type and NUMA distance, modelled on
  `hsa_amd_memory_pool_link_info_t` (§10.3).
- **UC-3.3** A caller asks for DMA topology and receives per-tile-class channel counts
  (§5) — the resource a dataflow mapping must schedule against.
- **UC-3.4** A capability absent from a provider's knowledge reports as *unknown*,
  distinctly from *unsupported*.

### 2.4 Surfacing

- **UC-4.1** `cajeta gpu-profile` (or a successor verb) lists NPUs alongside GPUs.
- **UC-4.2** A cajeta program queries NPU presence and capabilities through the stdlib.
- **UC-4.3** NPU enumeration never initializes HIP, Vulkan, CUDA or XRT — it must work on
  a machine with none of them (§7.1's soft-dependency discipline).

---

## 3. Capability II — execution on AMD AIE

Scope: **AMD XDNA/XDNA2 via Peano and MLIR-AIE**, per §9.7 option A. This is the only
on-package NPU where a third party can run its own kernels (§9.6), and Peano became the
default core backend in `mlir-aie` v1.4.1 (2026-08-11), with Vitis no longer required.

### 3.1 What "execution" means here, precisely

**One tile.** An AIE-ML compute tile has **16 KB program memory and 64 KB data memory, no
cache, no MMU, 20-bit pointers, at most two stream inputs and two outputs** (§9.3). 16 KB
is roughly one nontrivial function with its inlined callees.

The deliverable is therefore **"cajeta kernels inside an IRON design," not "cajeta on the
NPU."** The spec says this plainly so the plan cannot drift into promising the latter.

**Use cases**

- **UC-5.1** A cajeta function marked as an NPU kernel compiles to LLVM IR for
  `aie2p-none-unknown-elf`, is passed to Peano, and produces a per-core ELF.
- **UC-5.2** A kernel exceeding 16 KB program memory or 64 KB data memory is **rejected at
  compile time with a diagnostic naming the limit and the measured size** — never
  silently truncated or deferred to a runtime failure.
- **UC-5.3** A kernel using a construct the tile cannot support (dynamic allocation,
  recursion, unbounded control flow) is rejected with a diagnostic naming the construct.
- **UC-5.4** The surrounding `aie`-dialect MLIR — tile placement, ObjectFifos, DMA
  configuration — is generated by cajeta, not hand-written by the programmer.
- **UC-5.5** A design targeting a device whose probed geometry cannot host it (§2.3) is
  rejected before invoking the toolchain.

### 3.2 Data movement

- **UC-6.1** Host memory is made available to a kernel and results read back.
- **UC-6.2** On a unified-memory part, the runtime uses the **userptr path**
  (`hipHostMalloc` + `xrt::ext::bo(device, userptr, size)`) giving one allocation
  addressable by CPU, GPU and NPU with **no import at all** (§8.2). This is evaluated
  *before* dma-buf.
- **UC-6.3** Where userptr is unavailable, a GPU buffer is shared by dma-buf export/import
  (§8.1) — **only if the empirical spike proves it works** (§12.1).
- **UC-6.4** Every device access is bracketed by explicit cache maintenance. Coherency is
  queried, never assumed: this machine reports `device_type = 0` (KMQ) and XRT's
  `is_cache_coherent()` returns **false** (§8.3).
- **UC-6.5** The runtime treats a GPU→NPU boundary as requiring **its own ordering**.
  amdxdna publishes its fence with `USAGE_WRITE` so amdgpu waits for the NPU, but amdxdna
  never adds implicit dependencies, so the NPU does **not** wait for the GPU (§6).
- **UC-6.6** Buffers are imported **once at pool setup** and cached; import is O(pages)
  with a per-page fault loop (§8.6).
- **UC-6.7** A configuration where `apu_prefer_gtt` is false (UMA carveout raised above
  GTT size) is **detected and reported**, because import then triggers a VRAM→GTT pin — a
  real copy that defeats the exercise (§8.6).

### 3.3 What is deliberately not attempted

- **Whole-program offload.** §9.2. The only demonstrated route to the whole NPU is the
  Edinburgh recipe — a parallel-loop construct carrying an independence guarantee, lifted
  to `linalg`/`tosa` (§9.7 option B). That is a separate, larger spec, and it must not
  begin until option A has proven the runtime, DMA and dispatch path.
- **Vectorization.** AIE cores run scalar in the open MLIR flow; the Edinburgh work had to
  detour through `aie-translate` to C++ with AIE intrinsics (§9.2).
- **Other vendors.** Intel's NPU has no module/kernel API at all — every DDI entry is
  `nullptr` (§9.1). Its only third-party route is emitting VPUIP/ELF-dialect MLIR into an
  open Apache-2.0 compiler, which is a different project.

---

## 4. Prerequisite work in existing code

These are not NPU features; they are defects and gaps that block NPU work.

- **UC-7.1 `LoweringTarget` gains a wave-model opt-out.** `waveWidth`, `waveShuffle`,
  `quadSwap`, `workgroupBarrier` are vtable members with GPU-shaped defaults, and the CPU
  backend *fakes* them. `CajetaXPU.md:1088-1097` already states NPUs must opt out; no
  mechanism exists (§7.4). **Nothing in Capability II can begin before this.**
- **UC-7.2 Every backend is implemented twice** — a C++ side under `src/cajeta/xpu/` for
  compiler and tests, and a C side under `runtime/native/` shipping as embedded bitcode
  (§7.1). The NPU backend budgets for both.
- **UC-7.3 `cajeta.xpu.interop` is amended, not merely implemented.** It specs host
  staging / external-memory FD / DMABUF with zero code (§7.2), but bets on
  `VK_KHR_external_memory_fd` — and §8.5 shows NPUs sit outside Vulkan entirely, because
  `DRIVER_COMPUTE_ACCEL` is mutually exclusive with `DRIVER_RENDER`. The design needs a
  correction, not just an implementation.
- **UC-7.4 Buffer-type vocabulary follows LiteRT** (§8.5) — distinct `HostPtr`, `DmaBuf`,
  `UserPtr`, `OpenCl`, `Vulkan` cases plus a vendor escape hatch. They do not unify.

### 4.1 Unrelated defects surfaced during research

Filed here for visibility; each needs its own row, not this plan.

- **`cajeta-llvm` release tag `cajeta-llvm-23-r6` was cut off the upstream mirror branch**
  (`ce465594e239`), which is not an ancestor of `cajeta-spirv` — **that release artifact
  contains none of the fork's SPIR-V work** (§7.5).
- **`UPSTREAM-PRS.md` contradicts HEAD**: it claims the cooperative-matrix Vulkan
  memory-model derivation is "capability-derived (not a name scan)"; `SPIRVModuleAnalysis.cpp:158-177`
  is a `starts_with` name scan. The clean version is on the unmerged branch
  `pr/spirv-coopmatrix-vulkan-memory-model` (§7.5).
- **`UPSTREAM-PRS.md` is stale** — 20+ commits map to no PR branch (§7.5).
- **Vulkan slice offsets are unaligned** — `minStorageBufferOffsetAlignment` is not
  enforced and self-documented as "not yet device-verified" (§7.6). NPU DMA descriptors
  have stricter alignment, so this bites if NPU buffers reuse the slice machinery.
- **`cajeta_xpu_query_raw_device` is HIP-only** while two headers claim it reads
  `cudaDeviceProp` (§7.3).

---

## 5. Acceptance posture

- **Capability I is done** when a machine with no vendor stack enumerates and characterizes
  its NPUs, an unknown family degrades gracefully, and a second provider can be added
  without touching shared code.
- **Capability II is done** when a cajeta-authored kernel runs on this machine's NPU,
  data reaches it without a host round-trip where the hardware allows, and the tile limits
  are enforced at compile time with actionable diagnostics.
- **Neither is done** on the strength of a passing test alone where the finding it
  implements was primary-source-only (§12.8). Spikes named in the plan must run on
  hardware first.

## 6. Open questions for Julian

1. **Is Capability I worth building given §10.4?** Four platform owners walked away from
   the general problem. §1.2 argues the narrow claim survives. This needs your judgement,
   and cutting Capability I is a legitimate outcome.
2. **Is Capability II worth it at one tile and 16 KB?** The honest framing is "cajeta
   kernels inside an IRON design." If that is not interesting, option C (Hexagon, upstream
   backend, no NPU matrix engine) or stopping after Capability I are both rational.
3. **How much churn is acceptable?** Nothing below Linalg in this ecosystem is frozen;
   AMD's own community says the AIE stack has "frozen none" of its interfaces, and every
   boundary is a commit-pinned source dependency.
4. **Does the `cajeta.xpu.interop` correction (UC-7.3) belong here or in its own spec?**
   It is prerequisite either way.
