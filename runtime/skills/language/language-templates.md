---
id: language-templates
applies-to: [cajeta/language/templates, cajeta/language/wildcards]
title: Templates — monomorphization, numeric bounds, wildcards
description: Templates monomorphize (no erasure — say "templates", never "generics"); method templates, Numeric/Integral/Floating pseudo-bounds, PECS wildcards, and the template-field ownership trap.
---

# Templates — monomorphized, never erased

Each (class, type-argument) pair compiles to a **distinct runtime type** with
its own field layout and vtable. `Box<int32>` stores a bare `int32` — no
boxing; `Box<String>` is a separate compiled class. `T` is a real type at
compile time. Never reason from Java erasure, and say "templates", not
"generics".

## Class templates — and the ownership trap

```cajeta
public class Box<T> {
    public T value;
    public Box(#T v) { this.value #= v; }   // OWN the element
    public T get() { return this.value; }
}
```

**Hazard (now a compile error; history in**
`specs/archive/field-store-title-trap-spec.md`**)**: the plain shape
`Box(T v) { this.value = v; }` is rejected with
`CAJETA_ERROR_CAPTURED_BORROW_PARAM` (ownership spec §4.2), because it *would*
dangle: `heap Box<Dog>(heap Dog())` surrenders the title to a formal that never
consumes it, so the object would be freed at constructor exit and the field
would point at freed memory. The rejection lands on the CONSTRUCTOR, so it
fires whatever the call site does — passing a named local does not make the
shape legal. This is **not** template-specific: a concrete `Dog value;` field is
rejected identically. Two legal spellings, and the choice is an API decision:
`Box(#T v) { this.value #= v; }` forces every caller to surrender, while
`Box(T v) { this.value #= v; }` is the sink model (§2.3) where `#=` records the
source's mode and the caller chooses per call.

## Method templates

Type parameters sit after the method **name**, and the method must be `final`
(instance) or `static` — templated methods are excluded from the vtable:

```cajeta
public final U first<U>(U a, U b) { return a; }
public static V second<V>(V a, V b) { return b; }
```

Each call-site type tuple monomorphizes separately: `t.first<int32>(7, 9)`.

## Numeric pseudo-bounds

Class bounds (`<T extends Base>`) can't admit primitives; three built-ins do:
`Numeric` (all ints + floats), `Integral`, `Floating`. Arithmetic on `T`
lowers to the primitive op — no boxing. Out-of-bound instantiation fails with
`CAJETA_ERROR_TYPE_PARAMETER_BOUND`.

```cajeta
public class Duo<T extends Numeric> {
    public T x;
    public T y;
    public Duo(T x, T y) { this.x = x; this.y = y; }
    public Duo<T> add(Duo<T> o) { return stack Duo<T>(this.x + o.x, this.y + o.y); }
}
```

## Wildcards (use-site variance, PECS)

Invariance is the default — `Box<Dog>` is **not** a `Box<Animal>`.
`Box<? extends Animal>` accepts any Animal-subtype instantiation; reads
project through the upper bound (virtual dispatch works), foreign writes are
rejected (`CAJETA_ERROR_PECS_WRITE_VIOLATION`). `Box<? super Dog>` is the
converse; `Box<?>` is unbounded. Capture conversion lets a value read from a
wildcard receiver feed back into the *same* receiver.

## Worked example (verified: returns 68)

```cajeta
package dev.cajeta.skills;

public class Animal {
    public int32 tag() { return 1; }
}
public class Dog extends Animal {
    public int32 tag() { return 2; }
}

public class TemplatesDemo {
    public final U first<U>(U a, U b) { return a; }
    public static V second<V>(V a, V b) { return b; }

    public static int32 inspect(Box<? extends Animal> b) {
        return b.value.tag();     // reads project through the upper bound
    }

    public static int32 run() {
        Box<int32> bi = heap Box<int32>(42);      // bare int32 field — no boxing

        Duo<int32> di = stack Duo<int32>(1, 2);
        Duo<int32> ds = di.add(di);               // (2, 4)
        Duo<float64> df = stack Duo<float64>(0.5, 1.5);   // distinct compiled class

        TemplatesDemo t = stack TemplatesDemo();
        int32 f = t.first<int32>(7, 9);           // 7
        int32 g = TemplatesDemo.second<int32>(7, 9);  // 9

        Box<Dog> bd = heap Box<Dog>(heap Dog());  // owning Box — safe
        int32 tag = TemplatesDemo.inspect(bd);    // 2 — virtual through the bound

        Box<? extends Animal> w = heap Box<Dog>(heap Dog());
        int32 wtag = w.get().tag();               // 2 — projection at the local

        return bi.get() + ds.x + ds.y + f + g + tag + wtag;  // 68
    }
}
```

## Sharp edges

- `T.class` inside a template body does not yet resolve the substituted type
  argument (guide ch. 09/21) — don't build reflection on it from inside the
  template.
- Bounds beyond the numeric pseudo-bounds are class bounds; a primitive
  outside `Numeric`/`Integral`/`Floating` cannot satisfy them.
