# Reflection — session state / resume handoff (2026-06-08)

Snapshot for resuming the reflection build after a machine reboot. All changes
are **uncommitted in this clone** (`cpp/cajeta-two`, branch `main`); a reboot
preserves the working tree. Companion docs: `reflection-plan.md` (phase
checklist, decisions D1–D5) and `docs/stdlib/Reflection.md` (spec).

## TL;DR

**REFL-1 (reflection foundation) is DONE and was VERIFIED** — all 6 JIT tests in
`test/parser/ReflectionTests.cpp` passed cleanly when the host was calm:
`Class.of(obj)` → `getFieldCount()`=3, `getMethodCount()`>0, `getName()`="test.User",
`isPublic()`, `getFieldName(0)`="id", `getInstanceSize()`>0. The full runtime chain
`obj → vtable.classObject → rtti → fixed-layout blob` works end to end.

The 0/6 results were NOT environmental RAM thrashing (an earlier guess) and NOT a
regression. **Root cause (found 2026-06-08): the `cajeta-llvm` fork was left on the
wrong branch with a clang/lib commit skew.** cajeta-two needs the custom SPIR-V
intrinsics that live ONLY on branch `cajeta-spirv`; the fork had been checked out on an
upstream PR branch (`pr/spirv-coopmatrix-aggregate-ptr`, missing them) and its LLVM libs
rebuilt there while `clang-23` stayed at an older commit. The embedded runtime bitcode
(produced by the skewed clang) then corrupted on link, so `linkRuntime →
Linker::linkModules → IRLinker` SIGSEGV'd/hung during stdlib build — failing EVERY JIT
test. Fix: `git -C cajeta-llvm checkout cajeta-spirv && (cd build-cajeta && ninja clang
lld llvm-config)`, then rebuild cajeta-two. After that, **REFL-1 is 6/6 and the full
RTTI/vtable/inheritance/destructor regression set is 89/89 green** (incl.
`DestructorChainTests.multiInheritanceBothParentsRun`, previously misfiled as flaky). See
memory `llvm-fork-toolchain-skew`.

## First thing to do when resuming

```
# 0. CONFIRM THE TOOLCHAIN FIRST (see memory llvm-fork-toolchain-skew).
git -C ~/code/cpp/cajeta-llvm branch --show-current      # MUST be: cajeta-spirv
~/code/cpp/cajeta-llvm/build-cajeta/bin/clang-23 --version | head -1   # commit must match fork HEAD
# if wrong branch / skewed: git -C ~/code/cpp/cajeta-llvm checkout cajeta-spirv
#   && (cd ~/code/cpp/cajeta-llvm/build-cajeta && ninja clang lld llvm-config)

cd ~/code/cpp/cajeta-two
./build.sh                                  # full rebuild (CMake re-globs reflect/ + test)
./build/test/cajeta_test --gtest_filter='ReflectionTests.*'    # expect 6/6 PASS
# regression — each JIT test ~8.5s, so batch with a long timeout:
./build/test/cajeta_test --gtest_filter='RttiSmokeTests.*:VirtualTableSmokeTests.*:DynamicDispatchTests.*:InheritanceSmokeTests.*:VirtualDropDispatchTests.*:ClassDropTests.*:SealedClassTests.*:DestructorChainTests.*'
```
Note: `DestructorChainTests.multiInheritanceBothParentsRun` was once thought "flaky" —
that was the toolchain skew, now resolved; it passes. Do NOT exclude it.

## Files changed (what each does)

- `runtime/src/cajeta/reflect/Class.cajeta` (NEW) — the `cajeta.reflect.Class`
  surface. `final` class; one field `pointer rtti`. Reached via static factory
  `Class.of(Object o)` (`@Native __cajeta_object_get_class`). Methods: getName
  (`#String`, built from RTTI name bytes via the `heap int8[]` + `nameInto` +
  `heap String(#out,len)` pattern), getFieldCount/getMethodCount/getParentCount,
  getModifierFlags/isPublic/isFinal, getFieldName/getFieldModifierFlags,
  getInstanceSize. Backed by `@Native private static __cajeta_rtti_*`.
- `src/cajeta/type/StructureMetadata.{h,cpp}` — **RTTI redesigned** to a
  FIXED-offset header `cajeta.reflect.#Rtti` (same LLVM struct for every class)
  with pointer-referenced descriptor tables `#FieldDesc`/`#MethodDesc`/
  `#ParameterDesc` + packed-int32 modifiers + per-method sig hash. New emitters:
  emitCString/emitCStringArray/packModifiers/get*StructType/emit*Table. `#VTable`
  gains a `classObject` slot (index 4; entries moved 4→5). `populate()` forward-
  declares vtable+rtti+classObject (breaks the vtable→classObject→rtti→vtable
  cycle) and emits a cached per-class `#ClassObject = {Class#VTable, rtti}`.
  classObject slot-0 resolves the real `cajeta.reflect.Class` vtable by
  **lookup-only** via `getStructureToModule()` (do NOT force-build it — calling
  writeVirtualTable/getOrCreateDropFunction mid-populate re-enters linkRuntime
  and corrupts the module; stdlib builds Class before user code so the lookup
  resolves).
- `src/cajeta/type/CajetaClass.{h,cpp}` — `llvmClassObjectGlobal` field +
  get/set. Secondary (multi-inheritance) vtable builder updated: entries index
  4→5 and a classObject slot added (points at THIS class's #ClassObject so
  getClass reports the dynamic/most-derived type through a parent view).
- `runtime/native/cajeta_runtime.c` — vtable offsets: added
  `CAJETA_VTABLE_CLASSOBJECT_OFFSET 24`, `ENTRIES_OFFSET 24→32`. New natives +
  C mirror structs (CajetaRtti/CajetaFieldDesc/CajetaMethodDesc):
  `__cajeta_object_get_class`, `__cajeta_rtti_field_count/method_count/
  parent_count/modifiers/alloc_size/name_len`, `__cajeta_rtti_name_into`,
  `__cajeta_rtti_field_name_into/field_name_len/field_modifiers`. MUST stay in
  lock-step with the LLVM struct shapes in StructureMetadata.cpp.
- `src/cajeta/buildtool/Resolver.cpp` — added `"cajeta.reflect"` to kStdlibRoots.
- `src/CMakeLists.txt` — added `${CAJETA_STDLIB_ROOT}/cajeta/reflect` to
  `CAJETA_STDLIB_DIRS` (after lang/stream). Without this the stdlib embed skips
  Class and `import cajeta.reflect.Class` resolves to null (compile-time crash).
- `test/parser/ReflectionTests.cpp` (NEW) — 6 JIT tests.
- `plans/reflection/reflection-plan.md` — REFL-1 marked done; D1–D5 resolved.

## Key facts a resumer needs

- Decisions (resolved): D1 default-open + `@Sealed` opt-out; D3 forName→Optional;
  D5 design async bridge in REFL-4; templates≠generics (memory).
- `#VTable` layout now: {i16 version, i16 count, ptr parent, ptr drop_fn,
  ptr classObject, [N x {i64 hash, ptr fn}] entries}. Runtime offsets:
  parent=8, drop=16, classObject=24, entries=32.
- `#RttiGlobal` is now the fixed `cajeta.reflect.#Rtti` header (C mirror
  `CajetaRtti` in cajeta_runtime.c) — see struct field order there.
- Ownership: plain return = borrow (no `#`), so `Class.of()` returning the
  static `#ClassObject` is a borrow (never freed). `#String` returns transfer
  ownership (getName).
- Class accessors dispatch VIRTUALLY (final ≠ static dispatch here), which is
  why #ClassObject slot-0 MUST be a real Class#VTable, not null.

## Remaining REFL-1 follow-ons (small)

- REFL-1.5 `T.class` literal lowering (grammar `typeTypeOrVoid '.' CLASS` →
  address of `#ClassObject`; hook in `Expression.cpp` PrimaryExpression::fromContext,
  needs a `ClassLiteralExpression` AST node). `Class.of(obj)` covers dynamic path.
- REFL-1.6 `Object.getClass()` proper (needs the Object→reflect edge; Class.of is interim).
- REFL-1.7 `Class<T>` generic param + real `Field`/`Method`/`Constructor`/
  `Parameter`/`Modifiers` objects (today: counts + per-index accessors).

## Next phases (REFL-2 → REFL-4, the user opted to push through 1–4)

- REFL-2: compiler-synthesized per-class adapters `T_reflect_getField/setField/
  invokeMethod/newInstance` (switch over index → direct load/store/call), reached
  through RTTI. `@Sealed` omits private cases. Foundation for 3–4.
- REFL-3: `Field.get/set` + typed primitive accessors + `@Sealed` enforcement.
- REFL-4: `Method.invoke` (reuse `__cajeta_vtable_lookup` + arg marshalling;
  per-method sig hash already in #MethodDesc) + `Constructor.newInstance`
  (rtti slot0 allocationSize → malloc + memset + store vtable + run ctor) +
  `Parameter` introspection + fiber-stack arg buffers + async bridge (D5).
  Then write the tour `ReflectionDemo` and wire into `samples/tour/.../Tour.cajeta`.
