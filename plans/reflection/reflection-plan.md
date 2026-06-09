# Reflection implementation plan

Companion to [`docs/stdlib/Reflection.md`](../../docs/stdlib/Reflection.md).
That document is the **spec** (full `cajeta.reflect` API surface, performance
strategy, worked examples); this is the **plan** (phased, checkbox-tracked
build order). Every unit of work is a checkbox `- [ ]` with a stable id
(`REFL-N`); mark `- [x]` when shipped.

Source of the work: `plans/current-focus.md` → **Tour → reflection**. The tour
wants a demo showing "access down to the parameter, invoke a constructor, make
a call." That demo is **gated on this** — there is no reflection API today, so
there is nothing to demo. This plan builds the API; the tour `ReflectionDemo`
lands once Phases 1–4 are in (enough for `getClass` → `getField`/`getMethod` →
`heapInstance`/`invoke` → parameter introspection).

## Current state (baseline)

- **RTTI already exists.** The compiler emits a `#RttiGlobal` and `#VTable`
  per class (`src/cajeta/type/StructureMetadata.cpp:411`). It carries canonical
  name, fields (name/type/annotations/modifiers), methods (name/params/
  annotations/modifiers), parent(s), implemented interfaces, vtable pointer.
- **AspectModel consumes it at compile time** for pointcut matching. **No
  public runtime API reads it.**
- **No `cajeta.reflect` package** — confirmed: no `Class`/`Field`/`Method`/
  `Constructor`/`Parameter` types, no `__cajeta_reflect*` natives, no usage.
- **`.class` literals** parse today only as annotation arguments
  (e.g. `@Encoding(Enc.class)`); there is no `Class<T>` runtime type behind
  them yet. `instanceof` works (static-type match) but isn't reflection.

So: the infrastructure (RTTI) is paid for; the surface is missing. This is a
**large, multi-phase feature**, not a demo.

## Phases (from spec § "Implementation sequence")

### Phase 1 — `Class<T>` + read-only introspection (REFL-1)  ← foundation shipped 2026-06-08
- [x] REFL-1.1 `cajeta.reflect.Class` (non-generic v1): `getName`/`getFieldCount`/
      `getMethodCount`/`getParentCount`/`getModifierFlags`/`isPublic`/`isFinal`/
      `getFieldName`/`getFieldModifierFlags`/`getInstanceSize`, reading RTTI via
      `@Native __cajeta_rtti_*`. (getShortName/getPackage/getSuperclass/Field/
      Method *objects* still TODO — counts + per-index access landed.)
- [x] REFL-1.3 **RTTI redesigned to a fixed-offset header** (`cajeta.reflect.#Rtti`,
      same struct for every class) with pointer-referenced descriptor tables
      (`#FieldDesc`/`#MethodDesc`/`#ParameterDesc`) + packed-int32 modifiers +
      per-method signature hash. C mirrors in `cajeta_runtime.c`. The old
      variable-length inline blob couldn't be walked by generic natives; nothing
      read it at runtime, so the change was safe.
- [x] REFL-1.4 `getClass()` via the static factory `Class.of(obj)` (avoids a root
      `Object`→reflect bootstrap cycle); reaches the cached **`#ClassObject`**
      (one `Class` instance per type, `{Class#VTable, rtti}`) through a NEW
      **`classObject` slot in `#VTable`** (entries shifted 24→32; runtime offsets
      updated). `populate()` forward-declares vtable/rtti/classObject to break the
      vtable→classObject→rtti→vtable cycle; slot-0 resolves the real `Class#VTable`
      (stdlib compiles before user code) so virtual dispatch on `Class` lands.
- [x] **Accept met:** `Class.of(obj).getName()`/`getFieldCount()`/`getFieldName(0)`/
      `getModifierFlags()`/`getInstanceSize()` all correct — 6 JIT tests pass
      (`test/parser/ReflectionTests.cpp`).
- [ ] REFL-1.2 `cajeta.reflect.registry` (canonical name → `Class`) — for `forName`,
      deferred to Phase 8.
- [ ] REFL-1.5 `T.class` literal lowering (grammar `typeTypeOrVoid '.' CLASS` →
      address of `#ClassObject`) — TODO; `Class.of(obj)` covers the dynamic path.
- [ ] REFL-1.6 `Object.getClass()` proper (needs the `Object`→reflect edge; the
      `Class.of` factory is the interim) — TODO.
- [ ] REFL-1.7 `Class<T>` generic parameter + `Field`/`Method`/`Constructor`/
      `Parameter`/`Modifiers` objects (currently counts + per-index accessors).

### Phase 2 — per-class reflection adapters (REFL-2)  ← hybrid design, 2A+2B shipped 2026-06-08
Decision: **HYBRID** (fastest). Fields are data-driven (no per-class codegen);
methods/ctors use synthesized switch-over-index adapters reached through `#Rtti`.
- [x] REFL-2A **Data-driven field access**: `#FieldDesc` gained `i32 byteOffset` +
      `i64 typeFlags` (CajetaType TYPE_ID), computed from the instance struct
      layout. No per-class field accessor code is generated — REFL-3 `Field.get/set`
      reads offset+typeFlags from a generic native. Natives
      `__cajeta_rtti_field_offset` / `_type_flags`; `Class.getFieldOffset/
      getFieldTypeFlags`. (StructureMetadata getFieldStructType/emitFieldTable;
      C mirror split CajetaFieldDesc / CajetaParamDesc.)
- [x] REFL-2B **Synthesized invokeMethod adapter**: `#Rtti` gained slot 12
      `invokeAdapter` (+ slot 13 `newInstanceAdapter`, null until 2C). Per class,
      `void __cajeta_<canon>_reflect_invoke(ptr obj, i32 idx, ptr args, ptr ret)` —
      a switch over the method-list index that marshals scalar/pointer args from an
      8-byte-strided buffer and makes a DIRECT call (skips ctors, varargs,
      aggregate/sret shapes, and declaration-only callees so the JIT doesn't drag
      dead stdlib code). Forward-declared in `createRttiConstant`; body filled by
      `CajetaClass::emitReflectInvokeBody` in a post-Phase-1/2 pass (added to BOTH
      Compiler::compile AND JitTestHelper). Entry: `Class.invokeScalar0(o, idx)` /
      native `__cajeta_object_invoke_scalar0`; `Class.getMethodParamCount/
      getMethodName`. Fixed two latent bugs: uninitialized `Method::llvmFunction`
      (garbage ptr) and the reflected param count/table wrongly counting implicit
      `this`. 9/9 reflection + 61/61 regression green.
- [x] REFL-2C **Synthesized newInstance adapter** (shipped 2026-06-09): per-class
      `ptr __cajeta_<canon>_reflect_new(i32 ctorIndex, ptr args)` — switch over the
      constructor index that mirrors `heap T(...)` (CreatorRest): `__cajeta_alloc`
      + zero + install primary/secondary vtables + `patchVirtualTableDropFn` + run
      the chosen ctor, returning the instance. Fills `#Rtti` slot 13; added slots
      14/15 (constructorCount + constructor `#MethodDesc[]` table).
      `CajetaClass::getReflectConstructorList` (ctors live in the `methods` map,
      sorted by canonical → stable index) / `getOrCreateReflectNewDecl` /
      `emitReflectNewBody`. Entry `Class.heapInstance(idx)` (returns `#Object`,
      owned) + `getConstructorCount`/`getConstructorParamCount`; native
      `__cajeta_class_new0`. 12/12 reflection + 61/61 regression green.
- **REFL-2 (per-class adapters) COMPLETE.** Next: REFL-3 (`Field.get/set` over the
  REFL-2A data-driven offsets) and REFL-4 (typed `Method.invoke`/`heapInstance` arg
  marshalling over the 2B/2C adapters → unblocks the tour `ReflectionDemo`).

### Phase 3 — `Field` read/write (REFL-3)  ← typed accessors shipped 2026-06-09
- [x] REFL-3.2 **Typed primitive accessors, data-driven, no boxing**: `Class`
      `getInt32/setInt32/getInt64/setInt64/getBoolean/setBoolean(o, fieldIndex[, v])`
      backed by natives `__cajeta_field_get/set_i32/i64/bool` that load/store at the
      field's REFL-2A `byteOffset` — NO per-class accessor codegen (the hybrid's
      field path). Verified: roundtrips + a reflectively-set field is observed by a
      reflective invoke (16/16 reflection + 49/49 regression green). Note: int64
      literals need an explicit `(int64)` cast at the call site (cajeta idiom).
- [x] REFL-3.1 `Field.get`/`set` on a real `Field` object — DONE via REFL-4's
      object model: `Class.getField(idx) -> #Field`; `Field.getInt32/setInt32/...`.
      (`__cajeta_field_get_ref` native exists but the reference getter isn't
      surfaced yet — borrow-return-multi-param needs the receiver form.)
- [ ] REFL-3.3 Visibility enforcement (throws on `private` when `@Sealed`,
      decision D1) — TODO; today access is unchecked, caller must match the type.
- [x] **float/double field accessors** (shipped 2026-06-09): `getFloat32/setFloat32/
      getFloat64/setFloat64(o[, idx], v)` on both `Class` (index form) and `Field`
      (object form), backed by natives `__cajeta_field_get/set_f32/f64` (same
      byteOffset path as the integer accessors; FP values cross the native boundary
      in their own ABI registers — no bit-casting). 26/26 reflection green (3 new
      float roundtrip tests; fixture `User` gained `float32 ratio`/`float64 precise`
      at field idx 3/4, field count 3→5). Remaining primitive accessors (int8/int16/
      uint*) — TODO if needed.

### Phase 4 — `Method.invoke` + `Constructor.heapInstance` (REFL-4)  ← object model + marshalling shipped 2026-06-09
Decision (2026-06-09): **real `Field`/`Method`/`Constructor`/`Parameter` objects**
(the Java-like model), not per-index accessors. Shipped in two increments
(`61ed3e3` object model, marshalling next), 23/23 reflection + 49/49 regression green.
- [x] REFL-4.1 `Method.invoke` — `Method.invokeScalar(o)` (no-arg) and
      `invokeScalar(o, int64[] args)` (raw-packed args, one int64/user param) route
      through the REFL-2B per-class invoke adapter (direct call). Native
      `__cajeta_object_invoke_scalar(0)`. Typed `invokeInt32` (narrowed) and
      FP-return `invokeFloat32`/`invokeFloat64` variants shipped 2026-06-09 —
      FP returns read the adapter's `ret` buffer in the FP register via natives
      `__cajeta_object_invoke_f32/f64` (verified: invokeFloat32/64ReturnsRealValue,
      invokeInt32Narrows). Boxed/Object-return variants: TODO.
- [x] REFL-4.2 `Constructor.heapInstance` — `heapInstance()` / `heapInstance(int64[] args)`
      (renamed 2026-06-09 from `newInstance` — allocation site is now explicit;
      `stackInstance`/unsafe-placement reserved in the same namespace)
      route through the REFL-2C adapter (alloc + vtable + ctor). Native
      `__cajeta_class_new(0)`. The 2C adapter mirrors `heap T(...)` incl. vtable
      install; super-chain runs inside the ctor as usual.
- [x] REFL-4.3 `Parameter` introspection — `Method/Constructor.getParameter(i) ->
      #Parameter`; `Parameter.getName/getTypeName/getIndex`. Natives
      `__cajeta_rtti_param_name/type_*` (method/ctor table selectable). `isThis`
      excluded by construction (the implicit `this` is dropped from the tables);
      param annotations: TODO.
- **Object model**: new `cajeta.reflect` classes Field/Method/Constructor/Parameter;
  `Class.getField/getMethod/getConstructor(idx)` return owned objects.
- [ ] REFL-4.4 Fiber-stack arg buffers for small calls (spec Strategy 6) — TODO
      (today args are a caller-built `int64[]`).
- [ ] **Async bridge (D5)** — reflective invoke of an `async` method routing through
      the fiber pool — TODO (design alongside the typed-invoke refinement).
- [x] **Tour `ReflectionDemo`** (the milestone, shipped 2026-06-09): reflects over
      `tour.Counter` — `Class.of` → identity (name/field/method/ctor counts) →
      enumerate fields → walk methods down to each `getParameter(i)`
      (name + type) → construct via `Constructor.heapInstance()` → reflective
      `Field.setInt32` → reflective `Method.invokeScalar(obj, args)` (bumpBy(2),
      v 40→42). `samples/tour/.../lang/ReflectionDemo.cajeta`, wired into
      `Tour.cajeta` after `AnnotationsDemo`. Verified by running the tour with the
      demo hoisted early (output exact); restored to intended order after.
      NOTE: the full tour hangs later in `LinkedListDemo` (pre-existing, unrelated
      to reflection — it never reaches the reflection demo in list order).

### Phase 5 — `MethodHandle.bindCallSite` (REFL-5)
- [ ] REFL-5.1 Per-signature-shape concrete `MethodHandle` subclasses,
      synthesized lazily + cached per shape (spec Strategy 4).

### Phase 6 — Annotations (REFL-6)
- [ ] REFL-6.1 `cajeta.reflect.Annotation` over the annotation metadata RTTI
      already carries for AspectModel.

### Phase 7 — Generic retention (REFL-7)
- [ ] REFL-7.1 Augment RTTI with per-instantiation type-argument substitutions.
- [ ] REFL-7.2 `TypeParameter` / `TypeArgument` API reading them.

### Phase 8 — `Class.forName` + `@Retained` (REFL-8)
- [ ] REFL-8.1 Link-time process registry; `@Retained` keeps stripped classes.
- [ ] REFL-8.2 `forName` / `forNameOrNull` (perfect-hash over canonical name).

### Phase 9 — access control (REFL-9)
- [ ] REFL-9.1 `@Reflectable` (private-member opt-in).
- [ ] REFL-9.2 `UnsafeReflect` escape hatch + construction-time audit log.

### Phase 10 — package / annotation queries (REFL-10)
- [ ] REFL-10.1 `classesInPackage` / `classesAnnotated` (registry scan).

### Phase 11 — constant-fold known reflection (REFL-11)
- [ ] REFL-11.1 Fold `T.class.getField("lit").get(x)` to direct access when all
      inputs are statically known and visibility permits (spec Strategy 5).

**Deferred (post-v1, separate efforts):** compile-time codegen escape hatch
(`@JsonSerializable`-style), dynamic class generation / proxies, plugin /
hot-reload class loading, reflection-emit.

## Decisions (resolved 2026-06-08)

- **D1. Private-access default → DEFAULT-OPEN + `@Sealed` opt-out.** Reflection
  can read/write/invoke any member by default; a class-level `@Sealed` directive
  (and an optional global compiler flag) bars reflective access to private
  members. **How:** the per-class synthesized adapter (Phase 2) omits the
  private-member cases when the class is `@Sealed`, and the Field/Method RTTI
  carries a visibility flag so the reflect API can throw `IllegalAccessException`
  for a sealed private member instead of silently missing it.
  Original lean: restrictive `@Reflectable` opt-in — rejected as too much
  framework friction.

- **D2. Generic retention — N/A (templates, not generics).** Cajeta
  monomorphizes; per-instantiation type info already exists. No erasure, no
  `-noGenericRetention` flag framing. See memory `templates-not-generics`.

- **D3. `forName` → `Optional<Class>`.** No throwing/null pair; one method
  returns `Optional`. (`forName(name) -> Optional<Class<?>>`.)

- **D4. Generic *methods* — read from vtable/IR, not erased.** Templates aren't
  generics; the per-instantiation type info is in the vtable / IR. Phase 7 reads
  it rather than re-emitting Java-style type-parameter RTTI. See memory.

- **D5. Async/reflective → DESIGN THE BRIDGE NOW.** Reflective invoke of an
  `async`-marked method must compose with async (even though async is still a
  placeholder). Plan: reflective invoke routes through the same fiber-pool
  dispatch a non-reflective async call uses — `Method.invoke` on an async method
  returns the same awaitable the static call would, and runs on the caller's
  fiber otherwise. Worked out alongside Phase 4 (REFL-4).

## Notes

- Scope is large (11 phases). The **tour demo** the user asked for needs only
  Phases 1–4. A reasonable first milestone: ship Phases 1–4 behind a small
  fixture, write `ReflectionDemo`, then continue.
- The value-type-rvalue-arg codegen fix (this session,
  `CajetaClass::invokeMethod`) is unrelated but adjacent — reflective arg
  marshalling (REFL-4.1) will pass value-type args by pointer and should reuse
  the same spill discipline.
