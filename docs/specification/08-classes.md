# 8 — Classes

This chapter defines class declarations and their members — fields, methods, constructors, destructors, and operator declarations — together with Cajeta's inheritance model: a class may extend multiple classes, each contributing methods and state, with virtual dispatch across all of them.

## 8.1 Class Declarations

```text
classDeclaration
    : CLASS identifier typeParameters?
      (EXTENDS typeList)?
      (IMPLEMENTS typeList)?
      (PERMITS typeList)?
      classBody
    ;
```

Modifiers (`public`, `final`, `abstract`, …) precede the declaration. A class declares an optional type-parameter list (Templates §11), an optional list of extended classes (§8.4), an optional list of implemented interfaces (Interfaces §9), and a body of members.

> *Discussion.* `final` on a class declares it closed to extension. As of 0.27.0 extending a `final` class is not yet diagnosed. The restriction is bound here and enforcement follows. `sealed` / `permits` / `non-sealed` are in the grammar, and their semantics are not yet specified here.

## 8.2 Members

A class body declares fields, methods, constructors, at most one destructor (§8.8), and operator declarations (§8.7). Members are in scope throughout the class body regardless of order. Instance members are reached through a receiver (`this.field`, `obj.method()`). Static members belong to the class and are reached through the class name.

Methods may be overloaded: two methods of the same name with different parameter types are distinct. Transfer mode is not part of a signature — declaring overloads that differ only in `#` is a compile-time error (Ownership §5.5.1).

**Example 8.2-1.** Static state and methods.

```cajeta
public final class Counter {
    public static int32 count;
    public static int32 bump() { Counter.count = Counter.count + 1; return Counter.count; }
}
Counter.bump();
System.stdout.println(Counter.bump());    // 2
```

## 8.3 Constructors

A constructor is declared with the class's own name and no return type. Construction runs the parents' constructors first — implicitly, one per extended class, in the declared order of the extends list — then the class's own constructor body. An explicit `super(args)` invocation reaches the first declared parent.

**Example 8.3-1.** Implicit parent construction in declared order.

```cajeta
public class A { public A() { System.stdout.println("A ctor"); } }
public class B { public B() { System.stdout.println("B ctor"); } }
public class Both extends A, B { public Both() { System.stdout.println("Both ctor"); } }
Both b = heap Both();     // prints: A ctor, B ctor, Both ctor
```

> *Discussion.* As of 0.27.0 a parent that lacks a no-argument constructor is silently skipped by implicit construction rather than diagnosed, and a diagnostic is intended. Reaching a non-first parent's constructor explicitly is not yet specified.

## 8.4 Inheritance

`class C extends A, B` inherits the members of every listed parent: methods *and* fields. Each parent contributes a sub-object to the instance layout, so parents with state compose without interference.

**Example 8.4-1.** Two parents, each contributing state and behavior.

```cajeta
public class Timestamped { public int64 created; public int64 age(int64 now) { return now - this.created; } }
public class Labeled { public String label; public String describe() { return "<" + this.label + ">"; } }
public class Event extends Timestamped, Labeled {
    public Event() { this.created = 100; this.label = "ev"; }
}
Event e = heap Event();
System.stdout.println(e.age(150) + " " + e.describe());    // 50 <ev>
```

**Overriding.** A method in `C` with the same name and parameter types as an inherited method overrides it — on every path: a call through a receiver typed as any ancestor dispatches to the most-derived override.

**Super and base selection.** `super.method()` resolves to the first declared parent. The qualified selectors `super<Base>.method()` and `this<Base>.field` name a specific base's member when more than one parent is in play.

> *Discussion.* A bare collision — two parents declaring the same member name, uninvolved in any override — is not yet diagnosed: as of 0.27.0 the resolution silently picks one parent's member. The intended rule is strict-by-default (a collision is an error until the program disambiguates with the qualified selectors), and it is bound here when the diagnostic lands.

## 8.5 Abstract Classes and Methods

An **abstract method** is declared with the `abstract` modifier and no body. It contributes a signature and a dispatch slot, and it obligates every concrete descendant to supply an implementation. An **abstract class** is a class declared `abstract`. It is a type that bindings, parameters, fields, returns and array elements may name, and it is not instantiable. The two modifiers say different things, and neither is inferred from the other.

**The method modifier.** An abstract method has no body. It is a compile-time error to declare an abstract method with a body (`CAJETA_ERROR_ABSTRACT_METHOD_HAS_BODY`), to declare one `static` (`CAJETA_ERROR_ABSTRACT_STATIC_METHOD`, there is nothing to dispatch), or to declare one `private` (`CAJETA_ERROR_ABSTRACT_PRIVATE_METHOD`, a descendant could never override it). A missing body is not a way to spell `abstract`: a method with no body that is not declared `abstract`, and carries no annotation that supplies its body (`@Native`, `@Intrinsic`, a synthesizer), is a compile-time error, `CAJETA_ERROR_METHOD_MISSING_BODY`. On an interface method the modifier is accepted and redundant (Interfaces §9). A record cannot declare one (`CAJETA_ERROR_RECORD_ABSTRACT_METHOD`).

**The class modifier.** A class that declares an abstract method must be declared `abstract`, or compilation fails with `CAJETA_ERROR_ABSTRACT_METHOD_IN_CONCRETE_CLASS`. The class modifier is two further assertions that the method modifier cannot make. First, the class is not allocatable with `heap` or `stack`, whether or not it has an abstract method (`CAJETA_ERROR_ABSTRACT_INSTANTIATION`). Second, the class may inherit an abstract method, or implement an interface, without discharging the obligation itself. The obligation passes to its descendants. A class that is not `abstract` must satisfy every obligation from every base and interface, or compilation fails with `CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED` or `CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED`. An `abstract final` class is a contradiction and is rejected (`CAJETA_ERROR_ABSTRACT_FINAL_CLASS`). A record cannot be abstract (`CAJETA_ERROR_RECORD_ABSTRACT`).

Reflection reports both: `Class.isAbstract()`, and `isAbstract()` on the `Modifiers` of a class or a method ([cajeta.reflect](reflect/Reflection.md)).

**Example 8.5-1.** An abstract base, a concrete subclass, and dispatch through a base-typed binding.

```cajeta
public abstract class Shape {
    public Shape() { return; }
    public abstract int32 area();
    public int32 twice() { return this.area() * 2; }
}
public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }
}
public final class C {
    public static int32 run() {
        Shape sh = heap Square(3);
        return sh.twice();      // 18 — twice() calls the derived area()
    }
}
```

**Example 8.5-2.** A rejected program. `Blob` is concrete and inherits `area()` without implementing it.

<!-- snippet: skip -->
```cajeta
public abstract class Shape {
    public Shape() { return; }
    public abstract int32 area();
}
public class Blob extends Shape {
    public Blob() { return; }   // CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED
}
```

**Example 8.5-3.** An intermediate abstract class. `Polygon` adds state and behavior, leaves `area()` to its descendants, and does not restate it.

```cajeta
public abstract class Shape {
    public Shape() { return; }
    public abstract int32 area();
}
public abstract class Polygon extends Shape {
    int32 sides;
    public Polygon(int32 n) { this.sides = n; }
    public int32 sideCount() { return this.sides; }
}
public class Square extends Polygon {
    int32 side;
    public Square(int32 s) { super(4); this.side = s; }
    public int32 area() { return this.side * this.side; }
}
public final class C {
    public static int32 run() {
        Polygon p = heap Square(3);
        return p.area() + p.sideCount();    // 13
    }
}
```

**Example 8.5-4.** A complete abstract class. Every method has a body, and the class is still not allocatable. Only its descendants are.

<!-- snippet: skip -->
```cajeta
public abstract class Base {
    public Base() { return; }
    public int32 id() { return 7; }
}
public class Leaf extends Base {
    public Leaf() { return; }
}
public final class C {
    public static int32 run() {
        Base b = heap Base();    // CAJETA_ERROR_ABSTRACT_INSTANTIATION
        Base l = heap Leaf();    // fine
        return l.id();
    }
}
```

**Example 8.5-5.** An abstract class implements an interface and leaves part of the contract to its leaf. A call through the interface reaches the leaf.

```cajeta
public interface Drawable {
    int32 draw();
    int32 id();
}
public abstract class Widget implements Drawable {
    public Widget() { return; }
    public int32 id() { return 1; }
}
public class Button extends Widget {
    public Button() { return; }
    public int32 draw() { return 41; }
}
public final class C {
    public static int32 run() {
        Drawable d = heap Button();
        return d.draw() + d.id();    // 42
    }
}
```

**Satisfaction across parents.** Under multiple inheritance, an abstract method inherited from one parent is satisfied by a concrete method of the same signature inherited from another. The obligation belongs to the class, not to the branch that declared it, and dispatch through the abstract declaration lands on the concrete implementation.

**Example 8.5-6.** The obligation from `A` is discharged by the implementation in `B`, and a call through the `A`-typed binding reaches it.

```cajeta
public abstract class A {
    public A() { return; }
    public abstract int32 step();
}
public class B {
    public B() { return; }
    public int32 step() { return 42; }
}
public class Both extends A, B {
    public Both() { return; }
}
public final class C {
    public static int32 run() {
        A a = heap Both();
        return a.step();        // 42
    }
}
```

**The vtable slot.** A class with an abstract method has a dispatch slot for it that holds a stub rather than null. The compile-time rules above make that slot unreachable from cajeta source. A reflective or foreign caller that reaches it gets a panic naming the class and the method, not a null dereference.

## 8.6 Method Dispatch

Instance method calls dispatch virtually: the runtime selects the most-derived override for the receiver's dynamic type, regardless of the receiver expression's static type. Assigning a derived instance to a base-typed binding adjusts the reference to the base's sub-object, and calls through it still reach the derived overrides.

## 8.7 Operator Declarations

Two shapes, by mutation:

1. **Binary operators are `public static`**, both operands explicit, no implicit `this`. The operator returns a fresh value and mutates neither operand. Shipped: `+ - * / % & | ^ << >>` and the comparisons `== != < > <= >=`.
2. **Indexed access is an instance member** — `[]` and `[]=` — because it targets the receiver. The call site must hold a mutable borrow for `[]=`.

**Example 8.7-1.** A static binary operator.

```cajeta
public final class Vec2 {
    public float32 x; public float32 y;
    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }
    public static Vec2 operator+ (Vec2 a, Vec2 b) { return stack Vec2(a.x + b.x, a.y + b.y); }
}
Vec2 v = stack Vec2(1.0f, 2.0f) + stack Vec2(3.0f, 4.0f);
System.stdout.println("" + v.x + "," + v.y);    // 4,6
```

> *Discussion.* Deferred operator forms — unary `+`/`-`, mutating `++`/`--`, compound assignment, and `operator!`/`operator~` — have grammar coverage in part but are not lowered. They are specified in the internal operator document and bound here as they ship.

## 8.8 Destructors

A class may declare one destructor, `~ClassName()`:

- The identifier must match the class, and a parameter list or return type is a parse error.
- It is not user-callable. Only the drop chain invokes it (Allocation §4).
- Inside the body, `this` is live: fields, methods, and intrinsics all work. The instance's memory is reclaimed after the body returns.
- It runs exactly once per instance, at whichever drop entry ends up owning the instance (Ownership §5.9).

Destructor chaining is automatic and non-suppressible: the drop runs the class's own destructor body and field auto-drops, then every transitive ancestor's, each ancestor exactly once even in a diamond. Dispatch on drop is virtual for heap instances — `Base b = heap Derived()` fires `~Derived()` — while stack instances use static dispatch, since the allocation site fixes the dynamic type.
