# Borrow soundness — detection beyond the type system

## Motivation

[`OwnershipTransfer`](OwnershipTransfer.md) shipped the two-sided
ownership model: a callee declares `#T` on a formal to require an
owning transfer, a caller writes `#x` at the call site to surrender
ownership, and the compiler enforces matched declarations through
Phase 2 (mismatch rejection) and Phase 3a (borrow params can't escape
via `return` or via `#x` to another call).

Two body-side checks were deliberately deferred:

- **Field-store of borrow** — `this.f = param;` where `param` is a
  plain-`T` (borrow) formal and `f` is a class-typed field.
- **Closure-capture of borrow** — a closure captures a class-typed
  local or borrow param into its environment, and the closure value
  itself outlives the captured source.

The reason both stay deferred is the **index / cache / view collection
pattern**. A `HashMap<K, V>` used as a lookup table alongside a
primary owner *must* be allowed to field-store plain-`T` values; the
primary owner manages lifetime, the index points at it. Forcing
`#K` / `#V` on `Pair`, `LinkedListNode`, `RedBlackNode` would collapse
the pattern — the index can't both own and not own.

The caller-side `#x` syntax is the sound escape hatch: a developer who
*does* want the container to own writes `cache.put("k", #f);`. The
problem is the silent case — `cache.put("k", f);` is accepted, and
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
borrow's source. Both compile, both UAF.

## Static detection (lint mode)

### S1 — same-scope escape analysis

When **both** the source local and the storage receiver are visible in
the current scope, the compiler can verify lifetime ordering. Declaration
order in a scope determines drop order in reverse (LIFO drop chain).

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
formal (this requires either a callee-side annotation — see S2 — or
a whole-method body analysis). For each such call, compare the
declaration index of receiver vs. argument in the **common enclosing
scope**. If the receiver was declared first, flag.

**Coverage.** Catches the toy cases and the most common "I forgot to
write `#`" bug. Cannot reason across method boundaries when the
receiver is itself a returned value or a field.

**False positives.** None in the strict same-scope form — the LIFO drop
order is deterministic.

**False negatives.** Cross-method receivers (`getCache().put("k", f)`),
fields-of-fields, anything where the receiver's true lifetime isn't
locally derivable.

### S2 — `@stores` callee annotation

Provide a lightweight callee-side marker — provisional spelling
`@stores` — that says "this formal will be parked in a longer-lived
slot, but ownership stays with the caller." Distinct from `#T` (which
transfers) and from plain `T` (which is a use-only borrow).

```cajeta
public class Cache {
    public void put(String k, @stores Foo v) {   // borrow + stored
        this.entries.put(k, v);
    }
}
```

At the call site, the lint applies the same scope-ordering check as S1
but **per formal**, driven by the annotation. The annotation also
documents the lifetime contract to readers — "this method retains
your value past the call."

**Why not just promote to `#T`?** Because `#T` *transfers* — the
caller surrenders ownership, the caller's drop chain stops tracking
the value. `@stores` doesn't transfer; the caller keeps owning, the
callee parks a borrow. Two genuinely different contracts.

**Coverage.** Generalizes S1 across one method boundary explicitly.
Composes: an `@stores` formal passed into a deeper `@stores` formal
threads the contract through.

**Cost.** New annotation in grammar, parser, and the formal-parameter
machinery. Stdlib sweep to mark genuine index-collection writers
(`HashMap.put`, `LinkedList.append`, etc.) so the lint has something
to check against.

### S1c — captured-and-escaped closure detection

A closure that **captures a borrow** and is **also escaped** (returned,
field-stored, posted to `Tasks.spawn`, sent through a `Channel`) is
flagged. The capture is trivially detectable — the compiler synthesizes
the env record and knows which captures are borrows. The escape is the
harder half:

- **Returned from a function** — directly observable from the AST.
- **Stored in a field** — observable; same shape as S1 field-store but
  for the closure value itself.
- **Passed to a `(closure) -> void` formal** — flag conservatively; a
  refinement opts the formal into "invoked synchronously, doesn't
  escape" via the same `@stores`-style marker (`@calls` or similar).

**Coverage.** The `makeReader` toy case is flagged because the closure
is returned. The cross-task `pool.spawn(() -> { ... })` case is
flagged because `spawn` is not annotated as a synchronous invoker.

**False positives.** Synchronous closure consumers (e.g. `stream.map`,
`forEach`) need the opt-out marker to not flag. Tractable — small
number of methods in the stdlib.

### S2c — capture-transfer syntax

Provide the closure analog of caller-side `#x` — a capture clause that
surrenders ownership into the closure environment.

```cajeta
// Capture list — explicit ownership transfer into env
return [#f] (int32 _) -> { return f.v; };
```

Inside the closure body, `f` is owned by the environment; the source
binding's drop is deactivated, exactly as `obj.foo(#f)` deactivates it
for the call. The closure's own drop becomes responsible for `f`'s
release.

**Coverage.** Gives developers a path to write the sound version of
the `makeReader` shape without lifetime annotations.

**Cost.** Grammar addition for the `[#x, #y]` capture list. Codegen
to thread captured-owned fields into the closure's drop function.

## Runtime detection (debug builds)

Static lint is conservative. Runtime checks are precise — they fire
only when an actual UAF occurs, at the cost of instrumentation overhead
acceptable only in debug builds.

### R1 — heap allocation generation counters

Every `heap` allocation carries a 64-bit generation counter at a fixed
offset. Borrow handles capture both the pointer and the generation at
capture time. Dereference through a borrow handle checks the captured
generation against the allocation's current generation; on mismatch,
trap with a clear message.

```
heap allocation header (debug build):
    [ refcount? | generation: u64 | type-tag | ... ]

borrow handle (debug build):
    { ptr, generation_snapshot }
```

When the allocation is freed, the slot is either zeroed (subsequent
read of the generation field returns 0, mismatching any captured
snapshot) or quarantined (the slot is kept, the generation is bumped
on reuse).

**Coverage.** Catches every UAF — field-store, closure-capture,
double-free, any handle-after-free. Independent of whether the lint
flagged it statically.

**Cost.** Per-allocation memory (1 word for the generation), per-deref
check (a load + compare + branch). Behind a debug-build flag, similar
to `-fsanitize=address` opt-in.

### R2 — drop-time borrow registry

Each heap allocation maintains a list of outstanding non-`#` references
("borrow registry"). On `put(String, Foo)` where `Foo v` is plain-`T`,
the runtime registers the cache as a holder of `v`'s identity. When
`v` drops, assert the registry is empty; if not, dump the holders with
synthesized source locations.

**Coverage.** Catches the field-store case at the moment of drop,
*before* the dangling pointer is read. Gives a clean "drop of `f` left
a live reference inside `c.entries`" diagnostic.

**Cost.** Higher than R1 — every borrow store needs registry update,
every drop walks the registry. Probably the right add-on for targeted
investigation, not the default.

### R3 — quarantine + poison after drop

On drop, don't return the heap allocation to the allocator immediately.
Overwrite with a poison pattern (`0xDEADBEEF...`) and hold in a
quarantine pool. Subsequent dereferences read the poison and trap.

**Coverage.** Same as R1 but catches reads against the freed memory
instead of catching them at the deref check. Useful when the deref
path bypasses the borrow handle (e.g. raw pointer escape in unsafe
code).

**Cost.** Memory — quarantine pool grows. Bounded by a high-water
mark with eviction once full.

## Recommended phasing

The two static checks at the cheap end pair with one runtime check at
the cheap end. Each lands independently; none of them block reference
types.

1. **P1 — S1 same-scope escape lint.** Compile-time, no annotation,
   no false positives in its strict form. Cheap to implement; catches
   the most common "forgot `#`" bug. Wired behind a `--lint=borrow`
   flag during stabilization, on-by-default after.

2. **P2 — R1 generation counters in debug builds.** Behind
   `-fsanitize=cajeta-borrow` (or equivalent). Catches everything the
   lint misses without affecting release builds. Mirrors what
   ASan / Rust debug_assertions / Go's race detector give.

3. **P3 — S2 `@stores` annotation + S2c capture-transfer syntax.**
   Grammar and stdlib sweep. Generalizes the static check across one
   method boundary and gives developers the sound closure shape.

4. **P4 — S1c captured-and-escaped detection.** Builds on the
   `@stores` / `@calls` distinction from P3 to avoid false positives
   on synchronous closure consumers.

5. **P5 — reference types arrive.** S1/S1c become enforced rather than
   linted; `@stores` becomes a thin sugar over reference types with a
   scope/owner lifetime; R1 stays as a debug aid for the unsafe edges.

## Open questions

- **`@stores` vs scoped-reference types.** Is the lightweight annotation
  worth shipping if reference types are coming anyway? Argument for
  yes: it gives a usable contract today and the migration is mechanical.
  Argument for no: it grows the language by one annotation that
  becomes vestigial. Probably ship — the value of the contract over the
  intervening period outweighs the migration cost.

- **R1 generation width.** 64 bits is overkill; 32 might suffice if we
  accept wraparound risk over long-running processes. Defer until R1 is
  prototyped.

- **Lint default-on threshold.** S1 is precise enough to default on
  immediately. S1c needs the `@calls` opt-out to be deployed across
  stdlib first.

- **Interaction with `Tasks.spawn` and `Channel.send`.** Both move
  values across thread boundaries; the closure-escape check needs to
  understand them as escapes. This is straightforward (mark
  `Tasks.spawn(Runnable)` as escape-flow on its formal) but requires
  the stdlib sweep.

## See also

- [`OwnershipTransfer`](OwnershipTransfer.md) — the two-sided ownership
  model these checks extend. Phase 3b / 3c there cross-reference
  back to this doc.
- [`MemoryModel`](MemoryModel.md) — the broader borrow / ownership /
  drop chain model.
- [`FieldOwnership`](FieldOwnership.md) — how fields participate in
  the drop chain.
