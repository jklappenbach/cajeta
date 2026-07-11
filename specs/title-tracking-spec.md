# Title-tracking ownership

## 1. Definition

### 1.1 Purpose
One model for heap-object ownership across the whole language: every
allocation has exactly one **title** (the right and duty to free it); titles
live in **places** (locals, fields, array slots, container entries); the `#`
sigil is the only way a place surrenders its title. Checking is **hybrid**:
static where the compiler sees every use (locals), runtime state where it
cannot (fields, entries), with an explicit ABI flag bridging the two at
`#?`-marked signatures.

### 1.2 Problem
Three verified failures motivate this spec (probe session 2026-07-10/11):

1. **Type-argument ownership modes fail under extraction.** The
   element-ownership model (`HashMap<K,#V>`) makes ownership a static
   per-instantiation property. The moment a title can leave the collection
   (`#map[k]`) — a flow the language must support — the type describes only
   the front door while per-entry reality diverges. Mode-colored types also
   bifurcate every API (distinct, non-convertible instantiations, §8.3.1 of
   the superseded spec) and forced dissolution, contagion, confinement, and
   four standing stdlib exemptions.
2. **Titles move without the sigil.** Plain field stores (`this.f = v`) and
   array stores silently deactivate the source's drop entry
   (`BinaryOpExpression.cpp`) — a transfer neither side spelled. This blinds
   any linearity checker: the source still looks like an owner.
3. **Local linearity is unenforced.** `#x` from a borrow mints a second
   active owner (forged title); call-arg `#x` deactivates the source's drop
   entry but does not mark it moved, so double transfers compile and yield
   poisoned reads. Both verified by probe.

### 1.3 Scope
Locals, method signatures, class fields, array slots, container entries;
the drop chain and teardown walks; the container operator surface
(`operator[]`, `operator[]=`, `operator#[]`, `remove`). Compile-time
linearity for locals; runtime ownership state for fields/entries.

### 1.4 Constraints
1. The runtime drop chain (stack `DropEntry` blobs, LIFO, active flag,
   `__cajeta_drop_mark_inactive`) is retained as the execution mechanism —
   the drop entry's `active` flag is already runtime state; this spec leans
   into it rather than replacing it.
2. Vtable dispatch is mode-erased (`#` skipped in signature hashes): two
   same-name methods differing only by `#` cannot coexist. Dual-mode APIs
   therefore use the `#?` spelling or distinct names, never overloads.
3. The live-set claim in `__cajeta_class_virtual_drop` (idempotent drops)
   and buffer poisoning remain as the last-resort backstop.

### 1.5 Non-goals
1. **Borrow-source tracking / lifetime inference.** A borrow whose lender
   dies first still dangles undetected (the `obj2` hazard, the
   extracted-entry hazard). Deliberately deferred; this spec records where
   the hazard lives (§7.4) but does not close it.
2. Cross-procedure flow analysis beyond signatures.
3. Concurrency-safe title transfer (single-thread semantics; the send/share
   story is separate work).

### 1.6 Relationship to element-ownership-spec.md
Supersedes its type-argument layer: §2 (modes), §4 (author gating and
dissolution), §5 (borrow-mode confinement), §8.2–8.6 (composition,
identity, wildcards, contagion). Retains and builds on: its Unit 1–3 drop
mechanics (owned-element walks, `NewExpression #` threading), Unit 6
(typed value `clone()`), the mode-erased dispatch fix, and the diagnostic
family. The `moveMask` thread-local is retired in favor of the `#?` ABI
flag (§4.4) — the same intent, threaded explicitly instead of ambiently.

## 2. Titles and places

### 2.1 Requirements
1. Every heap allocation has exactly one title at any instant. The title's
   holder frees the allocation (directly via drop entry, or via the owning
   object/container's teardown walk).
2. A **place** is anything that can hold a reference: local binding, class
   field, array slot, container entry.
3. `#place` is the only spelling that moves a title out of a place.
   Storing `#expr` into a place is the only spelling that moves a title in.
4. Place roles/state only **decay**: local owner → moved; field/entry
   owned → borrowed (extraction). Nothing is promoted; a borrow can never
   become an owner in place. (A new *store* into a place re-arms it — that
   is a new value, not a promotion.)
5. Reads are borrows, always and everywhere. No read moves a title.

### 2.2 Use cases
1. As a developer reading any assignment, when I see no `#`, then I know no
   title moved on that line — anywhere in the language.
2. As a reviewer, when I audit who frees an allocation, then I can follow
   the `#` sigils from construction to final owner without reading callee
   bodies.

## 3. Locals — static linearity

### 3.1 Requirements
1. A local's role is fixed at declaration by its initializer shape:
   **owner** (fresh construction, `#`-returning call, `= #x` move) or
   **borrow** (bare identifier, field read, element read, plain-returning
   call). Owners get a drop entry; borrows never do.
2. `#x` requires `x` to be a **statically-active owner**. Violations are
   compile errors:
   - `#x` where `x` is a borrow → error: *cannot move out of a borrow;
     ownership belongs to `<owner>`* (kills the forged-title case).
   - `#x` where `x` was already moved → error: use of moved value, naming
     the prior move site.
3. **Every** transfer marks the source moved — call arguments and
   constructor arguments included (today only `= #x` does). Reads of a
   moved local are `CAJETA_ERROR_USE_AFTER_MOVE`.
4. Reassignment of a fresh value re-arms the binding (definite-assignment
   already tracks this); the loop-reuse pattern
   (`piece = source.trySplit()` then transfer, next iteration reassigns)
   stays legal.
5. Branch merges are conservative: moved on any path = moved after the
   join.
6. A local initialized from a `#?`-flagged return (§4.4) is a **runtime
   owner**: its drop entry's active flag is set from the call's flag. `#x`
   on such a local forwards the flag rather than asserting static
   ownership; a definite compile-time answer is not required.

### 3.2 Use cases
1. As a developer who writes `obj2 = obj; obj3 = #obj2;`, when I compile,
   then I get "cannot move out of a borrow" at the `#obj2`, naming `obj` as
   the owner.
2. As a developer who transfers the same local twice
   (`c1.put(#v); c2.put(#v);`), when I compile, then the second `#v` errors
   with the first transfer's location — not a poisoned read in production.
3. As a developer moving titles between same-scope locals
   (`obj3 = #obj; obj4 = #obj3;`), when I compile and run, then exactly one
   drop fires at scope exit and reads of `obj`/`obj3` are rejected.

## 4. Signatures — the transfer ABI

### 4.1 Formal spellings
1. `V v` — **borrow formal**. The callee holds a borrow for the call's
   duration. `#` at the call site is a compile error; forwarding `#v` from
   inside the body is a compile error (exists today:
   `BORROW_PARAM_ESCAPES`).
2. `#V v` — **mandatory-transfer formal**. The call site must surrender a
   title (`#x`, or an owned rvalue: fresh construction, `#`-returning
   call). A plain argument is a compile error. No runtime flag — transfer
   is statically certain. The body owns `v` (may forward `#v`, store it
   with `#`, or let it drop).
3. `#?V v` — **maybe-transfer formal**. Accepts both spellings; a hidden
   ABI flag (§4.4) carries the per-call answer. Inside the body, `v` is a
   runtime owner (§3.1.6); stores of `v` into dynamic places propagate the
   flag into the place's bit.

### 4.2 Return spellings
1. `V f()` — borrow return. The caller registers no drop entry.
2. `#V f()` — title return, statically certain (fresh result, or a place
   the body definitely owned — e.g. `#place` extraction, which panics
   rather than return borrowed, §6.3). Caller registers an active entry.
3. `#?V f()` — maybe-title return; the flag rides back and the caller's
   drop entry is active iff the flag is set. `remove` (§6.4) is the
   canonical user.

### 4.3 Requirements
1. Overloads distinguished only by `#`/`#?` are impossible (dispatch is
   mode-erased) and rejected at declaration.
2. Agreement is checked at every call edge: plain-into-`#V` and
   `#`-into-plain are compile errors with the fix named (the existing
   `TRANSFER_REQUIRED` / call-edge diagnostic family, minus the
   instantiation-mode variants).

### 4.4 The ABI flag
1. Each `#?` formal adds one hidden `i1` parameter; a `#?` return adds one
   hidden flag alongside the value. The flag is per-call, threads through
   forwarding explicitly, and replaces the `moveMask` thread-local (whose
   failure was precisely that forwarding chains lost it).
2. Flag sources: `#x` at the call site (true — after linearity validated
   the source), plain arg (false), a runtime owner's own flag (forwarded).
3. Flag sinks: the destination place's ownership bit (§5), the caller's
   drop-entry active flag, or a forwarded call's flag.

### 4.5 Use cases
1. As a container author, when I write
   `public void operator[]= (K key, #?V value)`, then `map[k] = v` and
   `map[k] = #v` both compile against the one method and the entry records
   which one happened.
2. As an API author of a sink that must own its input
   (`register(#Session s)`), when a caller passes a plain borrow, then the
   call is rejected at compile time.

## 5. Dynamic places — fields, slots, entries

### 5.1 Requirements
1. Class-reference **fields**, **array slots**, and **container entries**
   each carry a runtime ownership bit: *owned* (this place holds the
   title) or *borrowed*.
2. The bit is set by the store's spelling: `place = #x` → owned;
   `place = x` → borrowed; store from a `#?` value → the flag. **The
   implicit transfer on plain field/array stores is removed** — a plain
   store is a borrow store, full stop (closes §1.2 problem 2).
3. Overwriting an *owned* place drops the displaced value (the overwrite
   is its scope exit). Overwriting a borrowed place drops nothing.
4. Teardown walks (class drop, array drop, container drop) free exactly
   the places whose bit is owned.
5. Field-bit storage: one hidden bitmask word per instance covering its
   class-reference fields (layout detail owned by the plan; must not
   disturb `@ValueType` PODs, which have no reference fields by
   definition).
6. Value-semantics types (primitives, `@ValueType` PODs) have no titles;
   `#` on them is an error (unchanged §8.1 rule). Shared-capable values
   (String/Utf8/Slice) keep their share/COW machinery — their "transfer"
   remains a share-bump, not a title move.

### 5.2 Use cases
1. As a developer, when I write `this.session = #s` in one method and
   `this.session = pooled` (plain) in another, then the same field owns in
   the first instance's life and borrows in the second, and each instance's
   teardown does the right thing.
2. As a developer who stores a plain local into a field and lets the local
   drop, when I read the field later, then I have the ordinary
   borrow-outlives-lender hazard (§7.4) — but my local's books were never
   silently altered.

## 6. Containers

### 6.1 Store
`operator[]=` / `put` take the value at a `#?V` (dual-capable) or `#V`
(own-only container) formal — the author's choice, visible in the
signature. The entry bit records the outcome per §5.

### 6.2 Read
`operator[]` / `get` return borrows. Unchanged, both bits.

### 6.3 Extract — `#map[k]`
1. `#` applied to an indexed place binds to the author-provided
   **`operator#[]`** (distinct canonical name — mode-erased dispatch
   forbids a `#`-only overload of `operator[]`).
2. Semantics: if the entry bit is owned → title to the assignee, entry
   **stays resident** with its bit decayed to borrowed (membership is not
   ownership). If the entry is borrowed or absent → **panic** (a
   `Recoverable` throw: *extraction from a place that holds no title*).
   Panic-on-miss makes `#`-extraction total: `operator#[]` returns plain
   `#V`, no flag needed.
3. Arrays need no operator: the compiler owns the layout; `#a[i]` moves
   the title out and decays the slot bit in place. Same panic on a
   borrowed/null slot.

### 6.4 Remove
`#?V remove(K key)` — structural removal, always ends membership, returns
the value in whatever mode the entry held it (title if owned, borrow if
borrowed). The flagged return (§4.2.3) is the "reference or own" contract.

### 6.5 Use cases
1. As a cache user, when I `Session s = #cache[k]`, then I own the session,
   the cache still serves reads of `k`, and the cache's teardown no longer
   frees that entry.
2. As a developer who extracts the same key twice, when the second
   `#cache[k]` runs, then I get a panic naming the key's state — not a
   second title.
3. As a map user winding down, when I `#?V v = map.remove(k)`, then the
   entry is gone and `v`'s drop entry is active only if the map owned it.

## 7. Diagnostics, teardown, hazards

### 7.1 Compile-time (extends the existing escape/ownership family)
- move-out-of-borrow; use-after-move (incl. call-arg moves); `#` into a
  borrow formal; plain into a `#V` formal; `#`-overload collision;
  `#` on a value type.

### 7.2 Runtime
- Title-miss panic on `#place` extraction (Recoverable).
- Live-set claim + poison stay as the absorb-don't-corrupt backstop for
  anything that slips through.

### 7.3 Teardown
Drop chain unchanged for locals (active flag now sometimes runtime-fed);
class/array/container walks consult ownership bits; `remove`d or
extracted entries are the owner's responsibility.

### 7.4 Documented hazard (deferred, §1.5.1)
A borrow — local, field, or resident-after-extraction entry — whose lender
dies first dangles undetected. Reads return poisoned memory (deterministic
garbage, not corruption, per the poison-on-free backstop). Closing this
requires borrow-source tracking; the `Field::_viewSource` mechanism is the
in-repo precedent.

## 8. Migration

### 8.1 Retired
Type-argument `#` (parse may remain temporarily with a deprecation error);
`#Type` on a **local declaration** (`#MyClass x = ...` — a local's role
comes from its initializer shape, §3.1.1, so a type-position sigil on
locals is meaningless-at-best and can contradict the initializer; same
deprecation error; signature positions `#V`/`#?V` on formals and returns
are the transfer ABI and stay, §4); dissolution/provenance gating (4A),
call-agreement instantiation checks (4B), declaration-`#`/contagion
(Unit 5), borrow-mode confinement gates (Unit 7), all four stdlib
transitional exemptions, `moveMask` thread-local and its runtime plumbing.
*(Local-declaration-`#` retirement approved 2026-07-11, this session.)*

### 8.2 Retained
Drop chain + entries; owned-element teardown walks (re-keyed from
instantiation mode to entry bits); typed value `clone()`; mode-erased
dispatch; `BORROW_PARAM_ESCAPES` and the fresh-return/borrow-return
signature checks; String/slice share machinery.

### 8.3 Known fallout to schedule
1. Plain field/array stores that today transfer implicitly (LinkedList,
   HashMap internals, user code) must gain `#` — a stdlib sweep with
   compile errors pointing at each site.
2. `HashMap<K, UserClass>` runtime crash found 2026-07-10 (plain put +
   indexer read segfaults) — must be fixed en route; it blocks any
   container validation.
3. Signature updates: container mutators to `#V`/`#?V`, `operator#[]`
   additions, `remove` re-typed.
4. Element-ownership spec/plan disposition: supersession note added, Unit 8
   replaced by this spec's plan.
