# CajetaXPU — Implementation Handoff

Cross-machine handoff for the `cajeta-xpu` feature branch. Pick up here
after cloning to another box (e.g. Windows). The design spec is
[`cajeta-docs/CajetaXPU.md`](cajeta-docs/CajetaXPU.md); the variance
discipline is [`cajeta-docs/CajetaXPU-Variance.md`](cajeta-docs/CajetaXPU-Variance.md).
This file tracks *implementation status* — what's built, what's stubbed,
and what's next.

> **Status: SAXPY runs on a real NVIDIA GPU (design phase 2 milestone).**
> The NVPTX vertical slice is proven end-to-end on an RTX 4090 (sm_89,
> CUDA 12.9, LLVM 22.1.4): the spec SAXPY `@Kernel` source compiles
> `cajeta → device LLVM IR → PTX → .cubin → cuLaunchKernel` and the
> on-device result matches the CPU reference over 2^20 elements
> (`test/xpu/XpuSaxpyDeviceTests.cpp`). Steps 9–11 of the original plan
> are demonstrated; what remains is generalization + the carried-over
> frontend deferrals (see "Vertical-slice status" below).
>
> Earlier baseline (still true on the CPU-emulation path): step 7 of 11,
> the design phase-1+2 CPU-emulation milestone ([`CajetaXPU.md`
> §12](cajeta-docs/CajetaXPU.md)).

---

## Vertical-slice status (NVPTX, steps 8–11)

The NVIDIA backend was built as a **vertical slice** to de-risk the
external toolchain early rather than finishing all of step 8's frontend
deferrals first. Six commits on `cajeta-xpu` (after the restore commits):

| Increment | What landed | Tests |
|-----------|-------------|-------|
| A | launch grammar (`kernel.launch(cfg)(args)`, `[...]` literals) + `CallExpression` / `ArrayLiteralExpression` AST | `XpuLaunchGrammarTests` |
| B | launch-site recognition + MIR `bodyOps`/launch-site walker (`XpuMirBuilder`) | `XpuMirLaunchSiteTests` |
| C | `NvptxBackend` (TargetMachine + PTX emit) and `NvptxKernelLowering` (kernel AST → device IR) | `XpuNvptxEmitTests` |
| D | `ptxas` → cubin assembly (`assembleCubin`) | `XpuNvptxEmitTests` |
| E+F | `CudaDriver` (dlopen nvcuda) + SAXPY launched & verified on-device | `XpuSaxpyDeviceTests` |

**Deviations from [`CajetaXPU.md`](cajeta-docs/CajetaXPU.md) (intentional, slice-scoped):**

1. **Kernel bodies lower via a dedicated `NvptxKernelLowering`** that walks
   the kernel AST and emits device IR directly — NOT by reusing the host
   `CajetaLlvmVisitor` (entangled with scope/drop/bounds runtime emits),
   and NOT "from MIR" as §5.2 diagrams. MIR stays thin metadata. The
   lowerer builds device LLVM types fresh from `CajetaType` flags in the
   device `LLVMContext` (never `getLlvmType()`, whose cache is host-context-
   bound). Supported subset = SAXPY-class kernels; unsupported constructs
   raise `XPU-N01`. Generalizing the subset is follow-on work.
2. **Single `sm_89` cubin, no fatbin** (multi-arch §5.2/§10.2 deferred).
3. **dlopen `nvcuda.dll`** rather than link `cuda.lib` — matches §1.1/§9
   and keeps the build CUDA-free; absent driver ⇒ `CudaDriver::available()`
   is false and device tests skip.
4. Spec `@kernel`/`global_id_x()` vs code `@Kernel`/`Thread.globalIdX()` —
   unchanged; carried-over deferral #6.

**Still open (generalization + carried-over deferrals):**

- **Host launch via Cajeta source.** The compiler half is **done**:
  `CallExpression::generateCode` lowers `kernel.launch(stream, grid:,
  block:)(args)` to `__cajeta_xpu_launch(name, gridX, blockX, argv)`,
  marshalling Buffer args to their device handle and scalars by value
  (`XpuLaunchCodegenTests`). Still to wire so it runs on-device through the
  JIT: make the `__cajeta_xpu_launch` / `_buffer_*` / `_stream_*` runtime
  symbols real (CUDA-backed — must live in the C runtime bitcode so the JIT
  resolves them, mirroring `CudaDriver`), a `Buffer.alloc` factory, a
  compile pass that builds each `@Kernel`'s cubin and emits a global ctor
  calling `__cajeta_xpu_register_module`, and the `JitTestHelper` hook to run
  it. (Compiler plumbing — not a toolchain risk now that the runtime path is
  proven.)
- **Launch-borrow-scope checking** (deferral #4): **done (core case)** —
  `launch` borrows each Buffer arg, released at `Stream.sync()` /
  `Event.waitHost()`; freeing a still-borrowed buffer is `XPU-K02`
  (`XpuLaunchBorrowTests`). Deferred: per-stream tracking, the move/reassign
  and auto-drop-at-scope-exit-without-sync cases, and cross-stream WAR/RAW
  (§11 cases 2–3).
- **`--xpu-backend` / `--xpu-arch` / `--xpu-emit` CLI flags** for the AOT
  `cajeta` path (the slice is JIT-test-driven).
- Broaden `NvptxKernelLowering`'s construct coverage beyond the SAXPY subset.

---

## Branch provenance

This branch restores the XPU work that was reverted on `main` by
`211e93e` ("revert: roll back XPU work to unblock v0.1.0 release"). The
reverted work — 5 commits, 38 files, +2936 lines — was cherry-picked
onto post-revert `main` and is **byte-identical** to the original
pre-revert commits (verified against `fb4d8d3`).

The 5 commits on this branch (oldest → newest):

| Commit    | Maps to steps | Summary |
|-----------|---------------|---------|
| `6f91da4` | (docs)        | CajetaXPU variance discipline + PR template |
| `4112e8e` | 1             | `@Kernel`/`@Device`/`@Host`/`@Wave` attribute recognition |
| `6325c7b` | 2             | `cajeta.xpu.core` stdlib declarations + runtime stubs |
| `c1ecd2c` | 3, 4, 5       | KernelArg validation + MIR scaffolding + address-space recognition |
| `76665b6` | 6, 7          | SAXPY end-to-end on CPU-emulation path |

Original (reverted) equivalents on the pre-revert line, for reference:
`7001e2c`, `8ab8dca`, `3e8c7ec`, `13453e3`, `fb4d8d3`.

---

## What was done — steps 1–7

### Step 1 — Attribute recognition  ✅  (`4112e8e`)
- `@Kernel` / `@Device` / `@Host` / `@Wave(width: N)` / `@Backend(...)`
  recognized via the existing `Annotatable` system — no visitor work.
- Grammar tweak (**step 1b**): `elementValuePair` now accepts `:` as an
  alias for `=`, so `@Wave(width: 32)` and `@Wave(width = 32)` both parse
  (`antlr4/CajetaParser.g4`).
- `src/cajeta/xpu/core/XpuAttributes.h` — short-name constant registry +
  `isKernel` / `isDevice` / `isHost` predicates.
- `src/cajeta/xpu/core/XpuKernelAttr.{h,cpp}` — typed view collecting
  `@Wave(width)` + `@Backend` (single or list) off a `@Kernel` method.
- Tests: `test/xpu/XpuAttributesTests.cpp` (9).
- **Naming note:** code uses PascalCase `@Kernel` (matches
  `@Native`/`@Component`); the spec spells it lowercase `@kernel`. To be
  reconciled in docs.

### Step 2 — `xpu.core` stdlib + runtime stubs  ✅  (`6325c7b`)
- 13 `.cajeta` declaration files under
  `runtime/src/cajeta/xpu/core/`: `Stream`, `Event`, `Fence`, `Thread`,
  `Workgroup`, `Barrier`, `Wave`, `Buffer`, `Capabilities`,
  `AddressSpace`, `KernelArg`, `KernelError`.
- `@Native` C runtime stubs in `runtime/native/cajeta_runtime.c`
  (search `cajeta.xpu.core runtime stubs`, ~line 3861). **Every stub
  returns zero / no-ops** — LLJIT materializes externs eagerly, so they
  must exist before any real implementation does. Calling them yields a
  null Stream / zero coordinate / no-op barrier — not a crash.
- Tests: `test/xpu/XpuCoreStdlibTests.cpp`.

### Step 3 — KernelArg validation  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/core/KernelArgTrait.{h,cpp}` — `isKernelArgAdmissible`
  + `validateKernelParams`. Admits: primitives, primitive arrays
  (recursive on element type), `Buffer<T>` (canonical-name prefix), and
  any class implementing the `cajeta.xpu.core.KernelArg` marker
  interface. Non-admissible params throw `cajeta::Exception` with
  errorId **`XPU-K01`**.
- Wired into `Method::generateCode` (`src/cajeta/method/Method.cpp`,
  after the idempotent `llvmBasicBlock` guard, so it runs once/method).
- **Not yet admitted:** POD structs without explicit `implements
  KernelArg`, `Texture<...>` / `Sampler` (types not declared),
  `@PushConstant` (Vulkan-only, deferred).
- Tests: `test/xpu/XpuKernelArgTests.cpp` (5).

### Step 4 — XPU MIR scaffolding  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/mir/`: `XpuMirOp.h` (9-op set), `XpuMirType.h`,
  `XpuMir.h` (Kernel/LaunchSite/Module containers), `XpuMirBuilder.{h,cpp}`,
  `XpuMirPrinter.{h,cpp}`.
- MIR is **thin by design**: it carries the structural envelope
  (canonical name, address-space-qualified param list, `@Wave`/`@Backend`
  attrs) around kernel bodies. Bodies still lower through the existing
  AST→LLVM visitor; MIR does not replace them.
- **Placeholders (intentionally empty in v1):** `XpuMirKernel::bodyOps`
  (leaf-builtin walker not run), `XpuMirLaunchSite` fields
  (grid/block/stream/args not captured).
- Tests: `test/xpu/XpuMirBuilderTests.cpp` (7) — incl. printer round-trip.

### Step 5 — Address-space recognition  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/core/AddressSpace.h` — 5 qualifiers
  (`Generic/Global/Shared/Constant/Private`) + per-backend numbering
  tables (`nvidiaNumberFor` / AMD shares it; `spirvNumberFor` is a stable
  test integer, real SPIR-V storage classes mapped later). The literal
  artifact behind variance-doc rows 4–5.
- `isAddressSpaceCanonical` recognizes
  `cajeta.xpu.core.{Global,Shared,...}[<...>]` names; `XpuMirBuilder`
  uses it to tag param types.
- **Deferred:** the leaf-builtin body-op walker (populating `bodyOps`
  with `Op_ThreadId` etc.) and the real LLVM-IR address-space lowering —
  those land with the NVPTX backend (step 9).
- Tests: `test/xpu/XpuAddressSpaceTests.cpp` (2).

### Step 6 — Launch surface (minimal form)  ✅  (`76665b6`)
- KernelArg admissibility extended to **primitive arrays** (`float32[]`,
  `int32[]`, …) so a SAXPY-shaped kernel signature compiles.
- **Deferred:** the real `saxpy.launch(stream, grid:, block:)(args)`
  postfix syntax needs an expression-grammar extension (postfix `(args)`
  after an expression result) — *not yet implemented*. See "Carried-over
  deferrals" below.

### Step 7 — CPU-emulation SAXPY end-to-end  ✅  (`76665b6`)  ← current
- A `@Kernel` compiles from Cajeta source via JIT; host driver (also
  Cajeta source) iterates the index space and calls the kernel per
  index; output matches the CPU reference (`y[i] = a*x[i] + y[i]`,
  sum = 32.0 over 8 elements).
- Because the real `launch()` syntax is deferred, the "launch"
  abstraction degenerates to a host-side loop. The kernel takes the
  thread index `i` as an **explicit parameter** rather than calling
  `xpu.thread.global_id_x()` — so the coordinate-reader stubs (still
  returning 0) are sidestepped, not exercised.
- Tests: `test/xpu/XpuLaunchAndSaxpyTests.cpp`.

---

## Carried-over deferrals (must be picked up regardless of step numbering)

These were shipped as stubs/placeholders to reach the milestone. They
are prerequisites for real backend codegen and shouldn't get lost in the
step-renumbering:

1. **Real `launch(stream, grid:, block:)(args)` grammar** — postfix
   `(args)` call after an expression. Blocks real launch-site
   recognition and stream/event scheduling.
2. **Launch-site resolution** — populate `XpuMirLaunchSite`
   (grid/block/stream/arg references). Currently a placeholder.
3. **MIR body-op walker** — populate `XpuMirKernel::bodyOps` with leaf
   builtins (`Op_ThreadId`, `Op_BarrierWorkgroup`, …). Currently empty.
4. **Deferred-borrow / launch-borrow-scope checking** — the borrow
   checker should treat `launch` as a borrow held until the next
   `Stream.sync()` / `Event.wait()` (spec §3.5, §11). Not implemented.
5. **Real runtime impls** behind the zero-stubs in `cajeta_runtime.c`:
   thread/workgroup coordinate readers (TLS set by the dispatch loop),
   `Stream`/`Event`/`Fence`, and a `Buffer<T>` host allocator. The
   CPU-emulation path needs these to run kernels that actually read
   `xpu.thread.*` or use `Buffer<T>`.
6. **Spec/code spelling reconciliation** — `@Kernel` (code) vs `@kernel`
   (spec).

---

## What's left — steps 8–11 (NVIDIA / NVPTX native backend)

> **Confidence note.** Steps **9** and **11** are anchored by explicit
> forward-references in the code (see citations). Steps **8** and **10**
> are *reconstructed* — the original 11-step plan's exact wording lived
> in the prior conversation, not in the repo, so the 8/10 boundaries
> below are my best reconstruction from [`CajetaXPU.md` §12](cajeta-docs/CajetaXPU.md)
> (design phase 2) and the code's hints. **Adjust the 8/10 split if it
> doesn't match your intended plan.**

### Step 8 — Frontend/MIR completion (reconstructed)
Close the carried-over deferrals that real codegen depends on: the
`launch()` postfix grammar (#1), launch-site resolution (#2), the MIR
body-op leaf walker (#3), and launch-borrow-scope checking (#4). After
this, there is a *complete* MIR (signature envelope **plus** body ops
**plus** launch sites) for a backend pass to consume.

### Step 9 — NVPTX lowering pass (anchored)
> Code: `XpuAddressSpaceTests.cpp` ("real lowering on LLVM IR lands with
> the NVPTX backend (step 9)"); `XpuCoreStdlibTests.cpp`,
> `KernelArgTrait.h`, `XpuMirPrinter.h` ("step 9+").

Lower MIR → LLVM IR with: NV address spaces (the `nvidiaNumberFor`
table), `ptx_kernel` calling convention, and target intrinsics for
thread/workgroup IDs, barriers, and wave ops (the `xpu.thread.x` →
`llvm.nvvm.read.ptx.sreg.tid.x` family in `CajetaXPU.md` §5.4).
Capability traits become Target-resolved-at-codegen here.

### Step 10 — NVPTX assembly + CUDA runtime (reconstructed)
`llc` → `.ptx` → `ptxas` → `.cubin`; fatbin embedding (`--xpu-arch=sm_*`,
`CajetaXPU.md` §5.2/§10.2). Real CUDA-driver dispatch runtime replacing
the zero-stubs on the NVPTX path: `cuMemAlloc`-backed `Buffer<T>`,
`cuStream`-backed `Stream`/`Event`, and a real kernel launch.

### Step 11 — NVPTX SAXPY device launch (anchored)
> Code: `XpuLaunchAndSaxpyTests.cpp` ("On the NVPTX backend (step 11)
> the same source compiles into a real device launch").

The step-7 SAXPY source compiles unchanged into a real on-device launch
(`cajeta` → `.cubin` → launch). This is the variance-discipline payoff:
one source, CPU-emulation **and** NVPTX. Milestone = design phase 2
("NVIDIA backend, compute only") done.

After step 11, the design roadmap continues with AMD (phase 3), tensor
cores (phase 4), Vulkan (phases 5–6), etc. — those are beyond the
current 11-step plan and tracked in `CajetaXPU.md` §12.

---

## Resuming on a new machine

1. **Check out the branch:** `git checkout cajeta-xpu`.
2. **Build** (both flags are load-bearing):
   ```
   cd build && cmake -G Ninja -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm .. && ninja
   ```
   On Windows the LLVM path differs — `main` already carries the
   MSYS2/MinGW release fixes (LLP64 type widths, path-separator
   normalization, `_aligned_malloc`, etc.); follow those.
3. **Verify the restored work compiles and the XPU tests pass** before
   starting step 8 — this work was reverted to unblock a release, so a
   green build on the target machine is the first checkpoint. Run the
   XPU subset:
   ```
   ctest -j $(nproc) -R Xpu
   ```
   (Tests: `XpuAttributesTests`, `XpuCoreStdlibTests`, `XpuKernelArgTests`,
   `XpuMirBuilderTests`, `XpuAddressSpaceTests`, `XpuLaunchTests`.)
4. **Then** start step 8 (frontend/MIR completion) per above.
