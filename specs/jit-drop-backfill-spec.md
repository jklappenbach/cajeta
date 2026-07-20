# jit-drop-backfill — spec

## 1 Definition

### 1.1 Purpose
Make the JIT materialize every drop thunk a compiled program references, so
non-trivial programs run and debug under `cajeta dap` / `cajeta jit-run`.
Spun out of run-config-ergonomics 7.4.

### 1.2 Problem
Drop thunks — `__cajeta_stack_<Type>_drop` and `__cajeta_<Type>_drop` — are
synthesized lazily (`CajetaClass::getOrCreateStackDropFunction`,
`getOrCreateDropFunction`): the definition is emitted into the type's own
module only when a consumer's codegen drops a value of the type. A consumer
module can hold a bare extern declaration for a thunk whose synthesis never
fired — notably for generic value-type instantiations created indirectly
during stdlib codegen, which the obligation set cannot enumerate.

The AOT pipeline repairs this with a backfill pass (`Compiler.cpp` ~2244):
scan every module for undefined `__cajeta[_stack]_<type>_drop` declarations
and synthesize exactly those. The JIT pipeline (`CajetaJitHost.cpp
buildJit`) has no equivalent, so LLJIT `initialize` fails with `Symbols not
found: [__cajeta_stack_tour_lang_Shape_drop, ...]` on any program large
enough to leave a declaration dangling. Reproduced 2026-07-20 on
samples/tour over stdio DAP; small programs synthesize everything
in-session, which is why all debug-tests fixtures pass.

### 1.3 Constraints
- The AOT backfill's observable behavior is preserved: same symbols
  synthesized, same module placement (the type's own module), same mangling.
- The JIT backfill runs before cross-module linking in `buildJit`, alongside
  the REFL-2 reflect-thunk pass, which repaired the same failure class.
- One implementation of the scan+synthesize logic serves both pipelines; the
  drop-symbol mangling exists in exactly one place.

### 1.4 Non-goals
- Auditing other lazily-synthesized symbol families (reflect thunks are
  already covered by REFL-2; new families get their own scope when observed).
- Restructuring lazy drop synthesis itself (e.g. defining at declaration
  site); the lazy scheme stays.
- Prettifying LLJIT "Symbols not found" output.

## 2 Shared backfill pass

### 2.1 Requirements
The AOT backfill logic is extracted into a single reusable pass: given a set
of modules and the canonical type map, find every undefined
`__cajeta_stack_<mangled>_drop` / `__cajeta_<mangled>_drop` declaration and
invoke the owning class's `getOrCreateStackDropFunction` /
`getOrCreateDropFunction`. Mangling is shared with the synthesis sites.

### 2.2 Use cases
- 2.2.1 As the AOT compiler, when an incremental build loads cached .bc with
  dangling drop declarations, then the pass synthesizes those thunks and the
  build links — exactly as today.
- 2.2.2 As the JIT host, when a compiled program leaves any drop declaration
  undefined in any module, then the pass synthesizes it before linking and
  LLJIT materialization succeeds.
- 2.2.3 As a maintainer, when the drop-name mangling changes, then AOT
  backfill, JIT backfill, and synthesis stay in agreement because they share
  one implementation.

## 3 JIT pipeline integration

### 3.1 Requirements
`buildJit` runs the shared pass after codegen quiescence, static
initializers, and REFL-2, and before donor modules are linked into the
primary module. Both drop families are covered. Programs that already
materialize are unaffected.

### 3.2 Use cases
- 3.2.1 As a developer, when I launch `cajeta dap` on samples/tour with
  entry `tour.Tour.main` and a breakpoint in main, then the launch reaches a
  `stopped` event instead of failing LLJIT initialize.
- 3.2.2 As a developer, when I run `cajeta jit-run` on samples/tour, then
  the program executes to completion.
- 3.2.3 As a developer, when I debug a trivial program (any existing
  debug-tests fixture), then behavior is unchanged.

## 4 Regression coverage

### 4.1 Requirements
A cajeta_debug_test fixture pins the backfill: a program small enough for
the suite but constructed to reference drop thunks its own codegen never
synthesizes (generic value-type instantiations dropped only via stdlib
paths, mirroring `Optional<SelectResult<int32>>` /
`Pair<int32,Pair<int32,int32>>` from the tour failure).

### 4.2 Use cases
- 4.2.1 As CI, when the JIT backfill regresses, then the fixture fails with
  the "Symbols not found" signature rather than the gap surfacing only on
  sample-sized programs.
