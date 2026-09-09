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

> *Discussion.* `final` on a class declares it closed to extension. As of 0.27.0 extending a `final` class is not yet diagnosed; the restriction is bound here and enforcement follows. `sealed` / `permits` / `non-sealed` are in the grammar; their semantics are not yet specified here.

## 8.2 Members

A class body declares fields, methods, constructors, at most one destructor (§8.6), and operator declarations (§8.5). Members are in scope throughout the class body regardless of order. Instance members are reached through a receiver (`this.field`, `obj.method()`); static members belong to the class and are reached through the class name.

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

> *Discussion.* As of 0.27.0 a parent that lacks a no-argument constructor is silently skipped by implicit construction rather than diagnosed; a diagnostic is intended. Reaching a non-first parent's constructor explicitly is not yet specified.

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

> *Discussion.* A bare collision — two parents declaring the same member name, uninvolved in any override — is not yet diagnosed: as of 0.27.0 the resolution silently picks one parent's member. The intended rule is strict-by-default (a collision is an error until the program disambiguates with the qualified selectors); it is bound here when the diagnostic lands.

## 8.5 Method Dispatch

Instance method calls dispatch virtually: the runtime selects the most-derived override for the receiver's dynamic type, regardless of the receiver expression's static type. Assigning a derived instance to a base-typed binding adjusts the reference to the base's sub-object; calls through it still reach the derived overrides.

## 8.6 Operator Declarations

Two shapes, by mutation:

1. **Binary operators are `public static`**, both operands explicit, no implicit `this`; the operator returns a fresh value and mutates neither operand. Shipped: `+ - * / % & | ^ << >>` and the comparisons `== != < > <= >=`.
2. **Indexed access is an instance member** — `[]` and `[]=` — because it targets the receiver; the call site must hold a mutable borrow for `[]=`.

**Example 8.6-1.** A static binary operator.

```cajeta
public final class Vec2 {
    public float32 x; public float32 y;
    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }
    public static Vec2 operator+ (Vec2 a, Vec2 b) { return stack Vec2(a.x + b.x, a.y + b.y); }
}
Vec2 v = stack Vec2(1.0f, 2.0f) + stack Vec2(3.0f, 4.0f);
System.stdout.println("" + v.x + "," + v.y);    // 4,6
```

> *Discussion.* Deferred operator forms — unary `+`/`-`, mutating `++`/`--`, compound assignment, and `operator!`/`operator~` — have grammar coverage in part but are not lowered; they are specified in the internal operator document and bound here as they ship.

## 8.7 Destructors

A class may declare one destructor, `~ClassName()`:

- The identifier must match the class; a parameter list or return type is a parse error.
- It is not user-callable; only the drop chain invokes it (Allocation §4).
- Inside the body, `this` is live: fields, methods, and intrinsics all work. The instance's memory is reclaimed after the body returns.
- It runs exactly once per instance, at whichever drop entry ends up owning the instance (Ownership §5.9).

Destructor chaining is automatic and non-suppressible: the drop runs the class's own destructor body and field auto-drops, then every transitive ancestor's, each ancestor exactly once even in a diamond. Dispatch on drop is virtual for heap instances — `Base b = heap Derived()` fires `~Derived()` — while stack instances use static dispatch, since the allocation site fixes the dynamic type.
