# XPU / @Kernel build ergonomics

> Status: DRAFT. Catalogues the build-configuration rough edges hit while building a
> standalone CPU `@Kernel` program (the matmul-kernel numpy-slaughter experiment,
> `samples/matmul-kernel/`), and specifies the fixes that make building an XPU program a
> one-command operation instead of a manual-linking expedition.

## 1. Definition

### 1.1 Purpose
Building a Cajeta program that uses a `@Kernel` (even on the CPU backend) currently requires
dropping out of the ergonomic build tool into the raw compiler plus a hand-assembled `clang`
link line — hunting native objects in the CMake tree, hand-stubbing optional natives, and
discovering required libraries by trial. This spec records every rough edge encountered and
specifies the changes that make `@Kernel` builds first-class.

### 1.2 Discovery context (what was being built)
A standalone CPU `@Kernel` matmul (`samples/matmul-kernel/src/mm/MatmulKernel.cajeta`), built
to measure 32-core parallelism against numpy. It compiled and ran (2.78 → 0.43 ms), but only
after working through every issue in §2. The kernel *code* was easy; the *build* was not.

### 1.3 KEY FINDING (re-scopes everything below)
`cajeta --emit=exe --xpu-backend=cpu <entry> <src> <out>` **already builds a runnable binary in
one command** — it auto-generates the OptiX stub, links the TLS + core runtime objects, pulls
the right system libs, and tree-shakes. (Verified: it produced `a.out`, identical 0.43ms result
to the 8-step manual build.) **Almost every rough edge in §2 came from following the tour's
`run-gpu.sh` `--emit=obj` + manual-`clang` recipe instead of `--emit=exe`.** That re-scopes the
work to: (P1) make the *build tool* able to do this (§3.1), (P1) make `--emit=exe` the taught
default and fix the misleading tour recipe (§3.2), and (P2) harden the `--emit=obj` power-user
path (§3.3-3.6).

### 1.4 Non-goals
- Kernel performance / autovectorization (separate work).
- GPU device drivers / runtime selection (this is about the build, not execution).

## 2. The rough edges (what broke, with evidence)

> NOTE (per §1.3): §2.2-2.7 are all **`--emit=obj` manual-link** edges that `--emit=exe`
> already avoids. §2.1 (build tool) and the *teaching* of the manual path are the real gaps.


### 2.1 The build tool cannot build kernels (the headline gap)
- 2.1.1 As a developer, `cajeta build` / `cajeta release` (the manifest-driven build tool the
  benchmark + tour samples use) has **no way to select an XPU backend** — `--xpu-backend` is a
  raw-compiler CLI flag, absent from the manifest and from build flavors. So a `@Kernel`
  program **cannot be built with the build tool at all**; you must invoke
  `cajeta --emit=obj --xpu-backend=cpu …` directly and link by hand.

### 2.2 Standalone `--emit=obj` drags in the whole stdlib's native dependencies
- 2.2.1 The embedded stdlib object (`cajeta.runtime.__stdlib__.o`) references `@Native` symbols
  for subsystems the program never uses — TLS (`__cajeta_tls_*`, 21 symbols) and OptiX ray-
  tracing (`cajeta_xpu_optix_*`, 8 symbols) — so the link fails with undefined symbols on a
  trivial matmul program.
- 2.2.2 `--emit=obj` does **not** tree-shake (the prune runs only on the `--emit=exe` path), so
  there is no way to drop the unused subsystems from the object.

### 2.3 No shippable runtime library; native objects must be hunted in the build tree
- 2.3.1 The C implementations of those `@Native` symbols live as CMake intermediate objects at
  `build-cajeta/src/CMakeFiles/cajeta_lib.dir/__/runtime/native/{cajeta_runtime,cajeta_tls}.c.o`
  — an internal build path a user must know to find and add to the link line. There is **no**
  `libcajeta_runtime.a` / documented runtime-object set.

### 2.4 Optional natives have no object at all → hand-written stubs
- 2.4.1 OptiX natives (`cajeta_xpu_optix_*`) are referenced by the embedded stdlib
  (`AccelerationStructure`) but **not compiled** on a CPU-only box, so even after linking the
  native objects the link fails. The only recourse was to hand-write a `.c` with 8 empty stub
  functions. There is no stub library and no weak-symbol fallback.

### 2.5 The required link line is undocumented and discovered by trial
- 2.5.1 The full stdlib needs `-lssl -lcrypto -ldl -lpthread -lm`; the SSL/crypto requirement
  is invisible until the link fails on TLS. The tour's `run-gpu.sh` lists only
  `-ldl -lpthread -lm`, which is insufficient for the full embedded stdlib.

### 2.6 Object layout breaks the naive link glob
- 2.6.1 `--emit=obj` writes objects into package subdirectories
  (`build/bin/mm/MatmulKernel.o`), so a `build/bin/*.o` glob silently misses the user's own
  object — producing the maximally-confusing `undefined symbol: mm.MatmulKernel::run()` and
  `undefined symbol: main` (the embedded `main` calls the entry it can't find). You must
  `find build/bin -name '*.o'` (and the tour's `mapfile -d ''` form is bash-only).

### 2.7 Toolchain discovery is manual
- 2.7.1 The linker is hardcoded to `clang-22` in `run-gpu.sh`; on this box it is at
  `/opt/rocm-7.x/llvm/bin/clang-22` while the fork compiler is LLVM v23. The
  version/path matching is implicit and left to the user.

### 2.8 (Adjacent) generated stdlib `.ll` artifact lives in the source tree
- 2.8.1 `samples/profile/cajeta.runtime.__stdlib__.ll` is a gitignored generated artifact
  sitting in the source tree; a tree-wide refactor sweep touched it and it had to be restored/
  regenerated. Generated artifacts in source dirs are a footgun for global edits.

## 3. Feature: make `@Kernel` builds first-class (the fixes)

### 3.1 Build-tool XPU backend selection (fixes §2.1)
- 3.1.1 As a developer, I can declare an XPU backend in `cajeta.json` (e.g.
  `"build": { "xpu-backend": "cpu" }` or a flavor knob `"xpu-backend": "cpu,vulkan"`) so
  `cajeta build` / `cajeta release` builds a `@Kernel` program with no raw-compiler escape.
- 3.1.2 The build tool threads the setting to the compiler's `--xpu-backend`, exactly as it
  already threads `opt` / `cpu` / `lto`.

### 3.2 One-command linked output — ALREADY WORKS; make it the taught default (fixes §2.2-2.7)
- 3.2.1 CONFIRMED: `cajeta --emit=exe --xpu-backend=cpu <entry> <src> <out>` produces a runnable
  binary in one command (auto OptiX stub, TLS + runtime objects, system libs, tree-shaken). The
  remaining work is **documentation + examples**, not the compiler:
  - 3.2.1a Fix `samples/tour/gpu/run-gpu.sh` (and any docs) to use `--emit=exe` as the default;
    demote the `--emit=obj` + manual-`clang` recipe to a clearly-labelled power-user appendix.
  - 3.2.1b Document the one-command form prominently in the XPU/CPU docs as THE way to build a
    `@Kernel` program standalone.

### 3.3 A shippable runtime + a published link line (fixes §2.3, §2.5)
- 3.3.1 As a power user doing a manual link, there is a single `libcajeta_runtime` (static
  archive) — or a documented, stable list of runtime objects — to link, not CMake-internal
  intermediate `.o` paths.
- 3.3.2 `cajeta --emit=obj` emits a **linker response file** (or a `compile_commands`-style
  manifest) listing exactly its objects + the required `-l` libraries, so the correct link line
  is generated, never guessed.

### 3.4 Optional natives never block a link (fixes §2.4)
- 3.4.1 As a developer building on a box without OptiX/CUDA/etc., the optional native symbols
  resolve via a shipped stub library (or weak symbols), so a CPU-only link never fails on a
  subsystem the program does not use.

### 3.5 Object packaging that links cleanly (fixes §2.6)
- 3.5.1 As a developer, `--emit=obj` either emits a single combined object/archive, or its
  response file (§3.3.2) enumerates every object including package-subdir ones — so no `find`
  glob is needed and a missing-user-object never masquerades as `undefined symbol: main`.

### 3.6 Diagnostics that point at the cause (fixes §2.6, §2.7)
- 3.6.1 As a developer, when the entry method's object isn't linked, the error explains "the
  entry method's object was not linked; link all objects under <build>" rather than a raw
  `undefined symbol: <entry>()` / `undefined symbol: main`.
- 3.6.2 The toolchain (linker path/version) is documented in one place, or the compiler invokes
  the linker itself (the §3.2 path makes this moot for the common case).

## 4. Acceptance themes
- `cajeta build` builds a `@Kernel` program from a manifest with no raw-compiler escape (§3.1).
- `cajeta --emit=exe --xpu-backend=cpu` yields a runnable binary in one command, tree-shaken,
  with no manual native-object hunting or stubbing (§3.2, §3.4).
- A manual link has one runtime library + a generated link line (§3.3), no glob/subdir traps
  (§3.5), and actionable diagnostics (§3.6).
- The `samples/matmul-kernel` build reduces from the ~8-step manual expedition to a single
  command.
