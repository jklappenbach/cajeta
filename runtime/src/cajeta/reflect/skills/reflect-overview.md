---
id: reflect-overview
applies-to: [cajeta.reflect]
title: cajeta.reflect orientation — runtime reflection rooted at Class
description: Map and routing for runtime reflection (Class root, index-driven RTTI members, @Sealed access, ownership of returned members vs borrowed Class).
---

# cajeta.reflect — runtime reflection

Read-only-plus-invoke introspection over the compiler-emitted RTTI. **`Class` is
the root and the only thing you obtain directly**; every other type
(`Field`, `Method`, `Constructor`, `Parameter`, `Annotation`, `Modifiers`,
`TemplateParameter`, `TemplateArgument`) is reached *through* a `Class`. Members
are addressed **by index**, not by name — you scan counts and compare names
yourself. This is the runtime face of RTTI; it is not a code-generation,
proxy, or bytecode-rewriting framework.

## Task → entry point

| Want to… | Start with |
|---|---|
| Get the `Class` of an object (dynamic type) | `Class.of(o)` (static factory; same as `o.getClass()`) |
| Get the `Class` of a known type | `T.class` → `Class<T>` |
| Resolve a `Class` from its canonical name | `Class.forName(String)` → `Optional<Class<?>>` (empty if unknown) |
| Enumerate the program's classes | `Class.allClasses()`, `Class.classesInPackage(name)`, `Class.classesAnnotated(name)` |
| List/read fields | `c.getFieldCount()` + `c.getField(i)` → `Field`, or index-form `c.getInt32(o, i)` etc. |
| List/invoke methods | `c.getMethodCount()` + `c.getMethod(i)` → `Method`, then `m.invoke*` |
| Construct reflectively | `c.getConstructor(i).heapInstance(...)`, or `c.heapInstance(i)` (no-arg, by index) |
| Construct **type-checked** by name | `Class.heapInstance<Shape>(name)` → `Optional<Shape>` |
| Enumerate subtypes of a bound | `Class.subtypes<Shape>()` → `#Class<?>[]` |
| Read a member's type uniformly (boxed) | `Field.getBoxed(o)` / `Method.invokeBoxed(o, …)` → `#Object` |
| **Look up a field/method by name in one call** | **Not provided.** Iterate `getFieldCount`/`getMethodCount` and compare `getName()`. |
| **Navigate to the superclass `Class`** | **Not provided.** Only `getParentCount()` exists; there is no `getSuperclass()`. |
| **Reach a method/field returning a borrow** | **Don't** — reflection has no borrow-return surface; see hazards. |

## Cross-cutting invariants

- **A `Class` handle is a process-lifetime BORROW**, never freed. Each is the
  compiler's cached per-type `#ClassObject` constant. `Class c = Class.of(o);`
  registers no drop entry. The same holds for every `Class` element of the
  arrays from `allClasses`/`classesInPackage`/`classesAnnotated`/`subtypes`:
  the **array owns its buffer, not its elements** — freeing the array frees no
  `Class`.
- **Returned `#` member objects ARE owned by you.** `getField`/`getMethod`/
  `getConstructor`/`getParameter`/`getAnnotation`/`getModifiers`/
  `getTemplateParameter`/`getTemplateArgument` all return `heap`-allocated
  objects (and `#String` for the name accessors) — drop-tracked, freed at scope
  exit like any owned value.
- **Errors are exceptions, both `RecoverableException` subclasses:**
  `IllegalAccessException` (sealed-off member) and
  `UnsupportedReflectionException` (no wrapper / unsafe boxing). Catch and skip,
  or fall back. `forName` is the exception to the exception: it returns
  `Optional` (empty = not found), never throws.
- **Null conventions:** `Class.of`/`forName`'s native layer yields null only
  internally — surfaced as `Optional`. `invokeBoxed` of a `void` method returns
  `null` after running for side effects. Index-form accessors with an
  out-of-range index return empty strings / zero counts, not exceptions.
- **Access model — DEFAULT-OPEN.** By default reflection reaches any member
  regardless of visibility. A class-level **`@Sealed`** opts the class out *for
  its private members only*; reflective read/write/invoke/construct of those
  raises `IllegalAccessException` (the compiler omits them from the synthesized
  adapters). Public members of a `@Sealed` class stay reachable.
- **Mutability/threading:** `Class` and member objects are immutable views, safe
  to share across fibers. Reflective `set*`/`invoke` mutate the *target object*
  with no added synchronization — same rules as a direct field write.

## Canonical example

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.Field;
import cajeta.lang.Object;
import cajeta.lang.String;

User u = heap User();
Class<?> c = Class.of(u);              // borrow; never freed
int32 n = c.getFieldCount();
int32 i = 0;
while (i < n) {
    Field f = c.getField(i);          // owned; dropped at loop-iteration end
    String name = f.getName();        // owned #String
    if (name.equals("id")) {
        f.setInt32(u, 99);            // mutates u directly
    }
    i = i + 1;
}
```

## Hazards

- **Boxing-returns-a-borrow trap.** `Method.invokeObject`/`invokeBoxed` hand
  back an **owned** `#Object`, so they are only sound for methods that return
  `heap T` (ownership transfers). A method returning a *borrow* must NOT be
  reached this way — reflection can't see the borrow's lifetime, so treating it
  as owned double-frees. For the same reason `Field.getBoxed` raises
  `UnsupportedReflectionException` on a **reference** field rather than aliasing
  the held object; read references with the typed accessors.
- **`getBoxed`/`invokeBoxed` only box W1 types** (`boolean`/`int32`/`int64`/
  `float32`/`float64`) plus the W2 8/16-bit and unsigned ints and `char`.
  int128, half/quad/ML floats, and `pointer` raise
  `UnsupportedReflectionException` — fall back to the typed
  `getInt32`/`invokeFloat64`/… surface and box manually.
- **No name-keyed lookup.** There is no `getField("id")` / `getMethod("bump")`;
  you iterate by index and compare `getName()`.
- **Indices are declaration order**, stable within a build but not a public
  contract — resolve by name when you need a specific member.
- **`forName` takes a canonical name** (`"test.User"`, `"cajeta.lang.String"`);
  primitives (`"int32"`) are not classes and never resolve.
- **Bounded `forName<T>` is deferred** — for type-checked by-name construction
  use `Class.heapInstance<Shape>(name)` (returns `Optional<Shape>`, an owned
  value), not a `Class<Shape>` handle.

## When to use which

- **`Class.of(o)`** for the dynamic type of a value; **`T.class`** when the
  type is statically known.
- **`Class.forName`** when you have a name string; **`allClasses`/
  `classesInPackage`/`classesAnnotated`/`subtypes<T>`** to discover classes you
  don't already name.
- **Index-form `c.getInt32(o, i)` / `c.invokeScalar(o, i)`** for a quick
  one-off; **`Field`/`Method` objects** when you keep and reuse the handle or
  need its metadata (modifiers, parameters, annotations).
- **Typed `invokeInt32`/`invokeFloat64`/`invokeObject`** when you know the
  return type; **`invokeBoxed`** for generic consumers that don't.

## Setup / preconditions

- Package: `cajeta.reflect` (this library). RTTI is emitted by the cajeta
  compiler for every class — no opt-in needed.
- The class registry (`allClasses` etc.) today holds **every compiled class**.
  Once AOT class-stripping lands it holds the statically-reachable set plus
  every `@Retained` class; `@Retained` is advisory until then.

## Going deeper

Class-level detail (signatures, per-method ownership/null, kind codes) lives in
the type skills: `cajeta/reflect/Class`, `cajeta/reflect/Field`,
`cajeta/reflect/Method`, `cajeta/reflect/Constructor`. Exception semantics:
`cajeta/reflect/IllegalAccessException`,
`cajeta/reflect/UnsupportedReflectionException`.
