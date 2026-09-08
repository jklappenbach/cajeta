# 11 — Templates & Wildcards

This chapter defines Cajeta's template model. Templates are monomorphized: each distinct type argument produces a distinct compiled instantiation with its own layout, vtable, and drop function. There is no erasure on the instantiation path; the erased view exists only where the program asks for it, through wildcards (§11.3).

## 11.1 The Template Model

A class or method may declare type parameters in angle brackets. Instantiating a template with a type argument compiles a version of it specialized to that argument. Primitive types are valid type arguments and instantiate without boxing — `Box<int32>` stores an `int32`, not a wrapper.

**Example 11.1-1.** A user template over a primitive and a library template.

```cajeta
import cajeta.collection.ArrayList;
public final class Box<T> {
    public T item;
    public Box(#T item) { this.item #= item; }
    public T get() { return this.item; }
}
Box<int32> b = heap Box<int32>(41);
ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(5);
System.stdout.println("" + b.get() + " " + xs[0]);    // 41 5
```

Boxed wrappers (Types §3.1) exist only for the boundary where a template slot requires a class type; the template model itself does not require them.

## 11.2 Template Declarations

Type parameters appear on class declarations (`class Box<T>`) and on methods. A template's type parameters are in scope throughout the declaring body, in field types, method signatures, and ownership spellings (`#T` formals and returns work per Ownership §5.4). At a template method's call site, type arguments are spelled explicitly in angle brackets — `stream.map<int32>((pt) -> pt.dist2())`.

> *Discussion.* Call-site deduction of method type arguments from argument types, and an explicit specialization syntax (a user-provided body for a particular instantiation), are not yet specified here.

## 11.3 Wildcards

A wildcard type argument stands for an unknown but tracked argument: `T<?>` (unbounded), `T<? extends Bound>` (some subtype of `Bound`), `T<? super Bound>` (some supertype). A wildcard type is usable wherever a type is: parameters, locals, fields.

Soundness follows producer/consumer polarity: a `? extends` view can be read at its bound but not written through — the unknown argument could be any subtype, so no written value is safe; a `? super` view can be written at the bound but reads only at `Object`. Writing through a `? extends` view is a compile-time error.

**Example 11.3-1.** One method over every instantiation.

```cajeta
import cajeta.collection.ArrayList;
public final class C {
    public static int32 count(ArrayList<?> xs) { return (int32) xs.count(); }
}
ArrayList<int32> a = heap ArrayList<int32>();
a.add(1); a.add(2);
ArrayList<String> s = heap ArrayList<String>();
s.add("x");
System.stdout.println("" + C.count(a) + C.count(s));    // 21
```

**Example 11.3-2.** A rejected program: writing through a `? extends` view.

<!-- snippet: skip -->
```cajeta
import cajeta.collection.ArrayList;
public final class C {
    public static void put(ArrayList<? extends Object> xs) {
        String v = "nope";
        xs.add(#v);            // CAJETA_ERROR_PECS_WRITE_VIOLATION
    }
}
```

Two wildcard occurrences are not assumed to be the same unknown type; identity is tracked through capture types where the language must reason about "the same unknown `T`".

> *Discussion.* A lint flags wildcard-typed access in hot loops, where the erased view defeats monomorphized code paths; suppress it per rule with `@SuppressLint` (Annotations §10.3) when the erased view is the point. The migration from the current receiver-identity heuristic to first-class capture identity is in flight.

## 11.4 Instantiation Across Archives

A library's templates ship in its `.cja` archive in compilable form; a consumer's instantiations with new type arguments are compiled in the consuming build against the shipped definition. A template instantiation error is therefore reported at the *instantiation site*, in the consumer, with the declaration it failed against.
