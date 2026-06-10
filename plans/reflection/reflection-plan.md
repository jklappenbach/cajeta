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
- [x] REFL-1.2 `cajeta.reflect.registry` (canonical name → `Class`) — SHIPPED as
      Phase 8 (REFL-8.1 process-wide `g_cajeta_classes` registry + REFL-8.2
      `forName`), where it was always deferred to. Closed here.
- [x] REFL-1.7 (object model) **`Field`/`Method`/`Constructor`/`Parameter` objects
      — SHIPPED in Phase 4** (REFL-4 object model; all five live in
      `runtime/src/cajeta/reflect/`). `Class.getField/getMethod/getConstructor(idx)`
      return owned objects; the old counts + per-index accessors stay as the fast
      path. Template introspection objects (`TemplateParameter`/`TemplateArgument`)
      landed in Phase 7. **Still open below:** `Class<T>` templating + a `Modifiers`
      object.
- [x] REFL-1.5 `T.class` literal lowering — **SHIPPED**. Grammar
      `typeTypeOrVoid '.' CLASS` → `ClassLiteralExpression` (Expression.h/.cpp),
      lowered to the address of the named type's `#ClassObject` global (the
      Class<T> instance), typed `Class<T>`. The named type's text is captured at
      parse time (the ANTLR context is freed before codegen) and resolved by name
      from canonicalMap at resolveTypes. `loadIfLValue` carve-out treats the
      #ClassObject address as the Class reference (not a slot to load through).
- [x] REFL-1.6 `Object.getClass()` proper — **SHIPPED**. Synthesized in
      `MethodCallExpression::generateCode` as `__cajeta_object_get_class(obj)` (the
      same native backing `Class.of`), returning `Class<?>`. Gated on a
      class-instance receiver, no args, and the class not declaring its own
      `getClass` (a user override wins). No `getClass` slot is added to `Object`,
      sidestepping the `Object`→reflect bootstrap cycle. Intercepts BEFORE the
      generic invokeMethod dispatch.
- [x] REFL-1.7 (remaining) `Class<T>` templatized + `Modifiers` object —
      **SHIPPED**. `cajeta.reflect.Class` is now `final class Class<T>` (phantom T:
      no T-typed field, so every instantiation shares one layout/vtable). The
      canonical `Class<?>` is force-instantiated before user parse
      (`ensureClassWildcardInstantiated`, called from both `Compiler::compile` and
      `JitTestHelper`) so its method bodies are emitted and every `#ClassObject`
      embeds its shared vtable. Concrete `Class<Foo>` receivers (from `Foo.class`)
      dispatch via the **template-origin alias hash** — same as wildcard receivers
      — because their runtime vtable is always the shared `Class<?>#VTable`
      (CajetaClass.cpp dispatch). `getModifiers()` on Class/Field/Method/Constructor
      returns a `Modifiers` object (`runtime/src/cajeta/reflect/Modifiers.cajeta`)
      wrapping the packed flag bits.

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
- [x] REFL-3.3 Visibility enforcement (shipped 2026-06-09, decision D1):
      reflection is DEFAULT-OPEN; a class-level `@Sealed` annotation bars
      reflective access to its PRIVATE members only. `@Sealed` is recorded as the
      synthesized `REFLECT_SEALED` (0x100) class modifier (derived from the
      annotation in visitClassDeclaration — NOT the Java `sealed` keyword), so it
      rides into the RTTI header `modifiers` word for free. Enforcement is twofold:
      (a) the synthesized invoke/newInstance adapters OMIT private cases for a
      sealed class (compile-time hardening); (b) the reflect API (Field/Method/
      Constructor + the `Class` index-form accessors) calls a `*_blocked` native
      (`__cajeta_reflect_field/method/ctor_blocked` = class sealed && member
      private) and throws `cajeta.reflect.IllegalAccessException` (new,
      RecoverableException) before touching the member — so callers distinguish
      "sealed off" from "no such member". Verified (6 tests): private field/method/
      ctor all throw (object + `Class` index forms), public field of a sealed class
      stays readable, and a private field of a NON-sealed class is readable
      (default-open). Public/protected members and non-private access are
      unaffected.
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
      invokeInt32Narrows). Reference-return `invokeObject` shipped 2026-06-09 —
      reads the adapter's `ret` buffer as a pointer via `__cajeta_object_invoke_obj`
      and returns an owned `#Object` (drop-tracked); ownership follows the invoked
      method's signature (a method returning `heap T` transfers ownership; a method
      returning a borrow must not be reflected this way — documented on
      `Method.invokeObject`). Verified: invokeObjectReturnsReference (reflective
      `make()` → real Cell, field reads 42). Primitive boxing into a wrapper
      Object: `Method.invokeBoxed` SHIPPED 2026-06-09 — boxes primitive returns
      into the `cajeta.lang` W1 wrappers (Int32/Int64/Float32/Float64/Boolean),
      reference→`invokeObject`, void→null, unsupported primitive→
      `UnsupportedReflectionException`. Uses the existing
      `CajetaMethodDesc.returnType` string via new native
      `__cajeta_rtti_method_return_kind` (no compiler change). `Field.getBoxed` /
      `Class.getBoxed` SHIPPED too (W5b, `__cajeta_rtti_field_kind`) — primitive
      fields box; reference fields throw `UnsupportedReflectionException`
      (ownership-unsafe without a borrow-return surface). Wrapper family +
      design: `plans/lang/wrapper-types-plan.md` (W1 + W5 shipped; W2-W4 =
      remaining wrappers, which auto-expand the boxable set).
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
- [x] REFL-4.4 Fiber-stack arg buffers for small calls (spec Strategy 6) —
      DONE 2026-06-09. Small-arity overloads `invokeScalar`/`invokeInt32(o, a0[,
      a1[, a2]])` pass raw args as discrete `int64` params to natives
      `__cajeta_object_invoke_scalar1/2/3`, which assemble the adapter's
      8-byte-strided arg buffer on their own C stack frame (= the calling fiber's
      stack) — no heap `int64[]`, no count header to skip. The heap-`int64[]`
      path stays for large/dynamic arg lists (Java's shape). Verified:
      methodInvokeStackArg1 (addId(5)→15), methodInvokeStackArgsMulti
      (sum2(7,9)→116, sum3(7,9,4)→120). Scope: scalar/int32 return (the spec's
      `invoke(obj, a, b)` example); FP-return/reference-return stack-arg siblings
      are a mechanical extension of the same `buf` pattern (read the ret buffer as
      float/double/pointer) — TODO if a typed stack-arg FP/obj surface is wanted.
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

### Phase 6 — Annotations (REFL-6)  ← COMPLETE: names + scalar/list arg values, all owners (incl. parameters)
- [x] REFL-6a **Annotation NAME reflection.** New `cajeta.reflect.Annotation`
      object (locator: rtti + ownerKind/ownerIndex/subIndex/index) exposing
      `getName()`/`toString()`. Enumeration + `hasAnnotation(canonicalName)` +
      `getAnnotationCount`/`getAnnotationName(i)`/`getAnnotation(i)` added to
      **Class, Field, Method, Constructor, Parameter** (every annotatable
      owner). One generic native family backs all owners —
      `__cajeta_rtti_annotation_{count,name_len,name_into}(rtti, ownerKind,
      ownerIndex, subIndex[, annIdx])` over a single C resolver
      `cajeta_annotation_list` (ownerKind 0=class 1=field 2=method 3=ctor
      4=method-param 5=ctor-param). **Filled the method/ctor emission gap**:
      `#MethodDesc` gained `i16 annotationCount, ptr annotations` (appended;
      existing readers keep offsets), emitted in `emitMethodTable` +
      `emitConstructorTable` (class/field/param names already rode the RTTI).
      **Names format**: a bare `@Foo` serializes to canonical `"code.Foo"`
      (single-identifier annotation names default to the `code` package; a
      qualified `@p.Bar` → `"p.Bar"`); `hasAnnotation` matches the exact
      canonical string. Any annotation identifier works (none need predeclaring).
      **Fixed a latent double-count bug** (`FieldDeclaration::updateParent`):
      field annotations were added to `annotationList` twice (the
      `Annotatable(set)` ctor AND the `addAnnotationInstance` loop), so a field's
      `getAnnotationCount()` came back doubled — invisible before 6a because
      nothing read a property's annotationList at runtime (DI/JSON use
      `findAnnotation`→annotationInstances). Now constructs the property with an
      empty set and lets the instance loop be the single source. Tests:
      ReflectionTests.{classAnnotationName, classHasAnnotation,
      annotationObjectName, classNoAnnotationsZeroCount, fieldAnnotationName,
      methodAnnotationName, constructorAnnotationName, parameterAnnotationName,
      multipleClassAnnotations}.
- [x] REFL-6b **Annotation ARGUMENT values** (shipped 2026-06-09). The
      `annotations` pointer in every owner descriptor (class slots 3/4,
      `#FieldDesc`/`#MethodDesc`/`#ParameterDesc`) was repointed from a bare
      `[N x i8*]` name array to `[N x #AnnotationDesc]` — each row
      `{ ptr name, i16 argCount, ptr args }`, with `args` an
      `[M x #AnnotationArgDesc]` `{ ptr name, i32 kind, i64 i64Val, ptr strVal,
      i8 boolVal, i32 listCount, ptr listData }` of the captured values. **No
      RTTI struct-shape bump** for the owner descriptors: with opaque pointers
      the LLVM field type stays `ptr`, so only the pointee (and the C mirror's
      interpretation) changed; the REFL-6a name natives now read `desc.name`.
      Values come from the args-carrying `AnnotationInstance`,
      paired to the REFL-6a `annotationList` by canonical name in a new
      `emitAnnotationArray`/`emitAnnotationArgArray`. `Annotation` gained
      by-index accessors (`getArgCount`/`getArgName`/`getArgKind`/`getArgInt`/
      `getArgBool`/`getArgString`) and kind-checked by-key getters (`getInt`/
      `getString`/`getBool`/`getClassRef`), with the unnamed single-arg form
      (`@Order(2)`) read via key `"value"` (mirrors `AnnotationInstance.findArg`).
      Natives `__cajeta_rtti_annotation_arg_{count,kind,int,bool,name_len,
      name_into,str_len,str_into}` over a shared `cajeta_annotation_arg` resolver.
      Tests (12):
      ReflectionTests.{classAnnotationIntArg, classAnnotationNamedStringArg,
      classAnnotationUnnamedStringArg, classAnnotationBoolArg,
      classAnnotationClassRefArg, annotationArgByIndex,
      annotationArgWrongKindFallback, fieldAnnotationArg, methodAnnotationArg,
      annotationMultipleNamedArgs, annotationNoArgsZeroCount}. Tour
      ReflectionDemo + Counter (`@Tracked(2)`, `@Metric("count")`) read arg
      values. NOTE: a `.class` annotation arg must reference a SEPARATE declared
      type via a NEUTRAL annotation (`@Refers(Marker.class)`) — a self-reference
      (`@T(Self.class)` on `Self`) hangs the reflect class-object build, and an
      active annotation (`@Encoding(...)`) invokes its own subsystem.
- [x] REFL-6b.1 **Parameter annotation argument values** (shipped 2026-06-10).
      Extracted the visitor's `parseAnnotationInstance` (+ `classifyLiteral`/
      `trimWs`) into a shared `src/cajeta/asn/AnnotationParser.{h,cpp}` so
      `FormalParameter::fromContext` captures full typed instances (was: names
      only via the legacy set). Parameters now populate `annotationInstances`
      aligned with `annotationList` (constructed with an EMPTY set + the
      `addAnnotationInstance` loop, avoiding the same double-count trap the field
      path had); `emitParameterTable` emits their arg values like every other
      owner. Test: ReflectionTests.parameterAnnotationArg (`@Bound(min=5)` on a
      parameter → `getInt("min")==5`).
- [x] REFL-6b.2 **List-valued argument values** (shipped 2026-06-10).
      `#AnnotationArgDesc` gained `i32 listCount, ptr listData`; the *List kinds
      emit their elements (`[N x i64]` Int64List / `[N x i8*]` StringList /
      `[N x i8]` BoolList). New natives `__cajeta_rtti_annotation_arg_list_{count,
      int,bool,str_len,str_into}`; `Annotation` gained `getArgListCount`/
      `getArgListInt`/`getArgListBool`/`getArgListString` (by index) + a public
      `getArgIndex(key)` to locate a list argument by name. Tests:
      ReflectionTests.{annotationStringListArg, annotationIntListArg,
      annotationBoolListArg}. Tour Counter `@Labels({"hot","live"})` read
      end-to-end. **Phase 6 COMPLETE.**

### Phase 7 — Template reflection (REFL-7)  ← shipped 2026-06-10
**NOT "generic retention".** Cajeta has TEMPLATES, not erased generics — there is
nothing to *retain* because nothing is erased. Each instantiation (`Box<int32>`)
is its own monomorphized `CajetaClass` that already carries `getTypeParameters()`
(the `<T>` decls) and `getTypeArguments()` (the concrete types). Phase 7 just
EXPOSES that to reflection. See [[never-call-it-generics]].
- [x] REFL-7.1 **RTTI carries template params + args** (2026-06-10). `#Rtti`
      gained 4 appended slots (16–19): `i16 templateParamCount`, `ptr
      templateParams` (`[N x #TemplateParamDesc]` `{ ptr name, i16 boundCount,
      ptr bounds, i8 isNonType, ptr nonTypePrimitive }`), `i16 templateArgCount`,
      `ptr templateArgs` (`[N x i8*]` canonical type names). Emitted from
      `structure->getTypeParameters()` / `getTypeArguments()` via
      `emitTemplateParamTable`/`emitTemplateArgArray`. C mirror
      `CajetaTemplateParamDesc` + 4 new `CajetaRtti` fields in lock-step. Arg
      names render via `CajetaType::toCanonical()` — the SAME rendering as
      field/parameter type names (so `int32` reads back as `"int32"`).
- [x] REFL-7.2 **`TemplateParameter` / `TemplateArgument` API** (2026-06-10,
      decision: FULL OBJECT MODEL). New `cajeta.reflect.TemplateParameter`
      (`getName`/`isNonType`/`getNonTypeName`/`getBoundCount`/`getBound(i)`) and
      `TemplateArgument` (`getTypeName`; `getType()` → `Class` DEFERRED, throws
      `UnsupportedReflectionException` until the Phase-8 forName registry exists).
      `Class` gained `getTemplateParameterCount`/`getTemplateParameter(i)`/
      `getTemplateArgumentCount`/`getTemplateArgument(i)`/`isTemplateInstantiation`.
      Natives `__cajeta_rtti_template_{param_count,param_name_*,param_is_nontype,
      param_nontype_*,param_bound_count,param_bound_*,arg_count,arg_name_*}`.
      Tests: ReflectionTests.{templateArguments, templateParameters,
      nonTemplateClassZero}. **`getType()` Class resolution is the one piece
      gated on Phase 8** (name→Class needs the registry).

### Phase 8 — `Class.forName` + `@Retained` (REFL-8)  ← shipped 2026-06-10
- [x] REFL-8.1 **Process-wide class registry** (2026-06-10). The compiler emits,
      per class, an `llvm.global_ctors` entry (in `StructureMetadata::populate`,
      at the single `#ClassObject` definition site) calling
      `__cajeta_register_class(canonicalName, classObject)`. The runtime keeps a
      growable, process-lifetime table (`g_cajeta_classes`) keyed by canonical
      name (strdup'd keys, last-writer-wins). The same `global_ctors` path clinit
      already uses, honored by both JIT (`initialize()`) and AOT (C runtime). No
      stripping exists yet, so every compiled class is registered. **`@Retained`**
      is recorded as the `REFLECT_RETAINED` (0x200) class modifier (mirrors
      `@Sealed`/`REFLECT_SEALED`, derived from the annotation in
      `visitClassDeclaration`) — advisory until the AOT linker's stripping pass
      lands, which will key off this bit to keep an otherwise-unreferenced class
      in the registry.
- [x] REFL-8.2 **`Class.forName(String) -> Optional<Class>`** (2026-06-10,
      decision D3: one Optional-returning lookup, no throwing/null pair). Backed
      by `__cajeta_class_for_name(int8[] nameBytes)` — single-parameter so it can
      return a `Class` borrow (a multi-param `@Native` returning a borrow is
      rejected, CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM); the name length comes
      from the `int8[]` count header. Linear scan over the registry (perfect-hash
      is a later optimization; correctness first). Tests:
      ReflectionTests.{forNameResolvesClass, forNameAbsentEmpty, forNameRoundTrip,
      forNameStdlibClass}. **`TemplateArgument.getType()` now wired** — resolves
      the argument's canonical type name via `forName`; a class-typed argument
      (`Box<Widget>`'s `Widget`) returns its `Class`, a primitive argument
      (`Box<int32>`'s `int32`) has no `Class` and throws
      `UnsupportedReflectionException`. Tests: templateArgGetTypeResolvesClass,
      templateArgGetTypePrimitiveThrows.
- [x] **Root-cause fix — template instantiations now auto-extend `Object`**
      (2026-06-10). Surfaced while running the Phase-7 template-reflection tests
      for the FIRST time (they had been written but never executed): `Class.of()`
      on ANY template instantiation crashed because `Class.of(b)` silently
      compiled to `null` (no call emitted) — a `Box<int32>` argument failed to
      match the `of(Object)` parameter. Cause: the visitor injects an implicit
      `extends Object` for any class with an empty `extends` clause
      (CajetaLlvmVisitor.h), but `TemplateInstantiator` builds the instantiation's
      extends list straight from the parse tree and bypassed that injection, so
      every instantiation had ZERO parents and was not recognized as `<: Object`.
      Fix: mirror the visitor's rule in `TemplateInstantiator` (add
      `cajeta.lang.Object` when the instantiation's extends list is empty). This
      was a PRE-EXISTING bug (not Phase 8) that blocked all reflection on template
      instantiations; Phase 8's `getType` was the first caller to hit it.

### Phase 9 — access control (REFL-9)  ← N/A under decision D1 (dropped 2026-06-10)
- [x] **REFL-9.1 `@Reflectable` — DROPPED.** The spec's `@Reflectable` was a
      private-member *opt-in*, which only makes sense under a restrictive default.
      Decision **D1** inverted that: reflection is DEFAULT-OPEN with `@Sealed` the
      sole opt-*out* (shipped in REFL-3.3). With everything reflectable by default,
      a per-class "yes, reflect me" opt-in is redundant. User confirmed: drop it.
- [x] **REFL-9.2 `UnsafeReflect` + audit log — DROPPED.** `UnsafeReflect` is a
      visibility-*bypass* escape hatch; under D1 there is almost nothing to bypass
      (only `@Sealed`-private), and its defining feature — a construction-time
      audit warning — has no sink (`cajeta.thread.log` doesn't exist; only
      `cajeta.xpu.core.Thread`). User confirmed: drop it. If a `@Sealed`-private
      bypass is ever wanted, it returns as a focused follow-on, not this phase.

### Phase 10 — package / annotation queries (REFL-10)  ← shipped 2026-06-10
- [x] REFL-10.1 `Class.allClasses()` / `classesInPackage(String)` /
      `classesAnnotated(String)` — registry scan returning `#Class[]` (borrows of
      the process-lifetime `#ClassObject`s; the array owns its buffer, not its
      elements). Two new natives expose the REFL-8 registry as an indexable list:
      `__cajeta_class_count()` and `__cajeta_class_at(i)`; the package/annotation
      filtering runs in cajeta over `getName()` (package = canonical name up to the
      last `.`) and the existing `hasAnnotation(canonical)`. `classesInPackage` /
      `classesAnnotated` are two-pass (count matches, then fill a right-sized
      array). Array length is `arr.count()` (the structural i64 accessor), NOT a
      `.length` property. Tests: ReflectionTests.{allClassesFindsRegistered,
      classesInPackageFilters, classesAnnotatedFilters}.
- [x] **Registry-correctness fix (2026-06-10):** REFL-8 registered EVERY class,
      but the "handful of stdlib classes parsed before `cajeta.reflect.Class`"
      (String, Object, …) get a `#ClassObject` whose slot 0 (`Class#VTable`) is
      NULL — not reflectively dispatchable (see REFL-1.4). `Class.of` never
      returns these, so earlier phases never hit it; `allClasses()` did, and the
      virtual `getName()` crashed on the null vtable. Fix is twofold:
      (a) `StructureMetadata::populate` only emits the registration ctor when slot
      0 is non-null; (b) a new post-pass `CajetaClass::finalizeClassObject()` (run
      alongside `emitReflectInvokeBody`/`emitReflectNewBody` in BOTH
      `Compiler::compile` and `JitTestHelper`, after the whole stdlib incl. `Class`
      is built) patches a deferred `#ClassObject`'s slot 0 to the real
      `Class#VTable` and registers it. Net: String/Object/etc. are now genuinely
      reflectable AND registry-discoverable; `forName("cajeta.lang.String")` keeps
      working (now safe to call accessors on the result).

### Phase 11 — constant-fold known reflection (REFL-11)  ← shipped 2026-06-10
- [x] REFL-11.1 **Fold `Class.of(<ident>).<accessor>(...)` to direct access**
      (decision: fold the dynamic `Class.of(...)` entry, NOT the `.class` literal —
      at the time `.class` lowering (REFL-1.5) wasn't done, and the spec lists
      reflective constant-folding as a v1 non-goal; the user chose the `Class.of`
      fold. REFL-1.5 has since shipped but the `.class`-literal fold remains a
      deliberate non-goal). In
      `MethodCallExpression::generateCode`, when the receiver is
      `cajeta.reflect.Class.of(<ident>)` and `<ident>`'s static type is a **`final`
      class** K — the exact-type guard: only `final` makes K provably its own
      runtime type through an identifier binding, so the static layout/metadata the
      fold bakes in equals what the runtime reflective native would read — fold:
      - integer metadata (no args): `getFieldCount`/`getMethodCount`/
        `getParentCount`/`getModifierFlags` → i32 `ConstantInt`; `getInstanceSize`
        → i64 `ConstantInt` (`getTypeAllocSize`, mirroring the rtti).
      - typed primitive field load: `getInt32`/`getInt64`/`getFloat32`/`getFloat64`
        `(obj, <literal idx>)` → a direct `(obj + byteOffset)` load, byte-identical
        to the `__cajeta_field_get_*` native (same `getElementOffset` arithmetic).
        Guards: field type matches the accessor, `obj` is the SAME identifier, and
        access is permitted — a `@Sealed`-private field is NOT folded so the runtime
        `IllegalAccessException` path is preserved.
      `Class.of` is identified codegen-free via the inner receiver: of()'s return
      type isn't resolvable without generating the (elided) call, but the receiver
      `Class` resolves by name (`CajetaType::of("Class")` → `cajeta.reflect.Class`).
      A non-`final` receiver declines the fold and runs the runtime path unchanged.
      IR-verified: `fold()` emits `getelementptr i8 + off`/`load` (no `Class::of`,
      no `__cajeta_vtable_lookup`); a non-final twin keeps the full chain.
      **Fix (2026-06-10):** the field-load fold initially read 0 — `obj`'s
      `IdentifierExpression::generateCode` returns the local's *alloca* (the l-value
      slot holding the object pointer), so GEP'ing off it indexed into the stack
      slot, not the object. Now load the pointer through the slot
      (`CreateLoad(ptr, objSlot)`) before the byte-offset GEP, matching the normal
      argument-lowering coercion. Tests:
      ReflectionTests.{foldFinalFieldCount, foldFinalFieldLoad, foldDeclinesNonFinal,
      foldDeclinesSealedPrivate}. **Follow-on (mechanical):** String-returning folds
      (`getName`/`getFieldName`) — skipped because the un-folded result is an owned
      `#String` (heap alloc), so folding to a constant isn't a pure win; the
      `getBoolean` field-load variant; and a `final`-construction-expression
      receiver (`Class.of(heap K())`) in addition to the identifier form.

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

- **D2. "Retention" — N/A (templates, NOT generics).** Cajeta monomorphizes;
  per-instantiation type info already exists, nothing is erased, so there is
  nothing to "retain" — Phase 7 only EXPOSES the template params/args each
  instantiation already carries. Never call cajeta's parametric types
  "generics" (that implies erasure). See [[never-call-it-generics]],
  [[templates-not-generics]].

- **D3. `forName` → `Optional<Class>`.** No throwing/null pair; one method
  returns `Optional`. (`forName(name) -> Optional<Class<?>>`.)

- **D4. Templated *methods* — read from vtable/IR, not erased.** The
  per-instantiation type info is in the vtable / IR. Phase 7 reads it rather than
  re-emitting Java-style erased type-parameter RTTI. See memory.

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
