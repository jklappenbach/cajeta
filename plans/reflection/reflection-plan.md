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
`newInstance`/`invoke` → parameter introspection).

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

### Phase 1 — `Class<T>` + read-only introspection (REFL-1)
- [ ] REFL-1.1 `cajeta.reflect.Class<T>`: `getName`/`getShortName`/`getPackage`/
      `getModifiers`/`getSuperclass`/`getInterfaces`/`getFields`/`getMethods`/
      `getConstructors`, reading existing RTTI. No codegen changes.
- [ ] REFL-1.2 `cajeta.reflect.registry` — map canonical name → `Class<?>`,
      built from the data the compiler already emits.
- [ ] REFL-1.3 Per-class integer-indexed lookup tables (spec Strategy 1).
- [ ] REFL-1.4 `T.class` literal lowering → cached `Class<?>` instance; and
      `Object.getClass()` (one vtable→RttiGlobal hop). One `Class<?>` per type
      in static storage (spec § open question "Object.getClass() performance").
- **Accept:** `obj.getClass().getName()` and field/method enumeration return
  correct metadata; TDD via JIT tests over a fixture class.

### Phase 2 — per-class reflection adapters (REFL-2)
- [ ] REFL-2.1 Compiler-synthesized `T_reflect_getField` /
      `T_reflect_invokeMethod` / `T_reflect_newInstance` (switch over field/
      method index → direct load / direct call), reached through RttiGlobal.
      Foundation for Phases 3–5.

### Phase 3 — `Field` read/write (REFL-3)
- [ ] REFL-3.1 `Field.get`/`set` via the Phase-2 adapter.
- [ ] REFL-3.2 Typed primitive accessors (`getInt32`/`setInt64`/…) — no boxing.
- [ ] REFL-3.3 Visibility enforcement (throws on `private` without
      `@Reflectable` — see Phase 9 / decision D1).

### Phase 4 — `Method.invoke` + `Constructor.newInstance` (REFL-4)  ← unblocks the tour demo
- [ ] REFL-4.1 `Method.invoke` (vtable-hash lookup + arg marshalling) and the
      typed `invokeInt32`/… variants.
- [ ] REFL-4.2 `Constructor.newInstance` (allocate + run ctor + super chain).
- [ ] REFL-4.3 `Parameter` introspection (`getName`/`getType`/`getIndex`/
      `isThis`/annotations) — the "access down to the parameter" the tour asks for.
- [ ] REFL-4.4 Fiber-stack arg buffers for small calls (spec Strategy 6).
- **Accept:** the spec's "annotation-driven DI scan" example runs; **write the
  tour `ReflectionDemo`** (construct via `newInstance`, call via `invoke`,
  enumerate `getParameters()`).

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
