---
id: reflect-Class
applies-to: [cajeta/reflect/Class]
title: Class — the reflection entry point
description: Obtain Class via Class.of / forName, then read fields, methods, templates, and registry by index; handles are never-freed process-lifetime borrows.
---

# `cajeta.reflect.Class`

The entry point to `cajeta.reflect`: a runtime handle to one declared type, backed
by the compiler-emitted RTTI. **Start here** — `Field`, `Method`, `Constructor`,
`TemplateArgument`, `Annotation`, `Modifiers` are all reached *from* a `Class`.

## Get one — never construct it

You do not `heap Class(...)`. Obtain a handle one of three ways:

- `Class.of(obj)` — the dynamic type of a live object (the spec's `o.getClass()`).
- `Class.forName(name)` — resolve a canonical name; returns `Optional<Class<?>>`.
- `T.class` — the statically-known type (typed as `Class<T>`).

```cajeta
import cajeta.reflect.Class;

User u = heap User();
Class<?> c = Class.of(u);
int32 n = c.getFieldCount();      // 5 for the User above
String name #= c.getName();        // "test.User"  (owned #String, you free it)
```

`Class.forName` returns an `Optional` (decision D3 — no throwing/null pair); empty
when the name is not in the process registry. Primitives (`"int32"`) are not classes
and never resolve.

```cajeta
import cajeta.reflect.Class;
import cajeta.lang.Optional;

Optional<Class<?>> c = Class.forName("test.User");
if (c.isPresent()) {
    int32 fields = c.get().getFieldCount();
}
```

## Ownership / lifecycle — the load-bearing fact

**A `Class` handle is a process-lifetime BORROW you never free.** There is exactly
one cached instance per declared type, emitted next to the type's RTTI; `Class.of` /
`forName` / `T.class` / registry queries all hand back that same constant.
`Class<?> c = Class.of(u);` registers **no** drop entry — do not free it, do not put
it where something will free it.

Caveat from this same rule: there is **no** bounded `forName<T>` returning
`Optional<Class<T>>` — a concrete `Optional<Class<Shape>>` would synthesize an
element-drop that faults on the global handle. Use the unbounded `Class<?>` form, or
`heapInstance<T>` (below) whose result genuinely is owned.

Other returns DO transfer ownership (marked `#`): `getName()` and the other
string-returning accessors return an **owned `#String`**; `getField/getMethod/
getConstructor/getTemplate*` return owned member objects; `heapInstance` returns an
owned `#Object`; the registry arrays (`allClasses`, `classesInPackage`,
`classesAnnotated`, `subtypes`) return an owned `#Class<?>[]` **whose elements are
borrows** — freeing the array frees no `Class`.

## Index-based field get/set (REFL-3)

Fields are addressed by **declared-field index** (0-based, same index used by
`getFieldName`/`getFieldOffset`). There is one typed accessor pair per primitive —
`getInt32/setInt32`, `getInt64/setInt64`, `getBoolean/setBoolean`,
`getFloat32/setFloat32`, `getFloat64/setFloat64`:

```cajeta
Class<?> c = Class.of(u);
c.setInt32(u, 0, 41);
int32 v = c.getInt32(u, 0);   // 41
```

The accessor's type **must match the field's type** — a mismatch is unchecked
(no conversion, no diagnostic). Validate with `getFieldTypeFlags(index)` if unsure.
For a boundary-crossing object value rather than a primitive, use
`getBoxed(o, index)` (boxes a primitive into its `cajeta.lang` wrapper; throws
`UnsupportedReflectionException` for reference/unwrapped fields), or step up to the
`Field` object via `getField(index)`.

## Counts + per-index accessors

Everything is count-then-index, no name lookup at this level:

- Fields: `getFieldCount`, `getFieldName(i)`, `getFieldOffset(i)` (`-1` for a
  static field), `getFieldModifierFlags(i)`, `getFieldTypeFlags(i)`, `getField(i)`.
- Methods: `getMethodCount`, `getMethodName(i)`, `getMethodParamCount(i)`,
  `getMethod(i)`; `Class.invokeScalar0(o, i)` is the minimal no-arg invoke (full
  marshalling lives on `Method`).
- Constructors: `getConstructorCount`, `getConstructorParamCount(i)`,
  `getConstructor(i)`, `heapInstance(i)` (no-arg construct; returns `null` if `i`
  is not a no-arg ctor).
- Templates (REFL-7 — cajeta has real templates, not erased generics):
  `getTemplateParameterCount`/`getTemplateParameter(i)`,
  `getTemplateArgumentCount`/`getTemplateArgument(i)`, `isTemplateInstantiation()`.
- Annotations (names only here; arguments live on `Annotation`):
  `getAnnotationCount`, `getAnnotationName(i)`, `getAnnotation(i)`,
  `hasAnnotation(name)`.
- Identity/modifiers: `getName()`, `getInstanceSize()`, `getModifierFlags()`,
  `getModifiers()`, `isPublic()`, `isFinal()`, `getParentCount()`.

## Registry queries (REFL-10/12)

Static enumeration over the process-wide class registry (built at startup): every
returned array element is a borrow.

```cajeta
Class<?>[] svc #= Class.classesAnnotated("code.Service");   // exact canonical name
Class<?>[] kinds #= Class.subtypes<Shape>();                // closed-world subtypes
```

Plus `Class.allClasses()` and `Class.classesInPackage("test")`. Annotation/package
matching is **exact** on the canonical form (a bare `@Component` is `"code.Component"`).

`Class.heapInstance<Shape>(name)` is bounded by-name construction: resolves, verifies
`leaf <: Shape`, then no-arg constructs — `Optional<Shape>` empty on unknown name OR
non-`Shape`. The result IS owned (unlike a `Class` handle).

## Errors

- `IllegalAccessException` — reflective field access or construction against a
  **private member of a `@Sealed` class** (REFL-3.3 / D1; index accessors gate on it).
- `UnsupportedReflectionException` — `getBoxed` on a non-boxable (reference) field.

## What this does NOT do

- No name-keyed lookup of a field/method (`getField("id")`) — index only at this level.
- No conversion in typed accessors — wrong-type get/set is silently wrong.
- No string get/set primitive — strings/objects go through `getBoxed`/`Field`.
- No multi-arg reflective invoke or construct here — use `Method`/`Constructor`.
- No `Class<T>`-typed `forName` (see ownership note) — only `Class<?>`.
