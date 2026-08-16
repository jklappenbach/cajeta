---
id: reflect-generics
applies-to: [cajeta/reflect/TemplateParameter, cajeta/reflect/TemplateArgument]
title: Reflecting cajeta templates — declared parameters vs concrete arguments
description: Read a class's declared template parameters (the <T>) and an instantiation's concrete arguments (the int32 in Box<int32>) via TemplateParameter and TemplateArgument.
---

# Reflecting generics: TemplateParameter + TemplateArgument

Cajeta has **templates, not erased generics**: it monomorphizes, so `Box<int32>`
and `Box<String>` are two distinct, separately-retained classes. Each
instantiation keeps BOTH sides of its genericity and reflection reads them back:

- **`TemplateParameter`** — the *declaration* side: the `T` in `class Box<T>`
  (name, bounds, and the non-type flag for value params like `uint32 N`).
- **`TemplateArgument`** — the *instantiation* side: the concrete `int32` that
  `Box<int32>` bound to `T`.

A single monomorphized class carries both. `Box<int32>` reports parameter `T`
*and* argument `int32`; you read them through different accessors.

## Entry point — you do not construct these

You never `heap TemplateParameter(...)` yourself. You get them from a
`Class` (see `cajeta/reflect/Class`). The owning class's RTTI is what backs them.

```cajeta
import cajeta.reflect.Class;
import cajeta.reflect.TemplateParameter;
import cajeta.reflect.TemplateArgument;

public class Box<T> {
    T value;
    public Box(T v) { this.value = v; }
}

public final class M {
    public static int32 run() {
        Box<int32> b = heap Box<int32>(5);
        Class<?> c = Class.of(b);

        if (!c.isTemplateInstantiation()) { return 1; }

        // Declaration side: the <T>.
        TemplateParameter p #= c.getTemplateParameter(0);   // #-owned, see below
        if (!p.getName().equals("T")) { return 2; }
        if (p.isNonType()) { return 3; }
        if (p.getBoundCount() != 0) { return 4; }

        // Instantiation side: the concrete int32.
        TemplateArgument a #= c.getTemplateArgument(0);      // #-owned
        if (!a.getTypeName().equals("int32")) { return 5; }
        return 0;
    }
}
```

## Object graph and ownership across the boundary

- `Class.getTemplateParameter(i)` / `getTemplateArgument(i)` return **`#`-owned**
  objects (you free them / let scope drop them). Bounds and counts come from
  `Class.getTemplateParameterCount()` / `getTemplateArgumentCount()` — iterate by
  index; there is no array accessor.
- Each `TemplateParameter`/`TemplateArgument` holds a **borrowed** `pointer rtti`
  into the owning class's process-lifetime RTTI plus its `index`. It is a cheap
  view, not a copy — but it is only meaningful for that class. Don't hand a
  parameter object a different class's RTTI.
- **Every String accessor returns a `#`-owned String** that you own and free:
  `getName()`, `getNonTypeName()`, `getBound(b)`, `getTypeName()`, and both
  `toString()`s. They are freshly allocated each call (not borrowed views), so
  call once and keep the result rather than re-reading.

## TemplateParameter — declared placeholders

- `getName() -> #String` — `"T"`, or `"N"` for a non-type.
- `isNonType() -> boolean` — true for a value parameter (`<uint32 N>`), which
  binds a compile-time integer constant, not a type.
- `getNonTypeName() -> #String` — the declared primitive of a non-type
  (`"uint32"`); **empty String** for an ordinary type parameter (not null).
- `getBoundCount() -> int32` and `getBound(b) -> #String` — declared bounds
  `<T extends Foo & Bar>` → count 2; unbounded → 0. Bounds are exposed as
  **canonical-name Strings only** (e.g. `"code.Foo"`). They are NOT resolved to
  `Class` — there is no `getBoundType()`; resolve yourself via `Class.forName`.

## TemplateArgument — concrete instantiation types

- `getTypeName() -> #String` — canonical type name of the bound argument
  (`"int32"`, `"cajeta.lang.String"`). Always available; primitive or reference.
- `getType() -> Class<?>` (borrowed, process-lifetime cached — never free it) —
  resolves the type name through the `Class.forName` registry.
  **It throws for anything not in the registry.** Primitive arguments
  (`Box<int32>`'s `int32`) have no `Class` and raise
  `UnsupportedReflectionException` (see
  `cajeta/reflect/UnsupportedReflectionException`); so do stripped/unregistered
  types. Reach for `getTypeName()` when you only need the name, and reserve
  `getType()` for class-typed arguments you know are registered (or guard it).

## What this does NOT do

- No erasure: a non-template class reports `getTemplateParameterCount() == 0`,
  `getTemplateArgumentCount() == 0`, and `isTemplateInstantiation() == false`.
- No bound→`Class` resolution (`TemplateParameter`): names only.
- No primitive→`Class` resolution (`TemplateArgument.getType()` throws, not null).
- No reflective instantiation of new `Box<...>` types — reflection reads existing
  monomorphizations; it does not create them.
