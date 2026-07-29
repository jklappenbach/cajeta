# 17 — Inheritance

## Single inheritance

`extends` names the parent. Dispatch is virtual by default: a parent-typed
reference to a child instance calls the child's override through the vtable,
whether the receiver lives on the stack or the heap. Overriding is by
signature — re-declare the method; there is no `override` keyword
(`@Override` is accepted as optional documentation). `super.method()` reaches
the parent's implementation, and `abstract` methods are obligations the first
concrete subclass must meet. Tour demo:
[InheritanceDemo](../../samples/tour/src/main/cajeta/tour/lang/InheritanceDemo.cajeta).

```cajeta
public class Shape {
    public int32 area() { return 0; }
}
public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }
}
```

```cajeta
Shape s = stack Square(5);
int32 a = s.area();
```

## Multiple inheritance

A class may extend several bases and inherit concrete behavior from all of
them — state is per-base, behavior composes. Tour demo:
[MultiInheritanceDemo](../../samples/tour/src/main/cajeta/tour/lang/MultiInheritanceDemo.cajeta);
design: [MultiClassing.md](../specification/lang/MultiClassing.md).

When two parents declare the same method, the child resolves the collision by
overriding and selecting a parent explicitly with `super<Base>`:

```cajeta
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
```

One caveat today: an *unqualified* call to a colliding inherited method (no
override in the child) silently picks one parent. The intended rule makes
that a compile error; until it lands, resolve collisions with an override.

### Diamonds share the ancestor

When two parents descend from the same ancestor, the child holds one shared
ancestor subobject — a write through one path is visible through the other:

```cajeta
public class Meta {
    public int32 x;
    public Meta() { this.x = 0; }
}
public class Left extends Meta {
    public void setViaLeft(int32 v) { this.x = v; }
}
public class Right extends Meta {
    public int32 getViaRight() { return this.x; }
}
public class Both extends Left, Right { }
```

```cajeta
Both b = stack Both();
b.setViaLeft(7);
int32 seven = b.getViaRight();
```

Unrelated parents that merely happen to declare same-named fields keep
separate slots — sharing applies only to a genuine common ancestor.

### Template mixins

Templates and multiple inheritance compose: a generic class inherits behavior
mixins once and carries them across every instantiation. See `Crate<T>
extends Identified, Versioned` in the
[tour demo](../../samples/tour/src/main/cajeta/tour/lang/MultiInheritanceDemo.cajeta)
— `Crate<int32>` and `Crate<String>` both get the mixin methods for free.

## Interfaces

`interface` declares a pure contract — method signatures, no bodies, no
default methods. A class lists its interfaces after `implements`, and an
interface-typed reference dispatches to the implementing class:

```cajeta
public interface Drawable {
    int32 draw();
}
public class Sprite implements Drawable {
    public int32 draw() { return 42; }
}
```

```cajeta
Drawable d = stack Sprite();
int32 n = d.draw();
```

Interfaces can be templated, and `extends` and `implements` combine — the
stdlib's `ArrayStream<T> extends Stream<T> implements Splittable<T>` is the
canonical shape. Since Cajeta has multiple inheritance of concrete behavior,
interfaces are for contracts, not for smuggling implementation.

Next: [18 Annotations](18-annotations.md).
