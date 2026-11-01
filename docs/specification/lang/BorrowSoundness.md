# Borrow soundness — detection beyond the type system

> **AMENDED (2026-07-03, [`slice-spec.md`](slice-spec.md)):** the `shared` state adds a third
> resolution for an escaping borrow of an immutable slice backing. Instead of rejecting,
> the compiler resolves. It copies a small one, shares a large immutable buffer, or copies
> always when the buffer is arena-backed. The tentative `refcount?` header slot of this doc's
> sketch is now real, as a side-table count keyed by buffer base plus a count-word sign bit.
> The resolution analysis must be a sound over-approximation, so unsure ⇒ resolve, and it
> never silently borrows. Over-pinning is a wasted count, and under-pinning is a UAF.
> Identity/mutable objects keep the rejection discipline described below unchanged.

## Motivation

[`OwnershipTransfer`](OwnershipTransfer.md) shipped the two-sided
ownership model. A callee declares `#T` on a formal to require an
owning transfer, a caller writes `#x` at the call site to surrender
ownership, and the compiler enforces matched declarations through
Phase 2, which rejects mismatches, and Phase 3a, under which borrow params
cannot escape via `return` or via `#x` to another call.

One of the two body-side checks has since shipped, and the other is still deferred:

- **Field-store of a parameter.** `this.f = param;` where `param` is a
  `T` or `#T` formal. It is now rejected by the field-store rule
  (`specs/field-store-ownership-spec.md`) with
  `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`. Store with `#=` so the caller
  chooses, which is the `ArrayList` model of §2.3, or spell the formal `^T`
  when it is only ever borrowed. Spelling it `#T` alone is not a fix, because a `#T`
  formal frees its argument at return. The rule covers `this.f = p`,
  `this.arr[i] = p` and a nested path (`this.head.prev = p`). The same rule
  rejects a plain `=` of an owned local or of a value read out of one, and it
  treats a local holder that escapes as a field. A forward into
  a container call (`this.entries.put(k, v)`) is a plain argument and stays
  allowed, so the shapes discussed below remain unlinted.
- **Closure-capture of borrow.** A closure captures a class-typed
  local or borrow param into its environment, and the closure value
  itself outlives the captured source.

The reason both stay deferred is the index / cache / view collection
pattern. A `HashMap<K, V>` used as a lookup table alongside a
primary owner must be allowed to field-store plain-`T` values. The
primary owner manages lifetime, and the index points at it. Forcing
`#K` / `#V` on `Pair`, `LinkedListNode`, `RedBlackNode` would collapse
the pattern, because the index cannot both own and not own.

The caller-side `#x` syntax is the sound escape hatch. A developer who
does want the container to own writes `cache.put("k", #f);`. The
problem is the silent case. `cache.put("k", f);` is accepted, and
when `f` drops before `cache` does, the cache holds a dangling slot.

This doc lays out detection strategies that do not require a full
reference / lifetime type system.

## The two shapes, side by side

```cajeta
// Field-store of borrow — accepted today, UAF at scope exit
public static int32 cacheUaf() {
    Cache c = heap Cache();
    {
        Foo f = heap Foo(42);
        c.put("k", f);             // borrow into field
    }                              // f drops here; cache slot dangles
    return c.get("k").v;           // UAF
}

// Closure-capture of borrow — accepted today, UAF at call
public static (int32) -> int32 makeReader() {
    Foo f = heap Foo(99);
    return (int32 _) -> { return f.v; };   // env captures &f
}                                          // f drops; closure dangles
```

Both write a borrow into a container whose drop is independent of the
borrow's source. Both compile, and both UAF.

## Static detection (lint mode)

### S1 — same-scope escape analysis

When both the source local and the storage receiver are visible in
the current scope, the compiler can verify lifetime ordering. Declaration
order in a scope determines drop order in reverse, through the LIFO drop chain.

```cajeta
Cache c = heap Cache();        // drops second
Foo f = heap Foo(42);          // drops first
c.put("k", f);                 // SUSPECT — c outlives f, store outlives source
```

vs.

```cajeta
Foo f = heap Foo(42);          // drops second
Cache c = heap Cache();        // drops first
c.put("k", f);                 // SOUND — c drops before f, no dangle at f's drop
```

The lint walks each call site whose callee field-stores a plain-`T`
formal, which requires either a callee-side annotation, as in S2, or
a whole-method body analysis. For each such call, it compares the
declaration index of receiver and argument in the common enclosing
scope. If the receiver was declared first, it flags.

**Coverage.** It catches the toy cases and the most common "I forgot to
write `#`" bug. It cannot reason across method boundaries when the
receiver is itself a returned value or a field.

**False positives.** There are none in the strict same-scope form, because
the LIFO drop order is deterministic.

**False negatives.** Cross-method receivers such as `getCache().put("k", f)`,
fields-of-fields, and anything where the receiver's true lifetime is not
locally derivable.

### S2 — `@stores` callee annotation

Provide a lightweight callee-side marker, with the provisional spelling
`@stores`, that says "this formal will be parked in a longer-lived
slot, but ownership stays with the caller." It is distinct from `#T`, which
transfers, and from plain `T`, which is a use-only borrow.

```cajeta
public class Cache {
    public void put(String k, @stores Foo v) {   // borrow + stored
        this.entries.put(k, v);                  // parks the borrow
    }
}
```

`HashMap.put` takes plain `K`, `V`, so this body compiles today and parks a
borrow in the map, which is exactly the hazard `@stores` targets. If the
caller's `v` dies before the `Cache` does, the entry dangles, and no compiler
check diagnoses it. `@stores` is what would let the lint see the contract at
the boundary, for a stdlib container and a plain user field alike.

At the call site, the lint applies the same scope-ordering check as S1
but per formal, driven by the annotation. The annotation also
documents the lifetime contract to readers, which is "this method retains
your value past the call."

> **Partly shipped as `^T`.** A borrow-only formal (`void keep(^Foo v)`) now
> states this contract in the signature. A caller cannot transfer into it, and
> the callee may keep it with a plain `=`. It carries no lifetime check, so the
> call-site ordering lint below is still unbuilt.

**Why not just promote to `#T`?** Because `#T` transfers. The
caller surrenders ownership, and the caller's drop chain stops tracking
the value. `@stores` does not transfer. The caller keeps owning, and the
callee parks a borrow. These are two genuinely different contracts.

**Coverage.** It generalizes S1 across one method boundary explicitly.
It composes, since an `@stores` formal passed into a deeper `@stores` formal
threads the contract through.

**Cost.** A new annotation in the grammar, the parser, and the formal-parameter
machinery, plus a stdlib sweep to mark genuine index-collection writers
(`HashMap.put`, `LinkedList.append`, etc.) so the lint has something
to check against.

### S1c — captured-and-escaped closure detection

A closure that captures a borrow and is also escaped, whether returned,
field-stored, posted to `Tasks.spawn` or sent through a `Channel`, is
flagged. The capture is trivially detectable, because the compiler synthesizes
the env record and knows which captures are borrows. The escape takes more
work to detect:

- **Returned from a function** is directly observable from the AST.
- **Stored in a field** is observable, and it has the same shape as the S1
  field-store but for the closure value itself.
- **Passed to a `(closure) -> void` formal** is flagged conservatively. A
  refinement opts the formal into "invoked synchronously, doesn't
  escape" via the same `@stores`-style marker, `@calls` or similar.

**Coverage.** The `makeReader` toy case is flagged because the closure
is returned. The cross-task `pool.spawn(() -> { ... })` case is
flagged because `spawn` is not annotated as a synchronous invoker.

**False positives.** Synchronous closure consumers such as `stream.map` and
`forEach` need the opt-out marker to not flag. That is tractable, since there
is a small number of such methods in the stdlib.

### S2c — capture-transfer syntax

Provide the closure analog of caller-side `#x`, a capture clause that
surrenders ownership into the closure environment.

```cajeta
// Capture list — explicit ownership transfer into env
return [#f] (int32 _) -> { return f.v; };
```

Inside the closure body, `f` is owned by the environment, and the source
binding's drop is deactivated, exactly as `obj.foo(#f)` deactivates it
for the call. The closure's own drop becomes responsible for `f`'s
release.

**Coverage.** It gives you a path to write the sound version of
the `makeReader` shape without lifetime annotations.

**Cost.** A grammar addition for the `[#x, #y]` capture list, and codegen
to thread captured-owned fields into the closure's drop function.

## Runtime detection (debug builds)

Static lint is conservative. Runtime checks are precise, because they fire
only when an actual UAF occurs, at the cost of instrumentation overhead
acceptable only in debug builds.

### R1 — heap allocation generation counters

Every `heap` allocation carries a 64-bit generation counter at a fixed
offset. Borrow handles capture both the pointer and the generation at
capture time. Dereference through a borrow handle checks the captured
generation against the allocation's current generation, and on mismatch
it traps with a clear message.

```
heap allocation header (debug build):
    [ refcount? | generation: u64 | type-tag | ... ]

borrow handle (debug build):
    { ptr, generation_snapshot }
```

When the allocation is freed, the slot is either zeroed, so a subsequent
read of the generation field returns 0 and mismatches any captured
snapshot, or quarantined, so the slot is kept and the generation is bumped
on reuse.

**Coverage.** It catches every UAF, whether field-store, closure-capture,
double-free or any handle-after-free. It is independent of whether the lint
flagged it statically.

**Cost.** Per-allocation memory of 1 word for the generation, and a per-deref
check of a load, a compare and a branch. It sits behind a debug-build flag,
similar to the `-fsanitize=address` opt-in.

### R2 — drop-time borrow registry

Each heap allocation maintains a list of outstanding non-`#` references,
the "borrow registry". On `put(String, Foo)` where `Foo v` is plain-`T`,
the runtime registers the cache as a holder of `v`'s identity. When
`v` drops, it asserts the registry is empty, and if not, it dumps the
holders with synthesized source locations.

**Coverage.** It catches the field-store case at the moment of drop,
before the dangling pointer is read. It gives a clean "drop of `f` left
a live reference inside `c.entries`" diagnostic.

**Cost.** Higher than R1, because every borrow store needs a registry update
and every drop walks the registry. It is probably the right add-on for
targeted investigation, and it should stay off by default.

### R3 — quarantine + poison after drop

On drop, do not return the heap allocation to the allocator immediately.
Overwrite it with a poison pattern such as `0xDEADBEEF...` and hold it in a
quarantine pool. Subsequent dereferences read the poison and trap.

**Coverage.** The same as R1, but it catches reads against the freed memory
instead of catching them at the deref check. It is useful when the deref
path bypasses the borrow handle, as with a raw pointer escape in unsafe
code.

**Cost.** Memory, because the quarantine pool grows. It is bounded by a
high-water mark with eviction once full.

> **Partly shipped.** The poison primitive already exists. The free path
> can overwrite a reclaimed block with `0xDB` when `__cajeta_set_poison_free`
> is on (`__cajeta_poison_buffer` in `cajeta_runtime.c`). What is not built
> is the quarantine. Today the block is poisoned and then immediately
> returned to the allocator rather than held back, so a poisoned read only
> traps until the address is recycled.

## Recommended phasing

The two static checks at the cheap end pair with one runtime check at
the cheap end. Each lands independently, and none of them block reference
types.

1. **P1 — S1 same-scope escape lint.** It is compile-time, needs no
   annotation, and has no false positives in its strict form. It is cheap
   to implement and catches the most common "forgot `#`" bug. It is wired
   behind a `--lint=borrow` flag during stabilization, and on by default after.

2. **P2 — R1 generation counters in debug builds.** It sits behind
   `-fsanitize=cajeta-borrow` or the equivalent. It catches everything the
   lint misses without affecting release builds, and it mirrors what
   ASan, Rust debug_assertions and Go's race detector give.

3. **P3 — S2 `@stores` annotation + S2c capture-transfer syntax.**
   Grammar and a stdlib sweep. It generalizes the static check across one
   method boundary and gives you the sound closure shape.

4. **P4 — S1c captured-and-escaped detection.** It builds on the
   `@stores` / `@calls` distinction from P3 to avoid false positives
   on synchronous closure consumers.

5. **P5 — reference types arrive.** S1/S1c become enforced rather than
   linted, `@stores` becomes a thin sugar over reference types with a
   scope/owner lifetime, and R1 stays as a debug aid for the unsafe edges.

## Open questions

- **`@stores` vs scoped-reference types.** Is the lightweight annotation
  worth shipping if reference types are coming anyway? The argument for
  yes is that it gives a usable contract today and the migration is
  mechanical. The argument for no is that it grows the language by one
  annotation that becomes vestigial. We should probably ship it, because the
  value of the contract over the intervening period outweighs the migration cost.

- **R1 generation width.** 64 bits is overkill, and 32 might suffice if we
  accept wraparound risk over long-running processes. Defer this until R1 is
  prototyped.

- **Lint default-on threshold.** S1 is precise enough to default on
  immediately. S1c needs the `@calls` opt-out to be deployed across
  stdlib first.

- **Interaction with `Tasks.spawn` and `Channel.send`.** Both move
  values across thread boundaries, so the closure-escape check needs to
  understand them as escapes. This is straightforward, since it marks
  `Tasks.spawn(Runnable)` as escape-flow on its formal, but it requires
  the stdlib sweep.

## See also

- [`OwnershipTransfer`](OwnershipTransfer.md) is the two-sided ownership
  model these checks extend. Phase 3b / 3c there cross-reference
  back to this doc.
- [`MemoryModel`](MemoryModel.md) is the broader borrow / ownership /
  drop chain model.
- [`FieldOwnership`](FieldOwnership.md) covers how fields participate in
  the drop chain.
