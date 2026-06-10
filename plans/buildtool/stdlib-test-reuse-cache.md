# Stdlib Test-Reuse Cache — Correctness Fix

## Status (2026-06-10) — speculative reuse + fallback + per-class binding reset. HANDOFF TO LINUX.

The "deep refactor" §0 below called for IS DONE and the picture has changed: the
resolution/emit split landed, and on top of it this session added (a) speculative
reuse with a fresh fallback and (b) a per-class reset of cached module-bound llvm
bindings. All gated behind `CAJETA_STDLIB_REUSE=1`; production `--emit` and the
default (reuse-off) test path are byte-unchanged by construction (the hazard gate
is never armed unless the JIT harness arms it). Work is shifting to **Linux** to
stabilize the parallelism/scaling gains with less toolchain churn, then back to
Windows for final compatibility. Sections §0–§7 below are retained as history;
this section supersedes the "blocked" status.

### What landed this session (branch `feature/windows-dylib-slimdown`)
1. **Speculative reuse with fresh fallback** (committed, `3d1d8871`).
   - `Compiler`: `s_reuseHazardArmed` flag (`setReuseHazardArmed`/`isReuseHazardArmed`)
     + `struct ReuseHazardAbort` — a non-`Exception` control signal.
   - `TemplateInstantiator.cpp` (~line 402, after ALL instantiation cache lookups,
     before any build): `if (Compiler::isReuseHazardArmed() && emitOwner != module)
     throw ReuseHazardAbort{};` — a primed/cached instantiation returns earlier and
     never trips it.
   - `JitTestHelper.cpp`: parse/compile wrapped in a `for(;;)` retry loop. First
     attempt binds the shared context + arms; on `ReuseHazardAbort` it scrubs
     (`restoreBaseline`), unbinds, disarms, sets `reuseStdlib=false`, and re-runs the
     whole parse on a fresh, isolated `Compiler`. Sources written/prescanned once.
     `CAJETA_STDLIB_REUSE_TRACE=1` logs each fallback.
2. **Per-class reuse-binding reset** (committed this session — see below).
   - Root cause: `CajetaClass` persistently caches MODULE-BOUND llvm pointers —
     `llvmDropFunction`, `llvmStackDropFunction`, `llvmVirtualTableGlobal`,
     `llvmRttiGlobal`, and the `interfaceVTables` / `staticFieldGlobals` /
     `secondaryVTables` maps — plus each `Method::llvmFunction`. These are lazily
     materialized into whatever module is active at first use; under reuse that's a
     per-test USER module (freed/merged after the test), so the cached pointer
     outlives it → "references a function/global in another module" on the NEXT
     reusing test.
   - Fix: `CajetaClass::captureReuseBaseline()` snapshots them at stdlib prime;
     `CajetaClass::restoreReuseBaseline()` resets any that drifted back to the
     snapshot between tests (pointer assignment only — freed pointers are never
     dereferenced). `Method::setLlvmFunction()` added for the method-fn reset.
     Wired into `StdlibReuseCache`: capture in `ensurePrimed`, restore in
     `restoreBaseline`. Context-bound `StructType*`/layout fields are deliberately
     NOT snapshotted (the reuse context is shared → they're module-independent).

### Validation (single-process, `CAJETA_STDLIB_REUSE=1`, 113-test set)
Filter: `StreamTests.*:StreamTerminalTests.*:StreamIntermediateTests.*:StreamFoldTests.*:StreamClassTTests.*:HashMapTests.*:HashMapStreamTests.*:TemplateBasicTests.*:StaticFieldTests.*`
- Split only (before this session): **20 pass / 93 fail** (cross-module refs).
- + speculative fallback (`3d1d8871`): **96 pass / 17 fail**. The 17 = 5
  `StreamIntermediateTests.mapOr*` + all 12 `StaticFieldTests`, all cross-module
  refs from persistent `CajetaClass` cached pointers (drop wrappers / static-field
  globals) — i.e. exactly what (2) targets.
- + per-class reset: **PENDING at handoff.** Validation re-running; 0 cross-module
  errors observed through `StreamIntermediateTests` (was riddled with them before),
  but the decisive suites (`mapOr*`, `StaticFieldTests`) run later in the process —
  **first Linux task is to re-run this and confirm 0 failures.**
  - Repro: `CAJETA_STDLIB_REUSE=1 CAJETA_STDLIB_VERIFY=1 CAJETA_STDLIB_REUSE_TRACE=1 ./cajeta_test --gtest_filter='<above>'`

### Remaining risk — out-of-graph caches
The per-class reset covers everything reachable from the stdlib class/method graph.
It does NOT cover module-bound caches that live OUTSIDE that graph (named in the
original design): the **`FileStream` singleton**, **`ComponentDescriptor::singletonGlobal`**,
and **`CajetaModule::sourceFileConstants`**. If a full-suite reuse run shows
cross-module refs in tests that touch file I/O / components / source-file string
constants, snapshot+reset those the same way (`capture/restoreReuseBaseline` is the
template). This is the likely next failure class at full-suite scale.

### Next steps (Linux pickup, in order)
1. Build (Linux: stock clang/gcc + `libLLVM.so` — no mingw fork needed). Re-run the
   113-test validation; confirm the per-class reset takes 17 → 0.
2. **Full-suite single-process reuse run** (~3597 tests): (a) surface any out-of-graph
   caches; (b) measure the **fallback rate** — the fraction that actually reuses
   (~3–4s) vs falls back (~15s). That rate is the real "tens of minutes" number and
   tells us whether reuse is worth flipping on.
3. Gate the **method-template path** too: `MethodTemplateInstantiator.cpp` has the
   same `emitOwner` redirect (~lines 191–198, 370) but does NOT throw
   `ReuseHazardAbort` — add the identical guard so method-template-over-user-type
   tests fall back rather than (potentially) contaminate.
4. Latent: `Method::emitAroundWrapper` (Method.cpp:1823/1845) does not swap to the
   emit module — only bites if an `@Around`-advised method is reparented via
   instantiation; close before default-on.
5. Then §5 below: reuse-vs-no-reuse differential (identical pass/fail sets) + ≥2
   shard orderings, then flip default-on with a `CAJETA_STDLIB_REUSE=0` opt-out.
6. **Scaling (the Linux motivation):** drive the suite with `ctest -j` + per-worker
   memory caps (cgroups) instead of the bespoke PowerShell runner. Windows data:
   **W=16 stable, ~73 min, reuse off** (3597 tests; 46 memory-pressure flakes
   recovered on serial retry; 12 genuine pre-existing reds — 4 `Xpu*Device`, 1
   `Net`, 2 `Phase14`, 5 `ZoneId` crashes, the last unrelated to reuse). **W=32
   OOM-restarted the box** — that, plus the can't-rebuild-a-running-exe file lock,
   is why scaling work moves to Linux.

### Critical files (this session's changes)
- `src/cajeta/compile/Compiler.h` / `.cpp` — `s_reuseHazardArmed`, `ReuseHazardAbort`.
- `src/cajeta/type/TemplateInstantiator.cpp` — hazard throw (~line 402).
- `src/cajeta/type/CajetaClass.h` / `.cpp` — `ReuseBindingBaseline`,
  `capture/restoreReuseBaseline`.
- `src/cajeta/method/Method.h` — `setLlvmFunction`.
- `test/jit/JitTestHelper.cpp` — retry loop, prime capture, `restoreBaseline` reset.

---

## (historical) Original Design B status

Status: **blocked on a deep refactor — Design B as specified is insufficient.**
The reuse cache + shared-context infra are built and validated for a ~5× per-test
speedup, but **gated OFF** (`CAJETA_STDLIB_REUSE=1`). Owner: stdlib-reuse.

## 0. Findings (2026-06-09) — why Design B is insufficient, and the real fix

Added a gated diagnostic, `StdlibReuseCache::verifyPristine` (env
`CAJETA_STDLIB_VERIFY=1`, JitTestHelper.cpp): at prime it snapshots every cached
stdlib llvm function's instruction-count + every global name; after each reusing
test's codegen it reports any baseline function that MUTATED/grew and how many
functions/globals accumulated. A full-suite serial reuse run under it produced the
exact diagnosis:

- **Two coupled leak modes**, both forced by keeping the cached stdlib in the
  per-test codegen list — which is REQUIRED, because stdlib-template instantiations
  emit their IR during their body walk and are owned by the stdlib module, so they
  only materialize if the stdlib is codegen'd each test:
  1. *baseline-constructor growth* — re-codegen re-emits into baseline
     `Class::Class(this)` ctors; verify shows `Math::Math` / `Object::Object` /
     `Clock::Clock` / … growing **+2 instructions every test** (2→4→6→…) until the
     duplicated init corrupts and crashes (dominant full-suite crash; a corrupted
     `Clock::Clock` surfaced as `ZoneId: unknown time zone`). It is NOT
     `generateStaticInitializers` (re-entry-guarded, CajetaClass.cpp:1943) — it is
     re-codegen appending blocks to a persistent `llvmFunction` after
     `restoreBaseline` resets the Method's codegen state.
  2. *user-type instantiation accumulation* — `Stream<test.M>` etc. accumulate in
     the cached stdlib (verify: `+23 fns +13 globals` for one stream test),
     referencing the previous test's freed user types → cross-suite heap corruption.
- **Why ownership-redirect (Design B) can't be done cheaply:** the instantiation
  choke point re-parses + walks the template body, and the instantiation's methods
  cross-reference and **emit IR during that walk** (prototype-on-reference), before
  any reparent point. An args-based attribution (own the instantiation in a
  user-type argument's module) DOES make `Stream<test.M>` resolve and keeps the
  baseline byte-PRISTINE — but the methods still emit into the stdlib during the
  walk (reparent then asserts "already emitted before reparent" for ~all methods),
  i.e. incoherent dual-ownership.

**Real fix (multi-day):** separate each `Method`'s RESOLUTION module (needs the
stdlib file's imports/substitution) from its EMIT module, so instantiation IR can
target the user module from the first prototype; OR clone the stdlib per test and
rebind every cached `llvm::Function*`/`GlobalVariable*` on the persistent
`Method`/`CajetaClass` objects. Until one is done, reuse stays gated OFF and the
default (no-env) test path is the authoritative, correct path. The args-based
attribution experiment and the verify diagnostic are retained; the reparent asserts
are restored (`ownerModule == module` in the current tree, so they're inert).

Sections 2–5 below describe the original (insufficient) Design B plan, kept for
reference.

Make the JIT unit-test stdlib-reuse cache **fully correct** so it can be enabled
by default. The cache parses + codegens the embedded stdlib ONCE per process
(into a shared `llvm::LLVMContext`) and reuses it across every test, collapsing
the ~14 s/test stdlib parse+codegen to a one-time prime. The one unsolved hazard
is a test whose USER code instantiates a STDLIB TEMPLATE over a USER type (e.g.
`Stream<test.M>`, `ArrayList<test.M>`): today the instantiation is owned by — and
codegens into — the persistent stdlib module, polluting the shared cache across
tests (heap corruption / "Referencing global in another module").

**Design B (recommended, reuse-gated):** make a user-triggered stdlib-template
instantiation's `CajetaClass` + `Method`s **owned by the codegen-site (user)
module** instead of the template's (stdlib) module. The instantiation's vtable /
RTTI / bodies then emit into the user module; cross-references to the stdlib base
route through the already-battle-tested `ensureFunctionInModule` /
`ensureGlobalInModule` machinery (the normal user→stdlib direction). The
persistent stdlib module is never mutated, so restore/clone is trivial. Gated on
`Compiler::getSharedContext() != nullptr`, so production `--emit=exe/obj/cja` is
bit-for-bit unchanged. (Design A — per-test clone + rebind every cached
module-bound `llvm::*` pointer — was rejected: it cannot be made complete because
module-bound caches live outside the `CajetaClass`/`Method` object graph
(`FileStream` singleton, `ComponentDescriptor::singletonGlobal`,
`CajetaModule::sourceFileConstants`).)

---

## 1. Foundation (already landed, gated off)

Describes the infra the fix builds on. No work items — context only.

- [x] 1.1 Shared process-global `LLVMContext` plumbing in `Compiler`
      (`s_sharedContext`, `s_sharedInitialized`, `activeContext`,
      `setSharedContext`); production path unchanged when null.
- [x] 1.2 Baseline snapshot/restore of all global registries
      (`CajetaType::capture/restoreBaseline`,
      `CajetaModule::capture/restoreBaseline`).
- [x] 1.3 `StdlibReuseCache` in `test/jit/JitTestHelper.cpp`: prime once,
      restore-to-baseline per test, clone-for-merge donor.
- [x] 1.4 Flag-keyed reuse (`stdlibReusable`) — only tests whose
      bounds/overflow/liveSet flags match the primed defaults reuse; others fall
      back to the fully-isolated fresh-Compiler path.
- [x] 1.5 Reuse gated OFF behind `CAJETA_STDLIB_REUSE=1`; default path validated
      green (32/32 on StaticField + Template suites).

## 2. Instantiation ownership redirect (core change)

Definition: in `CajetaClass::instantiateInternal` (`TemplateInstantiator.cpp`,
class path ~628–678 and interface path ~369–495), when reuse mode is active and a
**stdlib** template is being instantiated from a **non-stdlib** codegen site,
register the new instantiation `CajetaClass` and reparent its `Method`s to the
**current codegen module** (the user module) instead of the template's module —
*before* `generatePrototype()` emits anything. Keep the entire re-parse + AST walk
on the template `module` (name resolution must use the template file's
imports/package); only ownership of the emitted IR moves.

### 2.1 TDD

- [ ] 2.1.1 Add `StdlibReuseInstantiationTests` (new `test/jit` suite) with a
      RED test `streamOverUserTypeThenUnrelated`: Test A instantiates
      `Stream<test.M>` (user `test.M`), exercises add/count/iterate; Test B is
      unrelated; both under `CAJETA_STDLIB_REUSE=1`. Today this heap-corrupts /
      fails to materialize — assert it PASSES.
- [ ] 2.1.2 RED test `arrayListOverUserType`: instantiate `ArrayList<test.M>`
      over a user type, with `poisonFreeEnabled` + `dropChainValidateEnabled` on.
- [ ] 2.1.3 RED invariant probe `pristineStdlibModuleUnchanged`: capture the
      stdlib `llvm::Module` function+global count (and an IR hash) at prime;
      assert it is identical after a template-instantiating reusing test.

### 2.2 Deliverables

- [x] 2.2.1 `#include "../compile/Compiler.h"` in `TemplateInstantiator.cpp`.
- [x] 2.2.2 Compute `ownerModule` after `instCanonical` is built (~line 333):
      `module` by default; when `Compiler::getSharedContext()` **and**
      `module == CajetaModule::getStdlibModule()` **and** a distinct
      `CajetaModule::getCurrentCodegenModule()` exists, use that codegen module.
- [x] 2.2.3 Register the instantiation under `ownerModule` **before the walk**:
      `make_shared<CajetaClass>(ownerModule, …)` (628); write into
      `ownerModule->getStructures()[instCanonical]` (replacing 641); set
      `CajetaModule::getStructureToModule()[instCanonical] = ownerModule` before
      the walk so self-referential bodies resolve via the structureToModule
      branch (lines 349–359).
- [x] 2.2.4 Reparent methods after `setClassBody` (669) and before
      `generatePrototype` (670): `inst->setModuleForInstantiation(ownerModule)`
      then loop `inst->getMethods()` setting each `Method`'s module, guarded by
      `assert(m->getLlvmFunction() == nullptr)` (nothing emitted yet).
- [x] 2.2.5 Leave the template `module` driving `synthesizePreamble`,
      `pushTypeSubstitution`, `setActiveModule`, `CajetaLlvmVisitor visitor(module)`,
      `fromContext(..., module)` — unchanged.
- [x] 2.2.6 Apply the identical reparent to the **interface-template branch**
      (369–495) before `ifInst->generatePrototype()` (488), registering `ifInst`
      under `ownerModule`.
- [x] 2.2.7 **(added during impl) Shared-context user-type collision fix.**
      Separate from the template path: `getOrCreateLlvmType` (CajetaType.cpp:1072)
      does `StructType::getTypeByName` first, so two tests both declaring
      `test.S` reuse the first's already-bodied struct → double `setBody` / stale
      layout → crash after several tests. Fix in `StdlibReuseCache`: record the
      stdlib struct names at prime; per reusing test, after the JIT module is
      built (separate bitcode-roundtripped context), `setName("")` on every
      non-baseline identified struct in the compile-context module
      (`clearTransientStructNames`), so the next test's `getTypeByName` misses and
      builds fresh. Called from `compile()` before return (reuse only).

### 2.3 Acceptance

- [x] 2.3.1 `StreamTests` (stdlib templates incl. over user types) 9/9,
      `HashMapTests` 15/15, combined Stream+Template+HashMap **44/44** GREEN under
      `CAJETA_STDLIB_REUSE=1` (was heap-corrupting). Dedicated named tests (2.1)
      still to be added; full differential is §5.
- [x] 2.3.2 No "Referencing global in another module!"; no heap corruption
      (`0xC0000374`) on the validated suites.
- [ ] 2.3.3 With reuse OFF, byte-identical behavior — confirm via the §5
      differential (gated on a non-null shared context, so expected).

## 3. Reparent accessors (Method / CajetaClass)

Definition: add the minimal setters that move IR ownership, mirroring the
existing `Method::setParentForInstantiation`. No cached `llvm::*` pointer is
touched — that burden belongs to the rejected Design A.

### 3.1 TDD

- [ ] 3.1.1 Unit assertion (within 2.1's harness): after instantiation, every
      value of `inst->getMethods()` returns `getModule() == ownerModule`.

### 3.2 Deliverables

- [x] 3.2.1 `Method::setModuleForInstantiation(CajetaModulePtr m) { module = m; }`
      (Method.h, near `setParentForInstantiation` ~369).
- [x] 3.2.2 `CajetaClass::setModuleForInstantiation(CajetaModulePtr m) { module = m; }`
      (CajetaClass.h).

### 3.3 Acceptance

- [ ] 3.3.1 Setters compile and are used only on the instantiation reparent path;
      no other call sites.

## 4. Harness invariant + simplification

Definition: enforce the cache-pristineness invariant in the reuse harness and,
once green, drop the now-unnecessary "append stdlib to the per-test codegen list"
workaround (user-triggered instantiation bodies now live in the user module and
are reached by the existing fixpoint over `compiler->getModules()`).

### 4.1 TDD

- [ ] 4.1.1 Harness asserts the stdlib-module IR hash is unchanged after EVERY
      reusing test (not just the targeted ones) when run under a debug/assert flag.

### 4.2 Deliverables

- [ ] 4.2.1 Add the stdlib-module-hash invariant check to `StdlibReuseCache`
      (compute at prime; compare in `restoreBaseline`).
- [ ] 4.2.2 After 2.x + 5.x are green, remove the `codegenModules.push_back(
      stdlibCache.stdlibModule)` append (JitTestHelper.cpp ~414–417); keep the
      stdlib clone-merge (it still supplies static stdlib defs).
- [ ] 4.2.3 `restoreBaseline`'s `stdlibModule->getStructures() = baselineStructures`
      becomes belt-and-suspenders (nothing mutates stdlib) — keep but comment.

### 4.3 Acceptance

- [ ] 4.3.1 Invariant never trips across the full reusing suite.
- [ ] 4.3.2 Removing the codegen-list append leaves all of §2 + §5 green.

## 5. Validation & default-on flip

Definition: the differential gate that lets reuse become the default. Run the
full JIT suite with and without reuse; pass/fail sets must match, then flip the
default.

### 5.1 TDD

- [ ] 5.1.1 `sameNamedUserTypeDifferentLayout`: two tests each declare `test.M`
      with a DIFFERENT field set and both instantiate `ArrayList<test.M>`; assert
      both produce correct values and the pristine stdlib StructType set is
      unchanged (the exact prior heap-corruption case).
- [ ] 5.1.2 `sharedBothSiteInstantiation`: a built-in-arg instantiation stdlib
      itself uses (e.g. `ArrayList<int32>`) — assert the merged primary module has
      exactly ONE external definition of the mangled function (dedup intact).
- [ ] 5.1.3 `pureReuseRegression`: a reusing test that instantiates no stdlib
      template — confirms the redirect didn't perturb the common path.

### 5.2 Deliverables

- [ ] 5.2.1 Full-suite differential run: entire JIT suite with
      `CAJETA_STDLIB_REUSE=1` vs without; capture both pass/fail sets.
- [ ] 5.2.2 Run the suite in ≥2 shard orderings under reuse (cross-test
      contamination only shows under reordering).
- [ ] 5.2.3 Flip the default: make reuse ON by default (remove/invert the env
      gate) once 5.2.1–5.2.2 are clean; keep an opt-OUT escape hatch
      (`CAJETA_STDLIB_REUSE=0`).
- [ ] 5.2.4 Update memory `stdlib-test-reuse.md` and any test-runner docs.

### 5.3 Acceptance

- [ ] 5.3.1 Reuse vs non-reuse pass/fail sets are IDENTICAL across the full suite.
- [ ] 5.3.2 No order-dependent failures across the tested shard orderings.
- [ ] 5.3.3 Measured per-test wall-clock drops ~16 s → ~3–4 s after prime; full
      suite wall-clock materially reduced.
- [ ] 5.3.4 Default-on landed with an opt-out; production `--emit` paths untouched.

## 6. Risks (ranked) & detection

- [ ] 6.1 **A method emits into the template module before reparent** (a lazy
      `getLlvmFunctionType`/`generatePrototype` firing during `visitClassBody`).
      Detect: the `assert(getLlvmFunction()==nullptr)` guard (2.2.4) + the
      stdlib-module-hash invariant (4.1.1).
- [ ] 6.2 **Incomplete reparent** (a synthesized ctor/getter left at
      `module=stdlib`). Detect: post-reparent assert all methods'
      `getModule()==ownerModule` (3.1.1); else `verifyModule` fires
      "Referencing global in another module!".
- [ ] 6.3 **Self-referential template body fails mid-walk** because the cache
      entry moved. Mitigated by dual pre-registration (2.2.3); detect with a
      `List<T>{ List<T> next; }`-over-user-type test.
- [ ] 6.4 **A body ref to a stdlib helper bypassing `ensure*`** (raw foreign
      `Function*`/`Global*` into the user module). Detect: `verifyModule`. Low —
      same risk production multi-module already covers.
- [ ] 6.5 **Interface-template branch missed** (2.2.6). Detect: a test
      instantiating a stdlib templated interface over a user type; assert stdlib
      module hash unchanged.
- [x] 6.6 **Same-named user type collides across tests in the shared context**
      (`getOrCreateLlvmType`/`getTypeByName` returns a stale bodied struct).
      Surfaced as a crash after several reusing tests in a suite. Fixed by
      `clearTransientStructNames` (2.2.7); detect: any suite that re-declares a
      class name across tests (HashMapTests was the repro — now 15/15).

## 7. Critical files

- [ ] 7.1 `src/cajeta/type/TemplateInstantiator.cpp` — owner-module computation +
      reparent (class ~628–678, interface ~369–495).
- [ ] 7.2 `src/cajeta/method/Method.h` / `Method.cpp` — `setModuleForInstantiation`;
      `module->getLlvmModule()` emission (994/998/1024) is the ownership lever.
- [ ] 7.3 `src/cajeta/type/CajetaClass.h` / `CajetaClass.cpp` —
      `setModuleForInstantiation`; globals/drops/static-fields emit via `module`.
- [ ] 7.4 `src/cajeta/type/StructureMetadata.cpp` — cross-module vtable/RTTI
      resolution (510–544); the machinery Design B relies on.
- [ ] 7.5 `test/jit/JitTestHelper.cpp` — reuse harness: codegen list (~414–417),
      clone-merge (~444–448), `restoreBaseline` (~231–236), stdlib-hash invariant.
