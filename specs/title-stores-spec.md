# title-stores — the `#=` title-assign operator and implicit element-slot ownership

## 1. Definition

### 1.1 Purpose
Complete the title-tracking (rev 2) store story. Today the caller's per-call
lend-or-surrender decision reaches the callee (ABI transfer word, armed drop
entries) and can be recorded on **scalar fields** (`this.f = #v` → field
ownership bit, bit-guarded synthesized drop). It cannot be recorded on
**array element slots** — so every array-backed container hand-rolls an
`owned[]` sidecar and reads the raw transfer word positionally
(`Cajeta.moveMask() & bit`), exposing the calling convention as a user API.

This spec (a) gives element slots the field treatment — implicit,
compiler-managed per-slot title bits — and (b) fuses the store spelling into
a single operator, `#=`, so the language keeps exactly one ownership sigil.

### 1.2 Problem statement
1. `data[i] = #v` has nowhere to put the bit: arrays carry no per-slot
   ownership state (element sidecars exist only for locals, slices 9.2.1).
2. Container authors compensate with manual `owned[]` arrays plus positional
   reads of the transfer word — fragile (bit index coupled to formal order),
   leaky (ABI as API), and unavailable to third-party collection authors
   without cargo-culting stdlib internals.
3. The plain retaining store of a runtime owner (`this.f = v` where `v` may
   have arrived owned) compiles silently and frees at callee exit — the
   single most recurring UAF family in the codebase (6.2.3's 54 stdlib
   sites; 6.2.6c stream wrappers; the CallerSideTransfer fixture respells).
4. Teardown of partially-populated arrays needs `@ElementCount` to avoid
   dropping garbage in `[size..capacity)` — a bookkeeping annotation that
   exists only because slots don't know their own state.

### 1.3 Constraints
- No ownership spelling returns to TYPE positions (title-tracking §8.1
  precedent: type-position `#` is retired; ownership is per-value/per-call,
  spelled at the site where title moves). Explicitly rules out `T[!]`,
  `#T[]`, `own T[]`, and any declaration-site marker.
- Field symmetry governs: whatever a store means for `this.f`, it means for
  `this.data[i]`. One mental model.
- Arrays that never receive a title store must pay nothing (no bitmap
  allocation, no layout change) and behave exactly as today.
- The shared-capable dual-role store (String/Utf8: plain store copies
  borrowed bytes at runtime) must compose — a title store must not bypass it
  (the 5.2.6 lesson).

### 1.4 Non-goals
- No change to call-site or return spelling (`f(#v)`, `return #v`,
  `#data[i]` move-out reads stay as-is).
- No static ownership inference (spec §4.6.4 stands).
- Collections whose element storage is non-contiguous or inline-struct
  (HashMap's MapEntry) may keep bespoke destructors where per-slot bits
  don't fit — see §3.4 for what they gain anyway.

## 2. The `#=` title-assign operator

### 2.1 Semantics
`dst #= v` stores `v` into `dst` and moves `v`'s **title, whatever it is**,
onto the destination:

- `v` statically owned (owned local, fresh rvalue) → hard move: `dst` owned,
  `v` marked moved (linearity, as `= #v` today).
- `v` a runtime owner (formal, flagged call result) → the runtime flag is
  forwarded: `dst`'s bit records what the caller actually did; `v`'s entry
  is consumed.
- `v` statically borrowed → `dst` records borrow (bit 0); no drop at
  teardown.

`#=` is exactly today's `= #v` at assignment sites, promoted to a fused
token so the store spelling is atomic and cannot be half-written.

### 2.2 Valid destinations
- 2.2.1 Field: `this.f #= v` (scalar class-typed field → field ownership
  bit; existing machinery).
- 2.2.2 Element slot: `this.data[i] #= v` (→ per-slot bit, §3).
- 2.2.3 Local: `T x #= v` — declaration-initializer form; equivalent to
  `T x = #v` today (arms/forwards the local's entry). IN v1 (decided
  2026-07-14, §5.3).
- 2.2.4 Indexed user class: `m[k] #= v` lowers through `operator[]=` with
  the transfer word composed (existing 6.2.2 lowering; the sugar surface
  changes only in spelling).

### 2.3 Migration — one spelling survives
- Phase 1: `#=` lands; `= #v` at assignment sites still accepted.
- Phase 2: `= #v` at assignment sites emits a deprecation WARNING with a
  fix-it (`use #=`). `#v` remains the ONLY spelling at call args, returns,
  and extraction reads — those are not assignments.
- Phase 3 (separate flip, after stdlib + tests are respelled): the warning
  becomes an error. Dual spellings must not survive long-term.

### 2.4 The loud-plain-store diagnostic
A plain `=` retaining store (field or slot destination) whose RHS is a
class-typed **runtime owner** (armed-capable formal or flagged call result)
becomes a compile-time diagnostic:

> `v` may arrive owned on some calls; a plain store borrows it and the
> armed entry frees it at exit. Spell `dst #= v` to move its title, or
> store a copy (`dst = v.clone()`).

- 2.4.1 Severity: WARNING at introduction (stdlib must go clean first),
  promoted to ERROR with Phase 3.
- 2.4.2 Out of scope for the diagnostic: primitives, value types without
  heap payload, statically-borrowed sources (no entry to leak), and
  non-retaining uses (call args, reads).

### 2.5 Use cases
- 2.5.1 As a container author, when I write `this.data[n] #= v` in
  `add(T v)`, then a caller's `add(#x)` makes slot n owned and `add(x)`
  makes it borrowed, with no other code on my part.
- 2.5.2 As a wrapper author (`Optional`, stream stages), when I write
  `this.value #= value`, the behavior is identical to today's
  `this.value = #value` — respell is mechanical.
- 2.5.3 As any author, when I forget the sigil on a retaining store of a
  formal, the compiler tells me at compile time instead of a UAF at run
  time (§2.4).

## 3. Implicit element-slot ownership bits

### 3.1 Model
Class-typed element arrays get per-slot title bits, exactly parallel to the
per-field ownership word:

- 3.1.1 Eager tail bitmap (decided 2026-07-14, §5.2): every array whose
  element type is droppable gets its bits at creation, laid out in the SAME
  allocation — `header | data[capacity] | bits`. The bitmap address is
  COMPUTED (`data + capacity * stride`; capacity is already a header word):
  no pointer chase, no side table, no null test, no materialization branch
  on any hot path. Cost: capacity/8 bytes + a memset already covered by
  zero-init. Grow reallocates and the tail moves with it. Primitive-element
  arrays get no bits. (Laziness was rejected with the rest of the
  ease-into-it options — done right the first time.)
- 3.1.2 The bitmap belongs to the ARRAY allocation, not to any referencing
  local or field — aliases (`T[] a = this.data`) see the same titles.
- 3.1.3 Declaration-free: no type or field marker exists (§1.3). The store
  is the only syntax.

### 3.2 Synthesized behaviors
Once slots carry bits, the compiler emits what containers hand-roll today:

- 3.2.1 Teardown: dropping an array (or its owning field/local going out of
  scope) drops exactly the slots whose bit is set. Vacant/garbage slots in
  `[size..capacity)` have bit 0 — **`@ElementCount` is retired**.
- 3.2.2 Displaced release: `data[i] #= v` (and plain `data[i] = v` on an
  array WITH a bitmap) first drops the current occupant iff its bit is set,
  then stores and records.
- 3.2.3 Move-out read: `#data[i]` clears the slot's bit and forwards it
  (return-flag / destination entry), composing with 3.2.1 so grow/shift
  loops (`bigger[i] #= #this.data[i]`) carry titles with zero bookkeeping.
- 3.2.4 Conditional free: `Cajeta.dropValue(#data[i])` is bit-guarded
  (owned → freed + bit cleared; borrowed → no-op).

### 3.3 Interactions
- 3.3.1 Shared-capable values (String/Utf8): a PLAIN store keeps the
  dual-role resolve (borrowed bytes copy). A `#=` store forwards title
  without the copy — matching field-store behavior. Slot bits and the
  wrapper's internal share machinery are independent layers.
- 3.3.2 Inline-struct elements (decided 2026-07-14, §5.1): PER-MEMBER
  bits, replicated per slot — an inline value-struct element is an object
  without a header, so each slot carries exactly the ownership word its
  heap counterpart would (one bit per droppable member, same indices).
  A single per-slot bit is insufficient: HashMap's reality is key-owned /
  val-borrowed in the same slot. `slots[i].val #= v` sets slot i's
  val-bit; teardown walks members like the field walk. HashMap converts
  in v1 — no bespoke-destructor carve-out.
- 3.3.3 Primitive/value element types without heap payload: no bits, `#=`
  on such slots is a no-op store (same rule as formals).
- 3.3.4 Views/interfaces: excluded from bit-guarded drop (no uniform drop
  shape — same exclusions as the field walk).

### 3.4 What the stdlib sheds (acceptance dividend)
- ArrayList/Heap: `owned[]` sidecars, the `mvm` reads, hand-carried bits in
  grow/sift.
- LinkedListNode: the `owned` mirror byte.
- BPlusTree: per-slot `owned[]` carried through shift/split.
- HashMap: candidate, gated on §3.3.2 (MapEntry inline structs); even if
  excluded in v1 it drops the positional `moveMask` reads via §4.
- The 6.2.6b tombstone machinery simplifies: cleared slots are bit-0 slots.

### 3.5 Use cases
- 3.5.1 As a third-party dev, when I write a `Pool<T>` with a `T[]` field
  and use `#=` at stores, teardown frees exactly the pool-owned elements —
  I never see a bit, a mask, or a sidecar.
- 3.5.2 As a container author, when I grow the backing array with
  `bigger[i] #= #this.data[i]`, titles travel and the old array drops
  empty — no double free, no leak.
- 3.5.3 As a maintainer, when I delete `@ElementCount` from stdlib, teardown
  behavior is unchanged (bits already bound the walk).

## 4. `Cajeta.owned(formal)` — the conditional-logic escape hatch

### 4.1 Semantics
`Cajeta.owned(v)` — `v` a formal (or flagged call-result local) — returns
`boolean`: did THIS call surrender title of `v`? Compiles to a read of the
enclosing function's ABI transfer word at `v`'s compiler-resolved index
(works for Strings and all types; no drop entry required, no positional
arithmetic in user code).

### 4.2 Scope and status
`Cajeta.owned()` is a first-class introspection primitive with the same
status — and the same smell profile — as `instanceof`: a legitimate query
that is occasionally the honest answer, and a design smell when it becomes
load-bearing. It is KEPT permanently (decided 2026-07-14), for algorithms
that genuinely BRANCH on ownership (interning pools: adopt owned / copy
lent).

**Design invariant: correctness never requires it.** No container, wrapper,
or store pattern may NEED `owned()` to be leak-free and UAF-free — `#=`,
slot bits, and bit-guarded `dropValue` must cover all bookkeeping on their
own. If a pattern is found that forces an `owned()` call for correctness,
that is a missing synthesis: file it against §3, do not document the
workaround.

### 4.3 `Cajeta.moveMask()` retirement
With §2–§4 in place, the positional intrinsic loses its last legitimate
caller. Stdlib respells (6 sites) → intrinsic removed → the name `moveMask`
ceases to exist in the language. (The TLS carrier is already gone —
title-tracking 7.2.2, `d4a2a67b`.)

### 4.4 Use cases
- 4.4.1 As an interner author, `if (Cajeta.owned(s)) adopt else copy` —
  readable, reorder-safe, type-agnostic.
- 4.4.2 As a stdlib maintainer, HashMap.put's `(mvm & 1)` / `(mvm & 2)`
  respell to `Cajeta.owned(key)` / `Cajeta.owned(value)` (or vanish
  entirely under §3).

## 5. Open questions (resolve before the plan)

- 5.1 RESOLVED 2026-07-14: per-member bits replicated per slot (§3.3.2).
- 5.2 RESOLVED 2026-07-14: eager single-allocation tail bitmap, address
  computed from the capacity header word (§3.1.1). Side table rejected
  (hash per store/drop); fat pointer rejected (taxes every array ref).
- 5.3 RESOLVED 2026-07-14: locals in v1 (§2.2.3).
- 5.4 OPEN — end-state spelling: does `= #v` at assignment sites retire
  after the migration window (one spelling, recommended), or coexist with
  `#=` permanently? (`#v` in non-assignment positions — call args,
  returns, extraction reads — is untouched either way.) Timing of any
  error flip parks until after title-tracking Unit 8 lands on main.
- 5.5 RESOLVED 2026-07-14: `#=` is NOT overloadable — title motion stays
  compiler-emitted end to end, in the heap/stack class of non-user-surface
  operations, NOT the operator+ class. A user overload could swallow or
  duplicate a title, un-guaranteeing the model. `m[k] #= v` lowers through
  the existing `operator[]=`+transfer-word path; the author spells `#=`
  on their own storage inside.
