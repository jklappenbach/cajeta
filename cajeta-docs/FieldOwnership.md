# Field Ownership and Auto-Drop

Status: design draft, 2026-05-17. Triggered by the auto-field-drop landing
(MemoryModel.md § Known gaps, item "No automatic field drops") regressing
stdlib tests for `ArrayStream<T>`, `Optional<T>`, and `Pair<K,V>`.

Outcome: the prior spec rule "borrows in fields are a static error" is
dropped. Fields may legitimately hold borrows. Auto-drop discriminates
owner from alias at runtime by consulting the per-fiber drop chain.

## Background

`MemoryModel.md` originally laid out a strict single-owner-fields doctrine:

- Every heap allocation has exactly one owning slot.
- That slot is registered in the per-fiber drop chain at allocation time.
- When the slot's scope exits, the drop chain runs the slot's destructor
  and frees the body.
- Old rule, MemoryModel.md ~line 102: "Field assignment transfers.
  `p.field = x` must transfer: either `p.field = #x` (explicit) or
  `p.field = heap T(...)` (auto-promoted). Plain `p.field = y` where `y`
  is a named borrow is a static error."
- Old rule, MemoryModel.md ~line 267: "Fields are owners. Borrows in
  fields are a static error."

The rule existed because if every class-typed field is statically
guaranteed to be the sole owner, auto-drop is unconditionally safe — the
compiler emits "free each field" with no runtime discrimination required.

The stdlib doesn't follow this rule. `ArrayStream<T>.data` aliases
`ArrayList<T>.data`. `Optional<T>.value` aliases the constructor
argument. `Pair<K,V>.first/second` likewise. These patterns are
necessary: `xs.stream()` can't transfer the buffer (xs must remain
iterable); `Optional<Hello>` storing `#h` would consume the local.

The decision in this doc: relax the spec, allow borrows in fields,
let the runtime drop chain self-discriminate.

## Problem

Four use cases. The first three motivate "borrows-in-fields are
legitimate." The fourth is the case auto-drop must still handle.

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

    public ArrayStream(T[] data, int32 limit) {
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
ArrayStream<int32> s = xs.stream();
while (s.next().isPresent()) { ... }
// Both `s` and `xs` drop at scope exit.
```

Naive auto-drop (drop every field unconditionally): ArrayStream frees the
buffer when it pops; ArrayList frees it again when it pops. Double free.

### Use case 2: Optional wrapping a class instance

```cajeta
public class Optional<T> {
    boolean present;
    T value;
    public Optional(boolean p, T v) { this.present = p; this.value = v; }
}

// caller
Hello h = heap Hello();
Optional<Hello> opt = heap Optional<Hello>(true, h);
print(h.greet());      // expected: still works
```

`opt.value` aliases `h`. Naive auto-drop frees Hello when opt pops;
h's own chain entry frees it again. Double free.

Generic wrinkle: `Optional<int32>` has nothing to drop. `Optional<Hello>`
does. Same class declaration, different instantiations — the field
discriminator can't be syntactic.

### Use case 3: Pair holding two references

```cajeta
public class Pair<K, V> {
    K first;
    V second;
    public Pair(K a, V b) { this.first = a; this.second = b; }
}

Hello a = heap Hello();
Hello b = heap Hello();
Pair<Hello, Hello> p = heap Pair<Hello, Hello>(a, b);
print(a.greet());
print(b.greet());
```

Same shape as Optional, two fields. Naive auto-drop double-frees both.

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

The Tracer is never aliased — Holder is its only reference. Without
auto-drop, the user must write `~Holder() { drop this.t; }`. The goal is
to handle this case without forcing destructor boilerplate.

## Why the old static rule is being dropped

> "Plain `p.field = y` where `y` is a named borrow is a static error."

The rule was conservative because we had no runtime discrimination. Once
the runtime can tell owner from alias (Solution B below), the rule does
nothing the runtime check doesn't already cover — and it makes use cases
1, 2, 3 inexpressible without `#` annotations that change the meaning of
the source (transfer consumes, borrow doesn't).

What the rule was protecting against:

- **Double-free**: handled by runtime check (Solution B).
- **Use-after-free of an aliased field whose source dropped first**: NOT
  handled by runtime check. This is the trade-off.

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

- Programmer discipline (Java / C++ accept the same risk; Java replaces
  it with GC).
- Lifetime tracker (Phase 6+) — proves container doesn't outlive borrow
  source.
- Runtime poison-on-free (drop scribbles a sentinel into the freed body;
  field access crashes hard instead of silently).

The pragmatic call: ship Solution B for double-free safety; accept
use-after-free as a programmer responsibility at v1; tighten with
lifetime tracker in a future phase.

## Solutions

### Solution A: `@Borrow` annotation on aliasing fields (rejected)

Mark fields that hold borrows; auto-drop skips them. Tried and reverted
in this session. Verdict: "It's a hack and an ugly one." (User feedback.)

Problems:
- Generic types like `Optional<T>` need it for the worst case (T is a
  class) even when T is a primitive — annotation can't be conditional on
  the instantiation.
- Annotation noise across the stdlib.
- Adds a third orthogonal axis (own / borrow / ???) at the syntax level
  for what's actually a runtime-resolvable question.

Kept here for documentation of why it isn't the choice.

### Solution B: Drop chain self-discrimination (chosen)

Auto-drop calls a helper for each owned-shape field. The helper walks the
per-fiber drop chain looking for an entry whose `obj` matches the field's
address.

- **Entry found** → this address has an outstanding drop registration.
  The container is the most-recently-pushed scope that can reach it, so
  cancel the entry (so it doesn't fire again later) and drop the field
  now.
- **No entry** → either the address is owned by something that already
  dropped (and cancelled its entry), or the address was never registered
  (e.g. assigned from a now-dropped local). Either way, don't touch it.

```c
void __cajeta_field_drop_if_owned(void* obj) {
    if (!obj) return;
    struct cajeta_drop_entry** top = __cajeta_drop_top_ptr();
    for (struct cajeta_drop_entry* e = *top; e; e = e->prev) {
        if (e->active && e->obj == obj) {
            e->active = 0;          // cancel; chain pop will skip
            e->drop_fn(e->obj);     // run drop here
            return;
        }
    }
    // no entry — aliased and owned elsewhere (or already freed)
}
```

The helper does NOT bump `__cajeta_drop_count`. That counter is bumped by
the original entry's pop path; here we're suppressing that pop, so no
bump. The drop_fn body can itself allocate (e.g. `~Tracer()` allocates
an int32[1]), which registers and fires its own chain entry, bumping the
counter naturally.

**Code-gen change in `CajetaClass::getOrCreateDropFunction`**:

For each owned-shape field (class-ref, array — the two that double-freed):
- Was: emit direct call to `__cajeta_class_virtual_drop(field)` or
  `__cajeta_free_array(field)`.
- New: emit call to `__cajeta_field_drop_if_owned(field)`.

For interface and struct fields, the existing code paths (`__cajeta_iface_drop`
which is kind-tagged, and struct destructors which are inline)
already handle owner-vs-borrow correctly. They don't need to change.

#### Walk-throughs

**Use case 4 (Holder owns Tracer):**

```
heap Tracer() in ctor → E_Tracer registered
heap Holder() → E_Holder registered
```

Drop:
1. E_Holder pops → drop_count += 1 → calls Holder drop wrapper.
2. Holder auto-drop walks `this.t` → helper finds E_Tracer → cancels →
   calls Tracer drop_fn.
3. Tracer drop runs `~Tracer()` → allocates int32[1] → E_junk registered.
4. ~Tracer body exits → E_junk pops → drop_count += 1 → frees array.
5. Returns out of Tracer drop, out of Holder auto-drop, out of Holder
   wrapper.
6. E_Tracer pops → inactive → no fire.

Total drop_count = 2. ✓ (Matches `AutoFieldDropTests.heapClassAutoDropsOwnedClassRefField`.)

**Use case 2 (Optional<Hello>):**

```
heap Hello() → E_Hello registered
heap Optional<Hello>(true, h) → E_Optional registered
opt.value = h  (assignment from local, no new chain entry)
```

Drop (reverse order, Optional first):
1. E_Optional pops → calls Optional drop wrapper.
2. Auto-drop walks `this.value` → helper finds E_Hello → cancels →
   calls Hello drop. Hello freed.
3. E_Hello pops → inactive → no fire.

Total: one Hello free. ✓ No double free.

(If user code accesses `h` between Optional dropping and outer scope
exit, that's use-after-free — accepted risk per the new doctrine.)

**Use case 1 (ArrayStream over ArrayList):**

```
heap ArrayList<int32>() → E_xs
xs.add(...) → grows buffer → E_buf for the T[]
heap ArrayStream<int32>(this.data, ...) → E_s; s.data = xs.data
```

Drop (reverse order, ArrayStream first):
1. E_s pops → ArrayStream drop wrapper → auto-drop walks `this.data` →
   helper finds E_buf → cancels → frees buffer.
2. E_buf pops → inactive → no fire.
3. E_xs pops → ArrayList drop wrapper → auto-drop walks `this.data` →
   helper finds no entry → no-op.
4. ArrayList body freed.

No double free. ✓

**Use case 3 (Pair<Hello, Hello>):** same pattern as Optional, two
fields, both helper-resolved.

#### Edge cases

- **Null field**: helper short-circuits on null. Matches
  `AutoFieldDropTests.heapClassAutoDropTolerateNullField`.
- **Virtual dispatch (Animal/Dog)**: E_Dog was registered with Dog's drop
  wrapper at the `heap Dog()` site. Helper calls `e->drop_fn` which IS
  Dog's wrapper. Auto-drop dispatches to ~Dog correctly. Matches
  `AutoFieldDropTests.classRefFieldAutoDropDispatchesVirtually`.
- **Array of class refs**: the array storage has its own E_arr; each
  element pointer has its own E_class. Auto-drop of the array field
  finds E_arr, cancels, calls `__cajeta_free_array`. Element entries
  fire normally (their pops aren't suppressed) and free the elements.
  Direction: ArrayList<Hello>.data is an array of Hello*; ArrayList's
  auto-drop frees the array storage; the Hello entries fire next and
  free each Hello.
- **Explicit alias outliving source** (`h2.t = h1.t` where h1.t was a
  ctor-allocated owner): when h2 drops, helper finds E_h1t (h1's
  ctor-allocated Tracer's entry), cancels, frees. When h1 drops, no
  entry, no-op. Net: no double free; h1.t dangles if user accesses it
  between h2-drop and h1-drop. (Programmer responsibility.)

#### Cost

- O(chain length) chain walk per owned-shape field per parent drop.
  Typical chain is short (per-scope). Long-lived scope with many heap
  allocs degrades — mitigation if it matters: per-fiber hashmap
  address → entry maintained alongside the chain.
- No per-allocation overhead beyond the existing chain entry.

### Solution C: No auto-drop; require explicit destructors (rejected)

Revert to the original spec text at MemoryModel.md:138. Users write
their own destructors with explicit `drop this.t;` (or equivalent) for
every owned field. Zero new mechanism, but high boilerplate cost, and
forgetting the destructor silently leaks. Not chosen because the
ergonomic loss is real (Java users don't expect to write destructors
for plain field cleanup).

### Solution D: Force ownership transfer at constructor (rejected)

Require `#` on all class-typed constructor params. Breaks the iterator
pattern (`xs.stream()` can't transfer xs's buffer or xs is dead). Not
viable.

## Spec updates required

`MemoryModel.md`:

- Line ~41 / ~102 — change `new T(...)` references to `heap T(...)` (the
  `new` keyword was retired in Phase 7).
- Line ~102 — remove "Plain `p.field = y` where `y` is a named borrow
  is a static error." Replace with: "Borrows may be stored in fields.
  Owner-vs-borrow is resolved at drop time by the drop-chain self-
  discrimination check (see FieldOwnership.md). Use-after-free of an
  aliased field whose source has dropped is the programmer's
  responsibility at v1; Phase 6+ adds a lifetime tracker."
- Line ~138 — replace "No automatic field drops." with the Solution B
  description (chain-walk helper).
- Line ~267 — replace "Fields are owners. Borrows in fields are a
  static error." with: "Fields may be owners or borrows. Auto-drop
  uses a runtime check on the per-fiber drop chain to free fields that
  this scope owns and skip aliased fields."

## Status

- Auto-drop scaffolding is in place in `CajetaClass::getOrCreateDropFunction`.
- The current call sites use direct `__cajeta_class_virtual_drop` /
  `__cajeta_free_array`, which double-frees stdlib classes. Solution B
  swaps those for `__cajeta_field_drop_if_owned`.
- `@Borrow` annotation experiment reverted.
- Tests in `test/parser/AutoFieldDropTests.cpp` pass under Solution B;
  stdlib regressions resolve.
