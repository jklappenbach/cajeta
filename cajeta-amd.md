# CajetaXPU — AMD (AMDGPU) Bring-Up Plan & Pickup

**Cross-machine pickup for the AMD second backend.** Clone `cajeta-xpu`,
read this, start at Increment 0. This is the *plan*; implementation status
lands back in [`cajeta-xpu.md`](cajeta-xpu.md) as each increment completes.

- Spec: [`cajeta-docs/CajetaXPU.md`](cajeta-docs/CajetaXPU.md)
- Cross-backend discipline: [`cajeta-docs/CajetaXPU-Variance.md`](cajeta-docs/CajetaXPU-Variance.md)
- NVIDIA status (the reference vertical): [`cajeta-xpu.md`](cajeta-xpu.md)
- Where to run on-device: [`cajeta-docs/ci-validation-targets.md`](cajeta-docs/ci-validation-targets.md)

> **Target box:** Strix Halo APU, `gfx1151` (RDNA 3.5), ROCm 7.2.x / HIP.
> **Owned, local, free Tier-1.** Burst-rent `gfx1201` (RDNA 4) later for the
> latest-consumer arch; see `ci-validation-targets.md`.

---

## 0. Why AMD now, and the one rule that shapes everything

The NVIDIA vertical (increments A–J) is **mature and on-device proven**:
SAXPY end-to-end, general compute bodies, static + dynamic shared memory,
AOT CLI, launch-borrow checking. What's left NVIDIA-side is *refinement*
(wave ops, multi-arch fatbin, @Device helper calls) — none of it
de-risks anything or teaches us anything new.

**The locked strategy** (decided with the user, May 2026):

1. Build **NVIDIA + AMD first**. Vulkan is a later gap-filler.
2. **Discover the overlap empirically — the build is the evaluation.** No
   data-matrix evaluation of "what's common." You can't find the
   NVIDIA∩AMD intersection with one backend; AMD *is* the measuring
   instrument. Until it compiles the same kernels, every "core vs.
   vendor-specific" call is a guess against a sample size of one.
3. **NVIDIA = the most comprehensive column** (hardware algorithms in
   silicon). AMD/Vulkan get **software equivalents** where the silicon
   isn't there. Wave ops are the archetype: hardware on NVIDIA,
   emulated on AMD.

**THE RULE — extract the seam *by threading AMD through it*, not before.**
Do **not** sit down and design an abstract `Backend` interface in a vacuum.
Stand AMD up as a concrete second implementation and let the duplication
*tell you* where the seam belongs. An interface validated by one
implementation is not a seam — it's NVIDIA assumptions wearing a hat. The
first time you write "if backend == Nvptx" you've found a seam coordinate;
the methods that end up on the abstraction ARE the measured variance
surface, and that surface is the deliverable.

---

## 1. The seams as they exist today (all hard-coded to NVIDIA)

Everything routes straight to NVIDIA right now. These are the exact
coordinates AMD will fork. **File:line are current as of this writing —
verify before editing.**

| Seam | Location | Today | AMD needs |
|------|----------|-------|-----------|
| AOT backend enum | `src/cajeta/compile/Compiler.h:62` `enum class XpuBackend { None, Nvptx }` | two values | add `Amdgpu` |
| AOT dispatch | `src/cajeta/compile/Compiler.cpp:824` `Compiler::emitXpuKernels()` — early-returns unless `xpuBackend == Nvptx`, then calls `nvidia::emitKernelRegistration` directly | hard call | dispatch on backend |
| JIT dispatch | `test/jit/JitTestHelper.cpp:297` calls `nvidia::emitKernelRegistration` directly | hard call | same dispatch |
| CLI flag | `src/main.cpp:286` `--xpu-backend=none\|nvptx`; arch default `sm_89` (`main.cpp:82`) | nvptx only | accept `amdgpu`; arch `gfx1151` |
| TargetMachine + assembly | `src/cajeta/xpu/nvidia/NvptxBackend.{h,cpp}` — `createNvptxTargetMachine`, `configureDeviceModule`, `emitPtx`, `findPtxas`, `assembleCubin` | PTX→ptxas→cubin | AMDGPU TM → ISA → hsaco |
| Kernel lowering | `src/cajeta/xpu/nvidia/NvptxKernelLowering.{h,cpp}` (~885 lines) — `lowerKernel(method, deviceModule)`; AST → device IR | NVVM intrinsics, `ptx_kernel` CC, alloca AS 0 | AMDGCN intrinsics, `amdgpu_kernel` CC, alloca AS 5 |
| Registration | `src/cajeta/xpu/nvidia/NvptxRegistration.{h,cpp}` — `emitKernelRegistration(kernels, hostModule, arch)`; embeds bytes + `llvm.global_ctors` → `__cajeta_xpu_register_module` | cubin bytes | hsaco bytes (runtime symbol is **already backend-neutral**, keyed by entry name) |
| Driver | `src/cajeta/xpu/nvidia/CudaDriver.{h,cpp}` (dlopen `nvcuda`) + runtime `__cajeta_xpu_*` in `runtime/native/cajeta_runtime.c` (dlopen `nvcuda`: `cuMemAlloc`/`cuLaunchKernel`/…) | CUDA driver | HIP/HSA: `hipMalloc`/`hipModuleLoad`/`hipModuleLaunchKernel`/… |

### What is **already shared** (do not re-derive)

- **Address-space numbers.** `src/cajeta/xpu/core/AddressSpace.h:65`
  `amdNumberFor` ≡ `nvidiaNumberFor`: Generic 0, Global 1, Shared 3,
  Constant 4, Private 5. The lowerer's "global = addrspace(1), shared =
  addrspace(3)" is **correct for AMD unchanged.** AS is *not* a fork point.
- **All frontend work** — `@Kernel`/`@Device` recognition, `KernelArg`
  validation (`XPU-K01`), the `shared` placement keyword, launch grammar,
  MIR scaffolding, launch-borrow checking. Backend-agnostic; AMD inherits
  it for free.
- **The host launch path** — `__cajeta_xpu_launch` / `_buffer_alloc` /
  `_register_module` are name-keyed runtime symbols. The *kernel binary
  format* behind them changes (cubin→hsaco) but the calling shape doesn't.

---

## 2. The variance surface (what actually differs NV vs AMD)

This is the whole game. The NVPTX lowerer emits IR that is ~90%
target-neutral; only these decisions are NVIDIA-shaped. **This list is the
seam.** Pull exactly these onto a small `LoweringTarget` vtable and share
the AST walk.

| Decision | NVPTX (today) | AMDGPU |
|----------|---------------|--------|
| Device triple | `nvptx64-nvidia-cuda` (`NvptxBackend.h:28`) | `amdgcn-amd-amdhsa` |
| Thread/block coord reads | `llvm.nvvm.read.ptx.sreg.tid.*` / `.ctaid.*` / `.ntid.*` | `llvm.amdgcn.workitem.id.{x,y,z}` / `llvm.amdgcn.workgroup.id.*`; **block dim is not an intrinsic** — comes from the dispatch packet (`llvm.amdgcn.dispatch.ptr` + struct offset, or pass as a kernarg) |
| Workgroup barrier | `llvm.nvvm.barrier0` (`bar.sync`) | `llvm.amdgcn.s.barrier` (+ fence ordering for LDS) |
| Kernel calling convention | `ptx_kernel` + `!nvvm.annotations` | `amdgpu_kernel` CC (no annotations metadata) |
| alloca address space | 0 (mem2reg removes most) | **5 (private)**; flat access needs `addrspacecast … to ptr addrspace(0)`. Default alloca AS differs — getting this wrong is the classic first AMDGPU bug |
| Assembler | `ptxas` → cubin (`assembleCubin`) | AMDGPU TM emits ELF object (relocatable) → **`lld` link** → hsaco. Can go object-file-direct via TargetMachine `addPassesToEmitFile(ObjectFile)` — no external assembler needed, unlike ptxas |
| Driver / loader | `nvcuda` (`cuModuleLoadData`/`cuLaunchKernel`) | HIP (`hipModuleLoad`/`hipModuleLaunchKernel`) or raw HSA (`hsa_executable_*`) |
| Dynamic shared (LDS) sizing | `sharedMemBytes` arg to `cuLaunchKernel` | `groupMemBytes`/`sharedMemBytes` field of `hipModuleLaunchKernel`; same `.extern` unsized addrspace(3) global on the IR side |

Wave ops (deferred on NVIDIA too) are the cleanest *later* test of the
seam: NVIDIA lowers to `llvm.nvvm.shfl.sync.*` (hardware); AMD lowers to
`llvm.amdgcn.ds.bpermute` / `ds.swizzle` or a software fallback. **Don't
build them until the seam exists** — they're the first variance-shaped
feature that justifies it, not a prerequisite.

---

## 3. Increment sequence

Mirror how NVIDIA was de-risked: a **GPU-free Tier-0 vertical slice
first** (text-assert the ISA), then on-device. Each increment is one
commit, one test target. Keep `--xpu-backend=none` byte-identical
throughout.

### Increment 0 — Seam extraction (refactor only, NVIDIA stays green)
- Add `Amdgpu` to `XpuBackend` (`Compiler.h:62`) and `amdgpu` to the CLI
  parse (`main.cpp:286`) + usage text. Wire `--xpu-arch` default to
  `gfx1151` when backend is amdgpu.
- Introduce a backend dispatch point: turn the direct
  `nvidia::emitKernelRegistration` calls (`Compiler.cpp:839`,
  `JitTestHelper.cpp:297`) into a switch on `xpuBackend`. AMD arm is a stub
  that throws "not yet implemented" for now.
- **Gate:** full `cajeta_tests` stays green; NVIDIA on-device unchanged. No
  AMD behavior yet — this is pure plumbing. This is also where you'll
  *discover* the real interface: write the switch, see what the AMD arm
  needs, and only then name the abstraction.

### Increment 1 — `AmdgpuBackend` (Tier-0, GPU-free): TargetMachine + ISA
- New `src/cajeta/xpu/amd/AmdgpuBackend.{h,cpp}` parallel to the NVPTX one:
  `createAmdgpuTargetMachine(arch="gfx1151")`, `configureDeviceModule`,
  and an `emitIsa`/`assembleHsaco` that uses the AMDGPU TargetMachine's
  object emission + `lld` (in-process `lld::elf::link` if available, else
  shell out) to produce a relocatable hsaco.
- Confirm `amdgcn` target is registered in the LLVM 22.1.4 build (it is in
  a standard build; verify `llc -march=amdgcn` equivalents exist).
- **Test:** `XpuAmdgpuEmitTests` — assert AMDGPU ISA *text* for a
  hand-built tiny device module. GPU-free. Mirrors `XpuNvptxEmitTests`.

### Increment 2 — `AmdgpuKernelLowering` via the shared AST walk (Tier-0)
- This is the seam payoff. Factor the NVPTX lowerer's target-specific
  decisions (§2 table) into a `LoweringTarget` interface; share the
  ~885-line AST walk. Implement the AMDGPU `LoweringTarget`.
- Start with **SAXPY-class** (params, global buffer load/store, coord
  reads, the arithmetic set) — exactly the subset NVIDIA started from.
  Get alloca-AS-5 + addrspacecast right early.
- **Test:** `XpuAmdgpuLoopEmitTests` — SAXPY + a loop kernel, assert
  AMDGPU ISA text (`global_load`/`global_store`, `s_barrier`,
  workitem-id). Still GPU-free. **This is the first real overlap
  data point** — capture which `LoweringTarget` methods diverged.

### Increment 3 — Registration + AOT/JIT wiring (Tier-0)
- `AmdgpuRegistration::emitKernelRegistration` — same shape as NVPTX:
  embed hsaco bytes as a host constant, `llvm.global_ctors` →
  `__cajeta_xpu_register_module` (unchanged, name-keyed). Fill the
  Increment-0 stub arm.
- **Test:** `XpuAmdgpuAotCliTests` — `--xpu-backend=amdgpu --xpu-emit=isa`
  drops an inspectable artifact; GPU-free for the text case.

### Increment 4 — HIP driver + on-device SAXPY (Tier-1, **Strix Halo**)
- `runtime/native/cajeta_runtime.c`: add a lazily-dlopen'd HIP path beside
  the `nvcuda` one — `hipMalloc`/`hipMemcpy*`/`hipModuleLoad`/
  `hipModuleGetFunction`/`hipModuleLaunchKernel`/`hipDeviceSynchronize`.
  Select by backend at launch. Absent HIP ⇒ graceful no-op (mirror the
  CUDA `available()` skip so CI without ROCm still passes).
- Optional `src/cajeta/xpu/amd/HipDriver.{h,cpp}` mirroring `CudaDriver`
  for the emit-side tests.
- **Test:** `XpuSaxpyAmdDeviceTests` — load hsaco, launch on `gfx1151`,
  verify sum == 4·n over 2²⁰ elements. The AMD analog of
  `XpuSaxpyDeviceTests`. **Skips cleanly if ROCm/HIP absent.**

### Increment 5 — Shared memory (LDS) on-device, then the overlap reckoning
- Bring the `shared` keyword path on-device for AMD (static + dynamic
  LDS). IR side is already addrspace(3); only launch-time `groupMemBytes`
  wiring + barrier fences differ.
- **Test:** AMD tree-reduction, static + dynamic, on `gfx1151`.
- **Then write the overlap reckoning** into `cajeta-xpu.md`: with two
  backends real, document what `LoweringTarget` actually forked vs. what
  stayed shared. That table is the empirical NVIDIA∩AMD core — the thing
  the whole strategy exists to produce, and the input to the later "how
  much can core extend to Vulkan" question.

---

## 4. Toolchain on the Strix Halo box

- **ROCm 7.2.x / HIP** installed (the user already runs ROCm 7.2.2 for
  llama.cpp / Comfy on this class of box). Verify: `rocminfo | grep gfx`
  should show **`gfx1151`** — confirm on the actual instance, don't trust
  this doc (the build is the evaluation).
- **LLVM 22.1.4** with the **AMDGPU target** registered (same LLVM the
  rest of Cajeta uses — no second toolchain). `lld` for hsaco linking.
- **No external assembler** needed (unlike ptxas): the AMDGPU
  TargetMachine emits objects directly; `lld` links to hsaco.
- Build is the same MSYS2/CMake/Ninja flow if on Windows; native ROCm is
  Linux-first, so the on-device increments (4–5) likely run on the Linux
  side of the Strix Halo box. Tier-0 (0–3) is GPU-free and runs anywhere.
- **Binary-lock hazard** (Windows): `taskkill /F /IM cajeta_test.exe`
  before rebuilding after a full test run (see project memory).

---

## 5. Guardrails carried from the strategy

- **Vulkan is later.** But cheap design-time hygiene now (per the user's
  Venn concern): when you add a capability AMD has that Vulkan can't
  cleanly express, gate it behind a capability trait rather than assuming
  universality, and keep launch syntax backend-neutral. Don't *build* for
  Vulkan; just don't paint it out.
- **"Universal" is a high bar.** Only `@Device` helper calls (a Tier-1
  codegen capability) are unconditionally universal. Wave ops, 2D/3D
  launch, etc. are variance-shaped — trait-gate them.
- **Commit discipline:** commit/push only when the user asks. Commit
  messages end with the `Co-Authored-By: Claude Opus 4.8 (1M context)`
  trailer. Force-push only with explicit per-branch authorization.

---

## 6. Definition of done for this phase

SAXPY + a loop kernel + shared-memory reduction compile through
`--xpu-backend=amdgpu` and run on `gfx1151`, with the same Cajeta source
that runs on NVIDIA, and `cajeta-xpu.md` carries the overlap-reckoning
table. At that point the NVIDIA∩AMD core is *measured*, not assumed, and
the Vulkan-reach question becomes answerable.
