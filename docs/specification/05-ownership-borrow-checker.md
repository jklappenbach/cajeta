# 5 — Ownership & the Borrow Checker

This chapter defines Cajeta's ownership model — the owned and borrow states — and the static analysis that enforces it. Every heap value has one responsible owner. A plain `=` produces a borrow, a `#` on the source surrenders the title, and `#=` is a passthrough that hands along whatever title its source holds. All enforcement is at compile time, with no ownership annotations in the type system and no reference counting on the owned path.

## 5.1 Ownership States

A binding to a title-bearing value is in one of two states. The state is inferred, and it is never written in source.

| State | Discipline |
|---|---|
| **owned** | Exactly one responsible dropper. `#` passes the title on. |
| **borrow** | Non-owning. Must not outlive its source, checked statically. |

The **title** is the ownership stake in a value, the right and the obligation to drop it. A **borrow** is a reference without the title.

One resolution operates beside the two states without adding a third. An escaping borrow of an immutable leaf buffer — a `String` produced by `substring` is the canonical case — resolves into a copy, or into a shared stake in the backing buffer. A stake is a property of the buffer, not of the binding. A runtime count co-owns the buffer, and the last stake frees it. The binding itself is a borrow. Stakes are specified in Arrays, Views, Slices & Records §12.

## 5.2 The Borrow Operator

`=` with a plain right side borrows. The title stays where it was, and the original owner drops the value when its scope ends. Three cases produce a borrow.

- **A local from a local.** `T b = a` makes `b` a borrow of `a`.
- **A field or element read.** `T b = h.c` makes `b` a borrow rooted at `h` with path `c`. Element reads borrow the same way.
- **A plain call argument.** `f(a)` lends `a` to the callee for the call. The caller keeps the title (§5.5).

**Example 5.2-1.** A local borrowing a local.

```cajeta
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int8[] b = a;           // borrow — the title stays with a
        return (int32) b[0];    // 7
    }
}
```

**Example 5.2-2.** A field read, rooted at the holder.

```cajeta
public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }
public class Holder { public Cell c; public Holder() { this.c = heap Cell(7); } }
public final class C {
    public static int32 run() {
        Holder h = heap Holder();
        Cell b = h.c;           // borrow rooted at h, path c
        return b.v;             // 7
    }
}
```

**Example 5.2-3.** A plain argument lends. The caller still owns the buffer after the call.

```cajeta
public final class C {
    public static int32 peek(int8[] p) { return (int32) p[0]; }
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int32 n = C.peek(a);
        return n + (int32) a[0];    // 14
    }
}
```

A borrow must not outlive its source. The compiler tracks each owner's declaration site and scope end, and each borrow's source. A borrow that may outlive its source is a compile-time error at the use site.

A borrow tracks its *path* from the named root, not only the variable it was read from. After `String n = person.address.city`, the borrow `n` is rooted at `person` with path `address.city`. Reassigning any link along that path — `person.address #= x` — invalidates everything derived from the link, and a subsequent use of `n` is a compile-time error.

Two further rules protect borrows.

- **Anonymous owners.** A chained access whose root is an unnamed temporary, where an intermediate step borrows into the temporary, is a compile-time error. The temporary drops at the end of the expression and the borrow would dangle. Binding the intermediate to a name resolves it.
- **Alias mutation.** A live borrow into a value blocks mutation of, or through, that value's path. Iteration is a borrow construct, so mutating a collection inside its own `for`-each body is a compile-time error.

> *Discussion.* An escaping borrow whose source is an eligible immutable leaf buffer does not error. It resolves into a copy or a shared stake (Arrays, Views, Slices & Records §12). The error-and-`#` discipline described in this chapter is the rule for identity objects and mutable values. At script top level, bindings live in the session scope, and a top-level borrow of a session binding is `CAJETA_ERROR_SESSION_BORROW_ESCAPE` (Script Units §18).

## 5.3 Transfers

A transfer moves the title from one binding to another. There is no transfer operator. The `#` prefix marks the surrender at the source, and it is written wherever the value is handed on.

- **A store.** `a = #b` gives `a` the title `b` held.
- **A call argument.** `f(#b)` gives the callee the title (§5.5).
- **A return.** `return #b` hands the title back to the caller (§5.5.2).

A signature can also require a transfer. A parameter declared `#T` accepts only a surrendered argument (§5.5.1), and a return declared `#T` promises the caller a title (§5.5.2).

After a transfer, the source is demoted to a borrow of the same instance. A previously owning variable remains valid, and reading through it stays legal. When it drops from scope it does not free the memory it once owned. That obligation traveled with the title, and only the new owner's drop fires the destructor. Transferring it again is a compile-time error (§5.6).

A fresh `heap T(...)` expression in transfer position promotes implicitly, with no `#` written. The temporary is an unnamed owner with no prior identity (Allocation §4).

**Example 5.3-1.** A transfer at a store, then a read through the demoted source.

```cajeta
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int8[] b = #a;          // b holds the title, a is demoted to a borrow
        return (int32) a[0];    // 7
    }
}
```

**Example 5.3-2.** A transfer at a call argument. The buffer's title reaches the `Sink`, which outlives the call.

```cajeta
public final class Sink {
    int8[] held;
    public Sink(#int8[] b) { this.held #= b; }
    public int32 first() { return (int32) this.held[0]; }
}
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        Sink s = heap Sink(#a);
        return s.first();       // 7 — the buffer now belongs to s
    }
}
```

**Example 5.3-3.** A transfer at a return, through a `#T` signature. The result is received with `#=` (§5.5.2).

```cajeta
public final class C {
    public static #int8[] make() {
        int8[] out = heap int8[2];
        out[0] = (int8) 7;
        return #out;
    }
    public static int32 run() {
        int8[] q #= C.make();
        return (int32) q[0];    // 7
    }
}
```

**Example 5.3-4.** A rejected program. `b` is a borrow, so it holds no title to surrender (§5.6).

<!-- snippet: skip -->
```cajeta
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int8[] b = a;
        int8[] c = #b;          // CAJETA_ERROR_MOVE_OF_BORROW
        return (int32) c[0];
    }
}
```

> *Discussion.* **Transfer of `stack`-allocated values is TBD.** The transfer machinery is being consolidated, and the semantics of `#` applied to a stack instance will be specified when that work lands. What holds today is that a `#`-return of a stack local is `CAJETA_ERROR_STACK_RETURN_ESCAPES`, and a plain return of a stack value copies into a caller-allocated slot (Allocation §4). Other stack-transfer shapes are unspecified here until the consolidation completes.

## 5.4 The Passthrough Operator

`#=` is a passthrough. It hands along whatever title its source actually holds — a transfer when the source owns, a borrow when it does not. `#=` is a single token, so an ownership store cannot be half-written. When the source owns, the store is a transfer and the source is demoted (§5.3). When the source is a borrow, the destination borrows and nothing is demoted.

The destination may be a local binding, a field, or an element. A field or element store records the arrived mode in the slot's own ownership bit. A `#T` result is received the same way (§5.5.2), and a formal forwards its arrived mode through `#=` (§5.5).

**Example 5.4-1.** A passthrough into a local. The source owns, so the title moves.

```cajeta
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int8[] b #= a;
        return (int32) b[0];    // 7
    }
}
```

**Example 5.4-2.** A passthrough into a field. The constructor's parameter is a must-own edge, so the field records an owned slot.

```cajeta
public final class Sink {
    int8[] held;
    public Sink(#int8[] b) { this.held #= b; }
    public int32 first() { return (int32) this.held[0]; }
}
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        Sink s = heap Sink(#a);
        return s.first();       // 7
    }
}
```

**Example 5.4-3.** A passthrough into an element. The slot records the arrived mode.

```cajeta
public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }
public final class C {
    public static int32 run() {
        Cell[] arr = heap Cell[2];
        Cell c = heap Cell(7);
        arr[0] #= c;
        return arr[0].v;        // 7
    }
}
```

## 5.5 Calls and Mode Forwarding

The call site decides ownership. `f(x)` lends and `f(#x)` transfers. The callee is told what it got — every class-typed parameter and return carries a hidden per-call ownership flag, whose encoding is the Runtime & ABI companion's concern — so a formal is a *runtime* owner, neither statically a borrow nor statically an owner, but whichever the caller made it. Its drop entry is armed from the flag on entry. A local initialized from a call arms from the return flag the same way.

Three consequences follow.

1. A surrendered argument the callee never consumes drops in the callee, on whatever exit the callee takes, including a `throw`.
2. A lent argument leaves the callee's drop entry disarmed, and the caller still owns it.
3. `#p` on a formal — at a store, a forward, or a return — consumes the formal and passes the *arrived* mode on. A forwarding chain threads the original caller's decision all the way down. A value lent into `outer` and forwarded with `#` to `inner` arrives at `inner` still lent, and nobody frees it.

This is what makes mode-forwarding wrappers expressible, and it is why the move-of-borrow check (§5.6) deliberately excludes plain formals.

**Example 5.5-1.** A mode-forwarding wrapper. `keep` forwards whichever mode arrived, and `run` transfers, so the buffer's title reaches the `Sink`.

```cajeta
public final class Sink {
    int8[] held;
    public Sink(#int8[] b) { this.held #= b; }
    public int32 first() { return (int32) this.held[0]; }
}
public final class C {
    public static int32 keep(int8[] p) {
        Sink s = heap Sink(#p);   // forwards the arrived mode
        return s.first();
    }
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        return C.keep(#a);        // 7 — the caller surrendered the title
    }
}
```

### 5.5.1 Must-own parameters

A parameter type may be spelled `#T`. This is an opt-in must-own edge for a method that cannot function with a borrow, because it stores the value somewhere that outlives the call and has no way to cope with the caller keeping the title. Passing a plain argument at such an edge is a compile-time error.

**Example 5.5-2.** A `#T` parameter, satisfied by a transfer.

```cajeta
public final class C {
    public static int32 take(#int8[] p) { return (int32) p[0]; }
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        return C.take(#a);        // 7
    }
}
```

**Example 5.5-3.** A rejected program. A plain argument at a `#T` parameter.

<!-- snippet: skip -->
```cajeta
public final class C {
    public static int32 f(#int8[] p) { return (int32) p[0]; }
}

int8[] a = heap int8[4];
a[0] = (int8) 5;
System.stdout.println(C.f(a));    // CAJETA_ERROR_TRANSFER_REQUIRED
```

Transfer mode is not part of a signature — dispatch erases `#` — so two declarations that differ only in mode collide. Declaring both `f(Cell c)` and `f(#Cell c)` is `CAJETA_ERROR_TRANSFER_MODE_OVERLOAD`. A plain formal already accepts both a lend and a transfer.

### 5.5.2 Returns

A plain `return x` hands back whatever title `x` held. A lent value stays lent, and an owned one transfers. `return #x` hands back the title.

A method may declare its return type `#T`, promising the caller a title. A `#T` result must be received with `#=`, which is what makes the acquisition visible at the call site without opening the callee. Binding it with a plain `=` is a compile-time error. A plain-`T` result binds with a plain `=` and carries whatever mode the callee's frame held.

**Example 5.5-4.** A rejected program. A `#T` result bound with `=`.

<!-- snippet: skip -->
```cajeta
public final class C {
    public static #int8[] make() {
        int8[] out = heap int8[2];
        out[0] = (int8) 3;
        return #out;
    }
}

int8[] q = C.make();    // CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER
System.stdout.println(q[0]);
```

The receiving local's *type* never carries the marker. `#Point q = …` is a compile-time error (`CAJETA_ERROR_TYPE_TRANSFER_RETIRED`), because a local's role comes from its initializer.

> *Discussion.* Nothing in user code reads the per-call flag positionally, and the retired `Cajeta.moveMask()` accessor is rejected with `CAJETA_ERROR_MOVEMASK_RETIRED`. Code that branches on a formal's arrived mode calls `Cajeta.owned(formal)`. Ownership is never inferred from the body. A lend at a local's last use is often a transfer the author did not spell, and the compiler advises (§5.10) rather than guesses, because the guess is wrong exactly where it matters — a value handed to a spawned task outlives the frame that appears finished with it.

## 5.6 Restrictions on Transfer

`#` hands along a title, so applying it to a value the compiler can prove holds no title is a compile-time error, `CAJETA_ERROR_MOVE_OF_BORROW`. The proven-borrow cases are

- a local that borrows another local (`T b = a; … #b`)
- a local bound to a borrow returned by a plain (non-`#`) method
- a source that has already been transferred, since transferring twice is transferring from a borrow (§5.3)

Plain formals are excluded deliberately. Their mode is a runtime fact fixed at the call site (§5.5), and rejecting `#p` statically would outlaw every mode-forwarding wrapper.

**Example 5.6-1.** A rejected program. A second transfer from the same source.

<!-- snippet: skip -->
```cajeta
public final class C {
    public static int32 run() {
        int8[] a = heap int8[4];
        a[0] = (int8) 7;
        int8[] b #= a;
        int8[] c #= a;      // CAJETA_ERROR_MOVE_OF_BORROW
        return (int32) c[0];
    }
}
```

> *Discussion.* At script top level the store-form check does not yet fire. The two stores of Example 5.6-1, written as loose statements, compile today. The gap is recorded as disabled pinning tests in `test/jit/SessionBindingTests.cpp`, and this section governs once they pass.

## 5.7 Escapes and Retention

A plain store and a plain argument lend. Two rules keep a lend from outliving its source.

**Dangling lend.** If a method lends a local into a receiver that *retains* it, storing it into a field, and the receiver then escapes the method, the escape is a compile-time error. The receiver would leave holding a pointer to a local that is about to drop. The check fires only when the callee actually retains, because a method that merely reads its argument cannot strand anything and does not poison its receiver. The fix is to say what was meant. `h.c #= s` gives the holder the title.

**Example 5.7-1.** A rejected program. A retained lend escapes.

<!-- snippet: skip -->
```cajeta
public class Cell { public int32 v; public Cell(int32 v) { this.v = v; } }
public class Holder {
    public Cell c;
    public Holder() { this.c = heap Cell(0); }
}
public final class C {
    public static #Holder build() {
        Holder h = heap Holder();
        Cell s = heap Cell(5);
        h.c = s;              // lend, the title stays with s
        return #h;            // CAJETA_ERROR_DANGLING_LEND — s drops at }
    }
}
```

**Captured borrow parameter.** A plain store of a plain formal into a field or element is a compile-time error, `CAJETA_ERROR_CAPTURED_BORROW_PARAM`. The field would borrow, while the formal's armed drop entry frees the value at callee exit on exactly the calls that surrendered it. Spell `this.f #= v` to record whatever title arrived, or `this.f = v.clone()` to keep a copy. Stores this check cannot reach — a nested path such as `this.head.prev = v`, or a source that is a runtime-conditional owner rather than a formal — warn instead (`CAJETA_WARN_PLAIN_RETAIN_STORE`).

**Example 5.7-2.** A rejected program. A plain formal captured by a field.

<!-- snippet: skip -->
```cajeta
public final class Holder {
    int8[] f;
    public Holder() { this.f = heap int8[1]; }
    public void set(int8[] v) {
        this.f = v;           // CAJETA_ERROR_CAPTURED_BORROW_PARAM
    }
}
```

## 5.8 Field Ownership and the Live-Set Claim

A field's ownership status is resolved at drop time, not at declaration. `p.field #= x` and `p.field = heap T(...)` make the field an owner. `p.field = y`, where `y` is a borrow, stores the borrow, and the field aliases `y`'s source.

Every `heap` allocation is registered in a global live set. When an owner drops, the synthesized drop body offers each owned-shape field to its drop dispatcher, and the dispatcher makes an atomic *claim* — remove-if-present — on the field's address. The first claimant frees the value and runs its destructor. Every later claimant for the same address, whether the owning local's own drop or another field aliasing it, finds it gone and does nothing. The claim is what keeps aliased fields from double-freeing without reference counts.

> *Discussion.* A use after free through an aliased field whose source has already dropped is the programmer's responsibility in v1. A static lifetime tracker for that case is planned but not implemented.

## 5.9 Drops Under Transfer

Transfer moves the obligation to drop. When `#` passes a title, the source's drop entry is deactivated and the destination's is armed. Each instance's destructor runs exactly once, when the drop chain reaches whichever entry ends up owning it. Drop entries fire at the closing brace of the declaring lexical block, in reverse declaration order, on both the normal and the exceptional path (Allocation §4). Destructor declaration and dispatch are specified in Classes §8.

## 5.10 Advisories

Lending a local at its final use is suspect. Nothing in the scope reads it again, so the lend is usually a transfer the author did not spell. The compiler reports `CAJETA_WARN_LAST_USE_TRANSFER` with a `#` fixit and continues. A later read of the local suppresses the warning. So does a use inside a loop, and so does spelling `#x`.
