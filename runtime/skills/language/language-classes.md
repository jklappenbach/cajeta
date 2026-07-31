---
id: language-classes
applies-to: [cajeta/language/classes, cajeta/language/inheritance, cajeta/language/operators]
title: Classes, multiple inheritance, interfaces, and operator overloading
description: Virtual dispatch, multiple inheritance with super<Base> and shared diamonds, contract-only interfaces, and the static/instance operator split with the ==/hash() pairing rule.
---

# Classes, inheritance & operators

Construction and per-value `heap`/`stack` placement: `cajeta/language/types`.
Field/parameter ownership: `cajeta/language/ownership` — read it before adding
class-typed fields.

## Inheritance

- **Single**: `extends`; dispatch is virtual by default (stack or heap
  receiver alike). Overriding is by signature — no `override` keyword;
  `@Override` is optional documentation. `super.method()`, `abstract` as in
  Java.
- **Multiple**: a class may extend several bases — behavior composes, state is
  per-base. A method collision is resolved by overriding in the child and
  selecting explicitly with `super<Base>.method()`:

  ```cajeta
  public class DualView extends HtmlView, TextView {
      @Override(from=HtmlView)
      public String emit() {
          return super<HtmlView>.emit() + "+" + super<TextView>.emit();
      }
  }
  ```

  **Trap**: an *unqualified* call to a colliding inherited method with no
  child override silently picks one parent today (intended to become a compile
  error) — always resolve collisions with an override.
- **Diamonds share the ancestor**: two parents descending from one ancestor
  give the child a single shared ancestor subobject — a write through one path
  is visible through the other. Unrelated same-named fields keep separate
  slots.
- **Template mixins**: templates and multiple inheritance compose —
  `Crate<T> extends Identified, Versioned` carries the mixins into every
  instantiation.

## Interfaces

Pure contracts: signatures only — no state, no default methods (multiple
inheritance of concrete behavior is what cajeta has instead). `implements`
lists them; interface-typed references dispatch virtually. Interfaces can be
templated; `extends` + `implements` combine.

## Operator overloading

Two rules carry the design:

- **Binary and non-mutating unary operators are `public static`** — operands
  explicit, nothing mutates, fresh result:
  `public static #Vec2 operator+ (Vec2 a, Vec2 b) { ... }`. Dispatch is on
  the static type of the **LHS only**, non-virtual — `v * 2` needs an
  overload on `Vec2`; `2 * v` cannot be expressed (primitives can't declare
  operators).
- **Mutating unaries (`++`/`--`) and subscripts (`[]`/`[]=`) are instance
  methods** — `t++` lowers to `t.operator++()`; `g[i] = v` to
  `g.operator[]=(i, v)`; subscript indices arrive as `int64`. The stdlib's
  `HashMap` `m[k]` is this mechanism.

Derived forms: declare `==` and `!=` is derived; declare `<` and `<=`/`>`/`>=`
come free; `a += b` rewrites through `operator+` unless an explicit instance
`operator+=` is declared (which wins — for in-place mutation).
`&&`/`||` are deliberately not overloadable (short-circuit); `!` and `~` are
specified but do not compile yet.

**The `==`/`hash()` pairing rule**: structural `operator==` without a
structural `hash()` mis-keys `HashMap` — two equal instances land in different
buckets. The compiler warns (`CAJETA_WARN_HASH_EQUALS_MISMATCH`); `@AutoHash`
synthesizes a field-based `hash()` consistent with `==`.

## Worked example (verified: returns 87)

```cajeta
package dev.cajeta.skills;

public class Shape {
    public int32 area() { return 0; }
}

public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }
}

public interface Drawable {
    int32 draw();
}

public class Sprite implements Drawable {
    public int32 draw() { return 42; }
}

public class HtmlView {
    public String emit() { return "html"; }
}
public class TextView {
    public String emit() { return "text"; }
}
public class DualView extends HtmlView, TextView {
    @Override(from=HtmlView)
    public String emit() {
        return super<HtmlView>.emit() + "+" + super<TextView>.emit();
    }
}

@AutoHash
public class Vec2 {
    public int32 x;
    public int32 y;
    public Vec2(int32 x, int32 y) { this.x = x; this.y = y; }

    public static #Vec2 operator+ (Vec2 a, Vec2 b) {
        return heap Vec2(a.x + b.x, a.y + b.y);
    }
    public static boolean operator== (Vec2 a, Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }
}

public class ClassesDemo {
    public static int32 run() {
        Shape s = stack Square(5);
        int32 virt = s.area();              // 25 — virtual dispatch
        Drawable d = stack Sprite();
        int32 drew = d.draw();              // 42
        DualView dv = stack DualView();
        int32 emitLen = (int32) dv.emit().count();   // "html+text" = 9
        Vec2 a = stack Vec2(1, 2);
        Vec2 b = stack Vec2(3, 4);
        Vec2 sum = a + b;                   // Vec2.operator+(a, b)
        boolean neq = a != b;               // derived from operator==
        int32 neqI = 0;
        if (neq) { neqI = 1; }
        return virt + drew + emitLen + sum.x + sum.y + neqI;   // 87
    }
}
```
