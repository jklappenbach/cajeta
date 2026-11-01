# Field Ownership and Auto-Drop

Status: design draft, 2026-05-17. It was triggered when the auto-field-drop
landing (MemoryModel.md § Known gaps, item "No automatic field drops")
regressed stdlib tests for `ArrayStream<T>`, `Optional<T>` and `Pair<K,V>`.

The outcome is that the prior spec rule "borrows in fields are a static
error" is dropped. Fields may legitimately hold borrows, and auto-drop
discriminates owner from alias at runtime by consulting the per-fiber drop chain.

## Background

`MemoryModel.md` originally laid out a strict single-owner-fields doctrine:

- Every heap allocation has exactly one owning slot.
- That slot is registered in the per-fiber drop chain at allocation time.
- When the slot's scope exits, the drop chain runs the slot's destructor
  and frees the body.
- Old rule, MemoryModel.md ~line 102: "Field assignment transfers.
  `p.field = x` must transfer: either `p.field #= x` (explicit) or
  `p.field = heap T(...)` (auto-promoted). Plain `p.field = y` where `y`
  is a named borrow is a static error."
- Old rule, MemoryModel.md ~line 267: "Fields are owners. Borrows in
  fields are a static error."

The rule existed because, if every class-typed field is statically
guaranteed to be the sole owner, auto-drop is unconditionally safe. The
compiler emits "free each field" with no runtime discrimination required.

The stdlib does not follow this rule. `ArrayStream<T>.data` aliases
`ArrayList<T>.data`. `Optional<T>.value` aliases the constructor
argument. `Pair<K,V>.first/second` likewise. These patterns are
necessary. `xs.stream()` cannot transfer the buffer, because xs must remain
iterable, and `Optional<Hello>` storing `#h` would consume the local.

The decision in this doc is to relax the spec, allow borrows in fields,
and let the runtime drop chain self-discriminate.

## Problem

There are four use cases. The first three motivate "borrows-in-fields are
legitimate," and the fourth is the case auto-drop must still handle.

### Use case 1: Iterator/Stream over a borrowed buffer

```cajeta
public class ArrayList<T> {
    T[] data;
    int32 size;

    public ArrayStream<T> stream() {
        return heap ArrayStream<T>(this.data, this.size);
    }
}

public class ArrayStream<T> extends Stream<T> {
    T[] data;        // aliases ArrayList.data
    int32 idx;
    int32 limit;

    public ArrayStream(^T[] data, int32 limit) {   // only ever a borrow
        this.data = data;
        this.idx = 0;
        this.limit = limit;
    }
}
```

Caller:

```cajeta
ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(1); xs.add(2); xs.add(3);
ArrayStream<int32> s #= xs.stream();
while (s.next().isPresent()) { ... }
// Both `s` and `xs` drop at scope exit.
```

Under naive auto-drop, which drops every field unconditionally, ArrayStream
frees the buffer when it pops, and ArrayList frees it again when it pops.
That is a double free.

### Use case 2: Optional wrapping a class instance

```cajeta
public class Optional<T> {
    boolean present;
    T value;
    public Optional(boolean p, T v) { this.present = p; this.value #= v; }
}

// caller
Hello h = heap Hello();
Optional<Hello> opt = heap Optional<Hello>(true, h);
print(h.greet());      // expected: still works
```

`opt.value` aliases `h`. The caller lent `h`, so the `#=` store records a
borrow. Naive auto-drop frees Hello when opt pops,
and h's own chain entry frees it again. That is a double free.

There is a template wrinkle. `Optional<int32>` has nothing to drop, and
`Optional<Hello>` does. The class declaration is the same and the
instantiations differ, so the field discriminator cannot be syntactic.

### Use case 3: Pair holding two references

```cajeta
public class Pair<K, V> {
    K first;
    V second;
    public Pair(K a, V b) { this.first #= a; this.second #= b; }
}

Hello a = heap Hello();
Hello b = heap Hello();
Pair<Hello, Hello> p = heap Pair<Hello, Hello>(a, b);
print(a.greet());
print(b.greet());
```

This is the same shape as Optional, with two fields. Naive auto-drop double-frees both.

### Use case 4: Single-owner field (auto-drop must handle this)

```cajeta
public class Tracer {
    public ~Tracer() { /* user-defined cleanup */ }
}

public class Holder {
    public Tracer t;
    public Holder() { this.t = heap Tracer(); }   // Holder is sole owner
}

Holder h = heap Holder();
```

The Tracer is never aliased, because Holder is its only reference. Without
auto-drop, you must write `~Holder() { drop this.t; }`. The goal is
to handle this case without forcing destructor boilerplate.

## Why the old static rule is being dropped

> "Plain `p.field = y` where `y` is a named borrow is a static error."

The rule was conservative because we had no runtime discrimination. Once
the runtime can tell owner from alias, as Solution B below does, the rule
does nothing the runtime check does not already cover. It also makes use
cases 1, 2 and 3 inexpressible without `#` annotations that change the
meaning of the source, because a transfer consumes and a borrow does not.

What the rule was protecting against:

- **Double-free** is handled by the runtime check (Solution B).
- **Use-after-free of an aliased field whose source dropped first** is not
  handled by the runtime check. This is the trade-off.

```cajeta
Holder h;
{
    Hello local = heap Hello();
    h = heap Holder();
    h.ref = local;            // borrow stored in field
}                             // `local` drops → Hello freed
print(h.ref.greet());         // use-after-free; h.ref dangles
```

The static rule caught this at `h.ref = local;`. Without it, the bug
compiles and crashes at run time. Mitigations:

- Programmer discipline. Java and C++ accept the same risk, and Java
  replaces it with GC.
- A lifetime tracker (Phase 6+), which proves the container does not
  outlive the borrow source.
- Runtime poison-on-free, shipped as an opt-in debug aid. With
  `__cajeta_set_poison_free`, drop scribbles `0xDB` over the freed body so
  a dangling field read sees poison instead of plausible stale data.

The pragmatic call is to ship Solution B for double-free safety, accept
use-after-free as a programmer responsibility at v1, and tighten with
the lifetime tracker in a future phase.

## Solutions

### Solution A: `@Borrow` annotation on aliasing fields (rejected)

Mark fields that hold borrows, and auto-drop skips them. This was tried and
reverted in this session. The verdict, from user feedback, was "It's a hack
and an ugly one."

Problems:
- Templated types like `Optional<T>` need it for the worst case, where T is
  a class, even when T is a primitive, because an annotation cannot be
  conditional on the instantiation.
- Annotation noise across the stdlib.
- It adds a third orthogonal axis (own / borrow / ???) at the syntax level
  for what is actually a runtime-resolvable question.

It is kept here to document why it is not the choice.

### Solution B: Live-set claim at the free dispatchers (chosen)

> **As shipped.** An earlier draft of this doc proposed a per-field helper,
> `__cajeta_field_drop_if_owned`, that walked the per-fiber drop chain to
> tell owner from alias. That helper was never built. The implemented
> mechanism moves the discrimination into the free dispatchers via a
> global live-set, which is simpler and works across fibers and across
> arbitrary aliasing beyond the in-scope chain.

Every `heap` allocation is recorded in a single global live-set through
`__cajeta_live_set_add`, an open-addressed hash table guarded by a mutex
in `cajeta_runtime.c`. Each free dispatcher begins with an atomic
claim, which removes the address if present:

- **Claim succeeds**, because the address was in the set, so this caller
  owns the free. It runs the destructor, frees the body, then poisons if enabled.
- **Claim fails**, because the address was already removed, so some other
  path already freed it. This is a no-op.

Auto-drop therefore emits a direct, unconditional drop call for each
owned-shape field, and the dispatcher's claim makes the call idempotent. When a
field aliases another owner, as `ArrayStream.data` aliases
`ArrayList.data`, both the field's auto-drop and the real owner's chain
pop call the same dispatcher on the same address. The first wins the claim
and frees, and the second no-ops.

```c
// Class-ref fields → __cajeta_class_virtual_drop:
void __cajeta_class_virtual_drop(void* instance) {
    if (!instance) return;
    if (!__cajeta_live_set_claim(instance)) return;   // idempotent claim
    void* vptr = *(void**) instance;
    if (!vptr) return;
    void (*drop_fn)(void*) =
        *(void (**)(void*)) ((char*) vptr + CAJETA_VTABLE_DROP_FN_OFFSET);
    if (drop_fn) drop_fn(instance);                   // runs ~Class() chain
}

// Array fields → __cajeta_free_array (same claim-then-free shape):
void __cajeta_free_array(void* ptr) {
    if (!ptr) return;
    if (!__cajeta_live_set_claim(ptr)) return;
    __cajeta_poison_buffer(ptr);
    free(ptr);
}
```

Neither dispatcher bumps `__cajeta_drop_count`. That counter is bumped only
by `__cajeta_drop_pop_run`, the chain-pop path. A field auto-drop is a
direct call, so it does not increment the counter. A `drop_fn` body can
itself allocate, as `~Tracer()` allocates an `int32[1]`, which registers
and fires its own chain entry and bumps the counter naturally. That is how
the tests observe an auto-drop firing.

The code-gen lives in `CajetaClass::getOrCreateDropFunction` → `emitDropBodyInline`.

For each owned-shape field, in reverse declaration order, emit a direct call
to the matching dispatcher:
- array field → `__cajeta_free_array(field)`,
- interface field → `__cajeta_iface_drop(field)` (kind-tagged),
- class-ref field → `__cajeta_class_virtual_drop(field)`, only when the
  field class `hasVtablePointerAtSlotZero()`. The vtable's `drop_fn` slot is
  patched via `patchVirtualTableDropFn()`.

Primitives, pointers, function-typed fields, and embedded views need no
drop. Owner-vs-alias is resolved entirely by the live-set claim inside each
dispatcher, and the code-gen does no static discrimination.

#### Walk-throughs

In the walk-throughs below, "claim(addr)" is `__cajeta_live_set_claim`.
It succeeds and frees the first time an address is seen, then fails.

**Use case 4 (Holder owns Tracer):**

```
heap Tracer() in ctor → Tracer in live-set; chain entry E_Tracer pushed
heap Holder() → Holder in live-set; chain entry E_Holder pushed
```

Drop:
1. E_Holder pops → drop_count += 1 → calls Holder drop wrapper.
2. Holder auto-drop calls `__cajeta_class_virtual_drop(this.t)` →
   claim(Tracer) succeeds → runs `~Tracer()`.
3. `~Tracer()` allocates int32[1] → its chain entry E_junk is pushed.
4. `~Tracer` body exits → E_junk pops → drop_count += 1 → frees array.
5. Returns out of Tracer drop, out of Holder auto-drop, then the Holder
   wrapper frees the Holder body.
6. E_Tracer pops → claim(Tracer) now fails (already removed) → no-op.

Total drop_count = 2. ✓ (Matches `AutoFieldDropTests.heapClassAutoDropsOwnedClassRefField`.)

**Use case 2 (Optional<Hello>):**

```
heap Hello() → Hello in live-set; E_Hello pushed
heap Optional<Hello>(true, h) → Optional in live-set; E_Optional pushed
opt.value = h  (borrow stored in field; no new allocation)
```

Drop (reverse order, Optional first):
1. E_Optional pops → calls Optional drop wrapper.
2. Auto-drop calls `__cajeta_class_virtual_drop(this.value)` →
   claim(Hello) succeeds → Hello freed.
3. E_Hello pops → claim(Hello) fails → no-op.

Total: one Hello free. ✓ No double free.

If user code accesses `h` between Optional dropping and outer scope
exit, that is a use-after-free, which is an accepted risk per the new doctrine.

**Use case 1 (ArrayStream over ArrayList):**

```
heap ArrayList<int32>() → E_xs
xs.add(...) → grows buffer → buffer in live-set; E_buf pushed for the T[]
heap ArrayStream<int32>(this.data, ...) → E_s; s.data = xs.data (alias)
```

Drop (reverse order, ArrayStream first):
1. E_s pops → ArrayStream drop wrapper → auto-drop calls
   `__cajeta_free_array(this.data)` → claim(buffer) succeeds → buffer freed.
2. E_buf pops → claim(buffer) fails → no-op.
3. E_xs pops → ArrayList drop wrapper → auto-drop calls
   `__cajeta_free_array(this.data)` → claim(buffer) fails → no-op.
4. ArrayList body freed.

No double free. ✓

**Use case 3 (Pair<Hello, Hello>):** this is the same pattern as Optional,
with two fields, both claim-resolved.

#### Edge cases

- **Null field**: the dispatchers short-circuit on null. This matches
  `AutoFieldDropTests.heapClassAutoDropTolerateNullField`.
- **Virtual dispatch (Animal/Dog)**: `heap Dog()` patched Dog's vtable
  `drop_fn` slot. `__cajeta_class_virtual_drop` loads `vptr →
  drop_fn` from the instance and dispatches to `~Dog` correctly. This matches
  `AutoFieldDropTests.classRefFieldAutoDropDispatchesVirtually`.
- **Array of class refs**: the array storage and each element pointer are
  separate live-set addresses. ArrayList's auto-drop frees the array
  storage, where the claim succeeds once, and the element chain entries fire
  next and free each element. The direction is that `ArrayList<Hello>.data`
  is an array of `Hello*`, so the array storage is freed, then the `Hello`
  entries free each `Hello`.
- **Explicit alias outliving source** (`h2.t = h1.t` where `h1.t` was a
  ctor-allocated owner): when h2 drops, claim(h1.t) succeeds and frees. When
  h1 drops, the claim fails and no-ops. The net is no double free, and `h1.t`
  dangles if the user accesses it between h2-drop and h1-drop. That is the
  programmer's responsibility.

#### Cost

- One mutex-guarded hash-set claim per owned-shape field per parent drop,
  plus one claim per heap free generally. It is O(1) amortized. The live-set
  is a fixed 64K-slot open-addressed table (`cajeta_runtime.c`), and past
  75% load it warns and leaks the excess instead of resizing.
- One word of set occupancy per live heap allocation, with no header change
  on the object itself.

### Solution C: No auto-drop; require explicit destructors (rejected)

Revert to the original spec text at MemoryModel.md:138. Users write
their own destructors with explicit `drop this.t;` or the equivalent for
every owned field. There is zero new mechanism, but the boilerplate cost is
high, and forgetting the destructor silently leaks. It was not chosen because
the ergonomic loss is real, since Java users do not expect to write
destructors for plain field cleanup.

### Solution D: Force ownership transfer at constructor (rejected)

Require `#` on all class-typed constructor params. This breaks the iterator
pattern, because `xs.stream()` cannot transfer xs's buffer without leaving
xs dead. It is not viable.

## Spec updates (applied)

`MemoryModel.md` has been updated to match the relaxed rule and the shipped
mechanism:

- The old "field assignment must transfer; plain `p.field = y` borrow is a
  static error" rule is gone. The § Fields bullets now say that borrows may
  be stored in fields, and that owner-vs-alias is resolved at drop time by
  the live-set claim. Use-after-free of an aliased field whose source dropped
  first is the programmer's responsibility at v1; a lifetime tracker is the
  planned tightening.
- Which sources a field store may keep with a plain `=` is governed by the
  field-store rule (MemoryModel § Fields). A `T` or `#T` parameter or an owned
  local is kept with `#=`, and a parameter kept with `=` is spelled `^T`.
- The "automatic field drops" gap is documented as ✅ Done via
  `emitDropBodyInline` and the live-set claim dispatchers, without a
  chain-walk helper.

## Status — shipped

- Auto-drop is implemented in `CajetaClass::getOrCreateDropFunction` →
  `emitDropBodyInline`, as direct, reverse-declaration-order calls to
  `__cajeta_free_array` / `__cajeta_iface_drop` / `__cajeta_class_virtual_drop`
  per owned-shape field.
- Double-free safety comes from the live-set claim inside those dispatchers
  (`__cajeta_live_set_claim`). The `__cajeta_field_drop_if_owned` chain-walk
  helper from an earlier draft was never built.
- The `@Borrow` annotation experiment was reverted (Solution A).
- It is pinned by `test/parser/AutoFieldDropTests.cpp`, and the stdlib
  regressions are resolved.
- The per-instance ownership bits live in one hidden struct member after the
  class's own fields. It is an `i64` for up to 64 bit-carrying fields, meaning
  class references, arrays and closures, and an `[N x i64]` past that, with
  field `k`'s bit at word `k / 64`, bit `k % 64`. Until 2026-10-24 the member
  was always one `i64` and the 65th field's shift was poison, so a release
  build of a class with 66 such fields corrupted itself two calls later, with
  no diagnostic (cajeta-llm `CausalLM`). Pinned by `test/ownership/WideOwnershipWordTests.cpp`.
  A class under 64 keeps the layout it always had, so archives built before
  the change still agree with code built after it.
