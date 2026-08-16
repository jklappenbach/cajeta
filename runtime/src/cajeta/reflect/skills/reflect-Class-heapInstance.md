---
id: reflect-Class-heapInstance
applies-to: [cajeta/reflect/Class.heapInstance]
title: Class.heapInstance<T> — type-safe by-name allocation returning an owned Optional<T>
description: Resolve a class name, verify it is a subtype of T, and no-arg construct it; empty Optional on unknown-or-not-a-subtype, the held T is genuinely owned.
---

# `Class.heapInstance<T>(name)` — bounded by-name construction

Use this when you have a class **name as a `String`** and want to construct it as a
statically-typed `T` with **no unchecked downcast**. It resolves the name, checks
`leaf <: T`, then runs the type's **no-argument** constructor.

```cajeta
import cajeta.reflect.Class;
import cajeta.lang.Optional;

Optional<Shape> s #= Class.heapInstance<Shape>("test.Circle");
if (s.isPresent()) {
    s.get().draw();   // s.get() is a Shape — virtual dispatch lands on Circle
}
```

## Signature & semantics
`public static Optional<T> heapInstance<T>(String name, Class<?> bound)`.

You write only `Class.heapInstance<Shape>(name)` — the `bound` parameter is the
`Shape.class` token **injected by the compiler** from the `<Shape>` type argument; never
pass it yourself. The returned `Optional<T>` is **present** holding a freshly constructed
`T` when `name` resolves to a class that is `T` or a subtype of `T`; otherwise **empty**.

## Return ownership — this is the whole point
The held `T` is **genuinely owned** by the `Optional`: when the `Optional` drops, it
frees the instance. This is unlike the `Class<?>` handles from
`cajeta/reflect/Class.forName`, `Class.of`, and `Class.subtypes`, which are
process-lifetime **borrows** of cached `#ClassObject`s and are never freed. That
ownership difference is exactly why bounded *construction* ships here while a bounded
`forName<T>` does not (a `Class<T>` would be an owned container over a value that must
not be freed). Treat `s.get()` as an owned `T` and follow normal cajeta move/drop rules.

## Empty vs. throw — what each path does
- **Unknown name** → empty `Optional` (same lookup path as `forName`; not a throw).
- **Resolves, but `leaf` is NOT a subtype of `T`** → empty `Optional`. This is a clean
  not-found, *not* a bad cast or crash — the subtype check is what makes the internal
  `(T)` sound.
- Both of these are normal control flow: branch on `isPresent()` / `isEmpty()`.

It does **throw** `cajeta/reflect/IllegalAccessException` in one case: when the no-arg
constructor it would invoke is a private constructor of a `@Sealed` class (the same gate
as `Class.heapInstance(int32)`).

## Construction details / gotchas
- Construction always uses the **no-argument constructor at index 0** (it delegates to
  the instance primitive `Class.heapInstance(0)`). There is **no argument marshalling
  here** — for constructors with parameters use the `Constructor` API
  (`cajeta/reflect/Constructor`). If the target type has no usable no-arg constructor,
  this is the wrong entry point.
- `name` is the **canonical** name (e.g. `"test.Circle"`, `"cajeta.lang.String"`), the
  same form `Class.getName()` returns. Primitives (`"int32"`) are not classes and never
  resolve → empty.
- Identity counts as a subtype: `heapInstance<Shape>("test.Shape")` constructs a `Shape`.
- Side effect: allocates on the heap, installs the vtable, and runs the constructor body.

## Related
- `cajeta/reflect/Class.forName` — resolve a name to a borrowed `Class<?>` (no construction).
- `cajeta/reflect/Class.subtypes` — enumerate the closed-world subtype set of `T`.
- `cajeta/reflect/Constructor` — construct with arguments / pick a specific constructor.
