# 9 — Interfaces

This chapter defines interfaces: contracts of abstract methods that classes implement, with virtual dispatch through interface-typed references. Interfaces carry no state and no method bodies — concrete reuse is what multiple inheritance of behavior is for (Classes §8.4).

## 9.1 Interface Declarations

```text
interfaceDeclaration
    : INTERFACE identifier typeParameters? (EXTENDS typeList)? interfaceBody
    ;
```

An interface declares abstract methods: a signature and no body. An interface has no fields, no constructors, and no default method bodies, by design — a type that wants to hand implementations down the hierarchy is a class, and multiple inheritance of behavior covers the reuse case that default methods patch over in single-inheritance languages.

An interface may extend one or more interfaces; the extending interface's contract is the union of its own methods and everything inherited.

> *Discussion.* As of 0.27.0 a method body inside an interface parses without a diagnostic and is ignored; the rejection is bound here and enforcement follows.

## 9.2 Implementing Classes

`class C implements I, J` obligates `C` to a concrete, accessible implementation of every method in each listed interface's contract, including inherited ones. Leaving any method unimplemented in a non-`abstract` class is a compile-time error.

**Example 9.2-1.** A rejected program: an unimplemented contract.

<!-- snippet: skip -->
```cajeta
public interface Ider { int32 id(); }
public class Broken implements Ider { }    // CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED

Ider i = heap Broken();
```

A class may both extend classes and implement interfaces; a method inherited from a behavior base can satisfy an interface obligation when its signature matches.

## 9.3 Interface Types

An interface name is a reference type. A reference to an instance of any implementing class converts to it implicitly, and calls through it dispatch virtually to the receiver's most-derived implementation — including methods the interface inherited from its superinterfaces.

**Example 9.3-1.** Dispatch through an interface, and through an inherited contract method.

```cajeta
public interface Ider { int32 id(); }
public interface Named extends Ider { String name(); }
public class Thing implements Named {
    public int32 id() { return 7; }
    public String name() { return "thing"; }
}
public final class C {
    public static void run() {
        Ider direct = heap Thing();
        Named n = heap Thing();
        System.stdout.println("" + direct.id() + " " + n.name() + " " + n.id());   // 7 thing 7
    }
}
C.run();
```

> *Discussion.* One conversion shape is broken as of 0.27.0: assigning an interface-typed *value* to a variable of its superinterface type (`Ider i = n;` where `n` is `Named`) compiles but dispatches incorrectly at run time. Until fixed, re-derive the narrower view from the class-typed reference. Interface-typed references also carry open ownership gaps at session scope (Script Units §18).

## 9.4 Choosing Interfaces or Behavior Bases

An interface says *what* a type can do and holds nothing; a behavior base (Classes §8.4) brings a concrete implementation and possibly state. Declare an interface when independent implementations must be substitutable behind one contract; extend a behavior base when the point is sharing one implementation. The two compose: a class may extend bases for its machinery and implement interfaces for its contracts.
