---
title: 'Cajeta Structs — Specification v1'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Language'
description: 'A struct in cajeta is a stack-allocated value aggregate — a named bundle of fields with no vtable in the receiver, no heap allocation of its own, and a lifetime tied to the enclosing scope. It is the ...'
---

## Purpose

A `struct` in cajeta is a **stack-allocated value aggregate** — a named bundle of fields with no vtable in the receiver, no heap allocation of its own, and a lifetime tied to the enclosing scope. It is the right choice for:

- **Small named tuples** — `Pair<A, B>`, `Entry<K, V>`, `(x, y)` coordinates, the result of a function that "wants to return two values."
- **Value-typed sums** — `Optional<T>` with a discriminant and a payload, kept off the heap.
- **Iterator state** — a cursor plus a borrow of the source. Stack-resident, monomorphizes through the `for` loop, no per-loop heap allocation.
- **Scope-bounded handles** — small bundles like `{ socket, sessionId, isAuthenticated }` that live for the duration of a request and don't need heap identity.

Structs **can hold class references**. They are not restricted to primitives. A class reference is just a pointer; the borrow checker tracks the lifetime of any class instance reachable through a struct field, the same way it tracks a local variable.

Structs are not for memory overlay of wire formats. That is `view`'s job — see `Views.md`. Trying to do both was the dual-role mistake the earlier `struct` design made; v1 separates them.

---

## `struct` vs `class` vs `view`

Cajeta has three user-defined aggregate kinds:

| | `class` | `struct` | `view` |
|---|---|---|---|
| Storage | heap | stack alloca | borrowed bytes |
| Lifetime | ownership-tracked | enclosing scope | borrowed buffer |
| Vtable on instance | yes (in header) | no | no |
| Inheritance | yes | no | no |
| Class refs in fields | yes | **yes** | no |
| Layout | compiler-chosen | compiler-chosen | declared, byte-exact |
| Endianness | host | host | annotation-required |
| Constructor allocates | yes (`new`) | no (stack alloca) | no (borrows / takes buffer) |
| Implements interfaces | yes | yes (tagged fat pointer) | no |

Use `class` when you need identity, inheritance, or behavior with virtual dispatch. Use `view` for typed access to a byte buffer that came off the wire or a memory-mapped file. Use `struct` for everything else — value-typed aggregates that should be cheap.

---

## Declaration

```cajeta
public struct Pair<A, B> {
    public A first;
    public B second;
}

public struct DbConnection {
    private TcpSocket socket;     // class reference — fine
    private int64     connectionId;
    private boolean   isAuthenticated;

    public boolean isAlive() {
        return this.socket.isConnected();
    }
}
```

Each field has a declared type. Field types can be:

- Primitives (`int*`, `uint*`, `float*`, `boolean`, `char`).
- Class references (`String`, `TcpSocket`, any user `class`).
- Other struct types (nested by value — see "Inline composition" below).
- Fixed-size arrays of any of the above (`T[N]`).
- Heap arrays (`T[]`) — the struct holds the array reference, not the bytes.

Methods are declared with the same syntax as a class. `this` is the struct pointer; methods can read and write fields and return primitives, owned values, or interface values.

Structs **do not** have constructors in the `new` sense. Initialization is one of:

- **Zero-init at declaration**: `Pair p;` — all bytes zeroed. Class-reference fields are null (which the borrow checker treats as "not initialized," not as a valid borrow).
- **Aggregate initializer**: `Pair p = Pair { first: 7, second: 11 };` — every field given an explicit value at declaration time.
- **Field assignment after declaration**: `Pair p; p.first = 7; p.second = 11;` — fields written individually.

---

## Storage and lifetime

A struct local lives on the stack of its declaring function. Allocation is a single `alloca` of the struct's fixed total size; no runtime cost beyond stack-pointer adjustment. The struct drops when its declaring scope exits — LIFO drop order, same as any other stack-resident owner.

```cajeta
public int32 sumPair() {
    Pair p;              // 16 bytes on the stack, zero-initialized
    p.first = 7;
    p.second = 11;
    return p.first + p.second;
    // p drops here — no heap to free, no runtime work
}
```

A struct field that holds an owned class reference participates in the drop chain: when the struct drops, its owned class fields drop too (recursively, in reverse declaration order). A struct field that holds a borrowed class reference does not — the borrow's source owns the class instance.

**Constraint**: every field in a struct must have a compile-time-known size. This means fixed-size arrays (`T[N]`) only as inline fields; variable-tail fields (the `byte[?]` form used in `view`) are not supported in `struct`. If you need variable-size content, hold a heap array (`T[]`) or a `String` — those are references with known pointer size; the bytes live on the heap.

**No recursive types.** A struct may not contain itself directly or transitively. The compiler detects this during layout and rejects it. Recursive shapes need indirection (a class reference, which has a known pointer size).

```cajeta
struct Bad {
    Bad child;     // STATIC ERROR — infinite size
}

struct Ok {
    Tree child;    // OK — Tree is a class, this is a pointer
}
```

---

## Inline composition in class fields

A `struct` can be a field of a `class`. The struct's bytes live **inline** in the class's heap layout — no extra allocation, no pointer indirection.

```cajeta
public class User {
    private Pair<int32, int32> coords;     // 16 bytes inline in User's layout
    private String              name;       // class reference (8-byte pointer)
    private DbConnection        db;         // struct inline; holds a TcpSocket ref
}

User u = new User(...);
u.coords.first = 42;       // direct field write into User's heap allocation
u.db.connectionId = 100;
```

When the class drops, every embedded struct field drops with it. The struct's own drop logic (releasing its owned class references, calling any user-defined destructor) runs as part of the class's tear-down.

Layout: the class's heap allocation includes the class header (vtable + RTTI), followed by fields in declaration order. Embedded structs occupy their fixed byte count inline. Class-reference fields occupy a single pointer-width slot. The compiler can reorder fields for packing within a class; struct field order within the struct is preserved (matters for ABI predictability when structs cross process boundaries).

---

## Methods

A struct can declare methods, including the usual `public` / `private` modifiers, generic type parameters, and parameter annotations. Method calls on the concrete struct type are **monomorphized** — direct calls, no vtable indirection, inlinable.

```cajeta
public struct Counter {
    private int64 value;

    public void increment() { this.value += 1; }
    public int64 get()      { return this.value; }
}

Counter c;
c.increment();    // direct call, inlinable
int64 v = c.get();
```

`this` is the struct's pointer (cajeta's existing pass-by-pointer calling convention for aggregate values). The method can read and write fields normally; the writes go directly to the struct's stack slot. Returning a `Self`-typed value from a method is allowed in the direct-call case (the compiler knows the size at every call site).

---

## Interfaces and polymorphic dispatch

Structs can implement interfaces. The dispatch mechanism is **tagged fat pointer**: an interface value holding a struct is a three-word value `{ data_ptr, vtable_ptr, kind_tag }`. The same interface type can hold either a class or a struct; the kind tag tells the runtime which it is.

```cajeta
public interface Iterator<T> {
    public Optional<T> next();
}

public struct ArrayIter<T> implements Iterator<T> {
    private T[]   data;
    private int64 idx;

    public Optional<T> next() {
        if (this.idx >= this.data.length()) return None();
        T v = this.data[this.idx];
        this.idx += 1;
        return Some(v);
    }
}

public class FileLineIter implements Iterator<String> {
    private File f;
    public Optional<String> next() { ... }
}

// Same call site accepts both:
void drain<T>(Iterator<T> it) {
    loop {
        match it.next() {
            Some(x) => process(x),
            None    => break,
        }
    }
}

drain(arr.iter());                  // arr.iter() returns ArrayIter<int32>
                                    // → struct-rooted interface value, borrowed
drain(new FileLineIter(path));      // → class-rooted interface value, owned
```

### Uniform dispatch path

Every interface call lowers to: load vtable_ptr (already in the interface value's second word), index by method offset, indirect-call. Two words drive every dispatch regardless of whether the underlying is a class or a struct. The kind tag is consulted only at lifecycle boundaries (drop, borrow tracking).

```
                  interface value layout
                  ┌───────────────┬───────────────┬──────────┐
                  │  data_ptr     │  vtable_ptr   │ kind_tag │
                  └───────────────┴───────────────┴──────────┘
                       8 bytes         8 bytes      1 byte
                                                   (padded to 16)
```

`kind_tag` distinguishes: `0 = borrowed_class`, `1 = owned_class`, `2 = borrowed_struct`. The `owned_struct` case isn't reachable in v1 — structs are always stack-rooted, so an interface value containing a struct is always a borrow.

### Where the kind shows through

The developer doesn't pick a dispatch mode. They write `Iterator<int32> it = ...;` and the compiler handles the rest. But the **lifetime rules differ** by kind, and the compiler error messages call out the underlying when something is rejected:

```cajeta
public Iterator<int32> escapeBug() {
    int32[3] xs = { 1, 2, 3 };       // xs is stack-resident
    return xs.iter();                // returns an ArrayIter<int32>
    // ERROR: interface value rooted in local struct `ArrayIter<int32>`
    //        cannot escape the function. The struct borrows `xs`, which
    //        drops at function exit.
}

public Iterator<int32> ok() {
    return new FileLineIter(...);    // class-rooted, owned interface value
                                      // can be returned freely
}
```

A class-rooted interface value with `owned_class` kind can be transferred (`#it`) and assigned to long-lived fields. A struct-rooted interface value is a borrow of its struct; it follows borrow-tracking rules — can't outlive the struct it borrows from, can't be stored in a heap field, can't be sent to another fiber.

### Cost

| Operation | Class-only world (pre-v1) | Tagged-fat-pointer world |
|---|---|---|
| Interface value size | 1 word (object pointer) | 3 words (data + vtable + tag, 16 bytes after padding) |
| Direct method call on concrete type | 1 indirect call | 1 indirect call (vtable hop) |
| Through-interface method call | vtable hop (load vtable from object header → indirect call) | one fewer load (vtable pre-loaded into the interface value at assignment time) → indirect call |
| Method call on concrete struct type | n/a | direct call, inlinable, zero vtable hop |

Net: interface values get bigger (3 words instead of 1), per-call dispatch gets slightly cheaper (no vtable load from object header), and direct calls on concrete struct types are fully monomorphized.

### Method restrictions on dyn-dispatched calls (v1)

A method called through an interface value cannot:

- **Return `Self`.** The interface value doesn't carry the concrete size; constructing a `Self` from inside a dyn-dispatched call doesn't have a place to land. Direct calls on the concrete type can return `Self` normally.
- **Have its own generic type parameters.** Method-level generics need monomorphization, which requires knowing the concrete type. Interface-typed generics (`Iterator<T>`) on the *receiver* are fine — they're resolved at the interface-value construction site.

Both restrictions apply only to the through-interface path. The same methods are callable directly on the concrete type without restriction.

These can be relaxed later via more elaborate mechanisms (associated-type-style tricks for `Self`, dictionary-passing for method generics), but the v1 contract is strict.

### Errors with concrete examples

```cajeta
// Self-returning through dyn
public interface Cloneable<T> {
    public T clone();
}
public struct Foo implements Cloneable<Foo> {
    public Foo clone() { return Foo { /* copy fields */ }; }
}

Foo f;
Cloneable<Foo> c = f;
Foo other = c.clone();
// ERROR: method `clone` returns `Self`; cannot be dispatched through
//        an interface value. Call directly on the concrete `Foo`.

// Method-level generic through dyn
public interface Searchable {
    public <T> Optional<T> find((Element) -> Optional<T> pred);   // method-level <T>
}
// → this method shape is rejected at interface declaration:
// ERROR: method-level generic type parameters cannot appear on
//        an interface method (v1 limitation; method would not be
//        dispatchable through interface values).
```

---

## Borrow semantics integration

A struct's field accesses follow cajeta's standard path-based borrow tracking. `p.first` is a borrow rooted at `p`; `p.db.socket` is a borrow rooted at `p` through the chain `db → socket`.

The borrow checker enforces:

- **Borrow can't outlive its source.** A reference borrowed from `p.field` cannot be stored anywhere that survives `p`'s scope.
- **Mutating `p` while a borrow into `p.field` is live → static error.** Path-based alias-mutation detection (same machinery the existing path-borrow pass uses).
- **Moved struct fields are unreadable.** `#p.db.socket` moves the socket out; subsequent reads of `p.db.socket` are rejected. The other fields of `p.db` remain readable; the field-path tracker resolves this precisely.

These rules are the same as for any other owner; structs don't introduce new semantics, they just make the rules apply uniformly to value-typed aggregates.

---

## Calling convention

Structs are passed **by pointer** at call sites and return sites. Cajeta's existing pass-by-pointer rule for aggregates (see `Method::generatePrototype`'s `passByPointer` logic) applies uniformly. A function taking `Pair<int32, int32>` receives an `i8*` to a stack-allocated `Pair`; returning a `Pair` is done via a `sret` slot the caller provides.

This is cheaper than memcpy-on-call (especially for larger structs) and matches what C, Rust, and Go do for aggregate types. The user doesn't see the pointer at the source level — `void inspect(Pair p)` reads as pass-by-value semantically.

Implications:

- Mutating a struct parameter mutates the caller's copy? **No.** The compiler emits a defensive copy at the callee's entry if the callee mutates `p`, restoring value semantics. (Optimization: skip the copy when escape analysis proves no aliasing — common case for small structs.)
- Returning a struct from a method that's then immediately bound to a local: the compiler elides the sret slot and writes directly into the caller's local. NRVO-style optimization, no extra copy.

---

## Errors caught statically

| Error | Caught by |
|---|---|
| Recursive struct definition (direct or transitive) | Layout-cycle detection during type registration |
| Variable-tail field (`byte[?]`) in a struct | Layout pass — `struct` requires every field to have compile-time-known size |
| Struct used after move | Path-based borrow tracking (`CAJETA_ERROR_USE_AFTER_MOVE`) |
| Borrow into struct field outliving the struct | Borrow checker scope rule (`CAJETA_ERROR_BORROW_ESCAPE`) |
| Mutating a struct while a field borrow is live | Path-based alias-mutation check |
| `Self`-returning method called through interface value | Interface dispatch check at call site |
| Method-level generic on an interface method | Interface declaration check |
| Struct-rooted interface value stored in a heap class field | Borrow-into-heap check |
| Struct-rooted interface value returned from a function whose source struct is local | Borrow escape check at return |

---

## Implementation outline

For the language implementer:

1. **Parser:** recognize `struct` keyword (already in lexer as `STRUCT`); parse `structDeclaration` with field list + optional method list. Accept `implements` clause same as class.
2. **Type system:** `CajetaStruct` (already exists as sibling of `CajetaClass`); extend with implemented-interface list. Layout pass computes total size, rejects recursive definitions and variable-tail fields.
3. **Inline composition:** when a class field's type is a struct, the class's LLVM layout inlines the struct's LLVM type at that field's offset (instead of a pointer slot). Drop codegen recurses into embedded struct fields in reverse declaration order.
4. **Method codegen:** identical to class methods — emit an LLVM function with `this` as the first parameter (struct pointer). No vtable involvement for direct calls.
5. **Interface vtable synthesis:** for each (struct, interface) pair the struct implements, emit a static vtable global containing function pointers to the struct's method implementations. One vtable per pair (not per instance).
6. **Interface value construction:** at assignment from a struct to an interface-typed variable, build the 3-word value: `{ struct-ptr, vtable-ptr-for-this-(struct,interface), kind_tag=BORROWED_STRUCT }`. From a class, load the class's vtable from its header and build `{ class-ptr, vtable, kind_tag=BORROWED_CLASS or OWNED_CLASS depending on assignment }`.
7. **Interface dispatch:** call site reads `vtable_ptr` (word 2), indexes by method offset, indirect-calls. Same shape regardless of underlying.
8. **Interface value drop:** at scope exit, switch on `kind_tag`. `BORROWED_*` → no action (source owns the data). `OWNED_CLASS` → drop the underlying class.
9. **Borrow tracking for struct-rooted interface values:** the interface value is recorded as a borrow rooted at the struct it was assigned from. Existing path-based borrow checker handles escape detection; the diagnostic points at the underlying struct, not at the interface type.
10. **Method-restriction enforcement:** at every interface-value method call site, reject if the method returns `Self` or has its own type parameters. At interface declaration, reject methods with type parameters of their own.
11. **Calling convention:** struct values pass by pointer at call sites and return sites — already implemented for `CajetaStruct` via `Method::generatePrototype`'s `passByPointer` rule. No changes needed.

---

## Examples

### Named tuple as a local

```cajeta
public struct Pair<A, B> {
    public A first;
    public B second;
}

public static int32 sumPair() {
    Pair<int32, int32> p = Pair { first: 7, second: 11 };
    return p.first + p.second;
}
```

### Optional<T> as a value-typed sum

```cajeta
public struct Optional<T> {
    private boolean present;
    private T       value;

    public static Optional<T> Some(T v) {
        return Optional { present: true, value: v };
    }

    public static Optional<T> None() {
        return Optional { present: false, value: /* zero */ };
    }

    public boolean isSome() { return this.present; }
    public T       unwrap() { return this.value; }
}
```

Each `Optional<T>` is `{ boolean present; T value; }` — one byte plus T-sized payload, on whatever frame holds it. No allocation. `T` can be a primitive, a class reference, or another struct — all work uniformly.

### Iterator as a struct implementing an interface

```cajeta
public interface Iterator<T> {
    public Optional<T> next();
}

public struct ArrayIter<T> implements Iterator<T> {
    private T[]   data;
    private int64 idx;

    public Optional<T> next() {
        if (this.idx >= this.data.length()) return Optional<T>.None();
        T v = this.data[this.idx];
        this.idx += 1;
        return Optional<T>.Some(v);
    }
}

// Direct call (monomorphized):
int32[] xs = { 1, 2, 3 };
ArrayIter<int32> it = xs.iter();
Optional<int32> first = it.next();    // direct call, inlinable

// Through interface (tagged fat pointer):
Iterator<int32> dyn = xs.iter();      // borrowed interface value
                                       // (struct rooted in caller's stack)
Optional<int32> firstDyn = dyn.next(); // vtable dispatch
```

### Embedded struct in a class

```cajeta
public struct ConnectionStats {
    public int64 bytesRead;
    public int64 bytesWritten;
    public int64 requestCount;
}

public class HttpServer {
    private TcpServerSocket   listener;
    private ConnectionStats   stats;            // embedded, 24 bytes inline
    private ArrayList<Fiber>  workers;

    public void recordRead(int64 n) {
        this.stats.bytesRead += n;              // direct field write
    }
}
```

The class's heap allocation is `header + listener-ptr + stats(24 bytes inline) + workers-ptr`. No extra allocation for `stats`. When the `HttpServer` instance drops, `stats` drops with it (no-op for pure-primitive structs; runs the embedded struct's drop logic for ones that hold owned references).
