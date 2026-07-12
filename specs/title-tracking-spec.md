# Title-tracking ownership

## 1. Definition

### 1.1 Purpose
One model for heap-object ownership across the whole language: every
allocation has exactly one **title** (the right and duty to free it); titles
live in **places** (locals, fields, array slots, container entries); the `#`
sigil is the only way a place surrenders its title. Checking is **hybrid**:
static where the compiler sees every use (locals), runtime state where it
cannot (fields, entries), with a hidden per-call ABI flag bridging the two
at every class-typed call edge (§4 — rev 2 2026-07-12: caller discretion
is the default; signatures carry no ownership spelling except the opt-in
`#V`).

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
   same-name methods differing only by `#` cannot coexist. Dual-mode
   behavior is therefore the *default* of the one plain signature (§4.1),
   never an overload pair; overrides must match the base's mode.
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
family. The `moveMask` thread-local is retired in favor of the per-call
ABI flag (§4.4) — the same intent, threaded explicitly instead of
ambiently.

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
   **owner** (fresh construction, `= #x` move from a static owner),
   **borrow** (bare identifier, field read, element read), or **runtime
   owner** (any class-typed call result — the return flag, §4.2, arms or
   leaves dormant its drop entry per call). Owners get a drop entry;
   borrows never do; runtime owners get a flag-fed entry.
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
6. A **runtime owner** (class-typed call result, §3.1.1; class-typed
   formal, §4.1) has its drop entry's active flag set from the call's
   flag. `#x` on a runtime owner forwards the flag rather than asserting
   static ownership; a definite compile-time answer is not required.

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
*(Rev 2, 2026-07-12: caller discretion replaces signature-declared modes.
Rev 1's three-spelling scheme — borrow / `#V` / `#?V` — required API
authors to anticipate every call site's ownership intent, which is
unknowable in general and untenable for collections: the ecosystem
endpoint was `#?` on everything. Rev 2 makes that endpoint the unwritten
default and deletes the `#?` spelling.)*

### 4.1 Formals — caller discretion by default
1. A class-typed formal `V v` is **caller-discretion**. The call site
   decides per call: a plain argument **lends** (flag false), `#x` or an
   owned rvalue (fresh construction, flag-true call result) **surrenders
   the title** (flag true). A hidden ABI flag (§4.4) carries the answer.
   Neither side pre-declares anything.
2. Inside the body, every class-typed formal is a **runtime owner**
   (§3.1.6): its drop entry is seeded from the call's flag. Storing or
   forwarding `#v` propagates the flag onward and *consumes* the formal
   (deactivates its callee entry). A flag-true formal never consumed is
   dropped by the callee at exit — the callee-side drop.
3. `#V v` — **opt-in must-own formal** for sinks whose correctness
   requires ownership (e.g. hand-off to another thread). A plain argument
   is rejected at compile time (`TRANSFER_REQUIRED`). ABI-identical to a
   plain formal with the flag pinned true, so dispatch stays mode-erased.
   *(Sub-fork A, pending developer decision: keep this spelling, or
   retire it and rely on runtime discipline. Recommendation: keep.)*
4. Consequence: the *static borrow formal* is retired for class types.
   `BORROW_PARAM_ESCAPES` no longer applies to class-typed formals — a
   store of a formal is legal and the flag decides the place's bit.

### 4.2 Returns — the flag rides back
1. A class-typed return `V f()` carries a paired hidden flag: the caller's
   drop entry (the receiving local is a runtime owner, §3.1.1) is active
   iff the flag is set. Sources: `return #x` forwards the owner's
   flag/title (true for static owners); a plain `return x` of a borrow is
   false; a fresh result is true. `remove` (§6.4) is the canonical
   dual-mode user.
2. Plain `return x` where `x` is a statically-owned local remains a
   compile error (`FRESH_RETURN_NEEDS_TRANSFER`) — the local would drop at
   exit and the caller would receive an immediately-dangling borrow. The
   fix is `return #x`.
3. `#V f()` — opt-in statically-owned return (flag pinned true);
   `operator#[]` (§6.3), which panics rather than return borrowed, is the
   canonical user. *(Rides sub-fork A.)*

### 4.3 Requirements
1. Same-name declarations differing only in transfer mode are impossible
   (dispatch is mode-erased) and rejected at declaration; an override's
   mode must match its base.
2. The only remaining static call-edge rejection is plain-into-`#V`
   (`TRANSFER_REQUIRED`). `#`-into-plain is not an error — it *is* the
   flag-true case. (Rev 1's planned `TRANSFER_NOT_ACCEPTED` is void.)

### 4.4 The ABI flag
1. Every class-typed formal contributes one hidden flag bit; the bits of
   one call pack into a single hidden word (layout owned by the plan; the
   per-instance fob word is the precedent). A class-typed return adds one
   paired flag alongside the value. The flag is per-call, threads through
   forwarding explicitly, and replaces the `moveMask` thread-local (whose
   failure was precisely that forwarding chains lost it).
2. Flag sources: `#x` at the call site (true — after linearity validated
   the source), plain arg (false), owned rvalue (true), a runtime owner's
   own flag (forwarded).
3. Flag sinks: the destination place's ownership bit (§5), the receiving
   runtime owner's drop-entry active flag (caller local or callee formal),
   or a forwarded call's flag.
4. The closed-world compile may elide flags provably constant at every
   edge (optimization, not semantics; virtual dispatch keeps the uniform
   word).

### 4.5 Use cases
1. As a container author, when I write
   `public void put(K key, V value)` — no ownership spelling — then
   `map.put(k, v)` lends and `map.put(k, #v)` hands over, both against the
   one method, and the entry records which one happened.
2. As a developer who hands over and then takes back
   (`put(k, #v); V v2 = remove(k);`), then `v2` owns the object (no drop
   fired in between), and any later read of `v` is `USE_AFTER_MOVE` naming
   the `#v` at the put.
3. As an API author of a sink that must own its input
   (`register(#Session s)`), when a caller passes a plain borrow, then the
   call is rejected at compile time. *(Rides sub-fork A.)*

### 4.6 Rejected shapes (recorded 2026-07-12)
1. **Signature-declared `#?V`** (rev 1): demands author foresight;
   converges on `#?`-everywhere; the spelling adds vocabulary without
   adding information once it's universal.
2. **Hard error on `#`-into-plain + caller-side release** (`f(#x)` lowers
   to borrow-call + caller drop at the sequence point): sound and
   ABI-free, but cannot let a callee retain, so it preserves the foresight
   problem for every retaining API; superseded by caller discretion.
3. **Per-call-mode dual lowering** (borrow body + transfer body, call site
   picks statically): 2^n bodies and mode-erased vtable dispatch cannot
   pick — the instantiation-mode/moveMask failure, already retired once.

## 5. Dynamic places — fields, slots, entries

### 5.1 Requirements
1. Class-reference **fields**, **array slots**, and **container entries**
   each carry a runtime ownership bit: *owned* (this place holds the
   title) or *borrowed*.
2. The bit is set by the store's spelling: `place = #x` → owned;
   `place = x` → borrowed; `#x` where `x` is a runtime owner (§3.1.6)
   → its flag. **The
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
`operator[]=` / `put` take the value at a plain formal — dual-capable by
default (§4.1.1), no signature spelling. The entry bit records the
outcome per §5. An own-only container may spell `#V` (rides sub-fork A).

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
`V remove(K key)` — structural removal, always ends membership, returns
the value in whatever mode the entry held it (title if owned, borrow if
borrowed). The flagged return (§4.2.1) is the "reference or own"
contract; the signature needs no spelling.

### 6.5 Use cases
1. As a cache user, when I `Session s = #cache[k]`, then I own the session,
   the cache still serves reads of `k`, and the cache's teardown no longer
   frees that entry.
2. As a developer who extracts the same key twice, when the second
   `#cache[k]` runs, then I get a panic naming the key's state — not a
   second title.
3. As a map user winding down, when I `V v = map.remove(k)`, then the
   entry is gone and `v`'s drop entry is active only if the map owned it.

## 7. Diagnostics, teardown, hazards

### 7.1 Compile-time (extends the existing escape/ownership family)
- move-out-of-borrow; use-after-move (incl. call-arg moves); plain into a
  `#V` formal (rides sub-fork A); mode-only overload/override collision;
  `#` on a value type; plain return of a statically-owned local
  (`FRESH_RETURN_NEEDS_TRANSFER`, §4.2.2).

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

Rev 2 widens exposure: lending into any retaining callee is spellable
everywhere (`m.put(k, s)` then `return #m` dangles the entry when `s`
drops). *(Sub-fork B, pending developer decision: add the single-hop
static check — returning/`#`-storing a container that provably holds a
borrow of a dying local — now in Unit 5's scope, or defer with the rest
of this hazard. Recommendation: defer; revisit as a Unit 8 lint once the
stdlib sweep shows the pattern's real frequency.)*

## 8. Migration

### 8.1 Retired
Type-argument `#` (parse may remain temporarily with a deprecation error);
`#Type` on a **local declaration** (`#MyClass x = ...` — a local's role
comes from its initializer shape, §3.1.1, so a type-position sigil on
locals is meaningless-at-best and can contradict the initializer; same
deprecation error); the `#?` signature spelling (rev 2 — never
implemented; caller discretion is the unwritten default, §4.6.1);
`BORROW_PARAM_ESCAPES` for class-typed formals (§4.1.4 — formals are
runtime owners now); dissolution/provenance gating (4A), call-agreement
instantiation checks (4B), declaration-`#`/contagion (Unit 5),
borrow-mode confinement gates (Unit 7), all four stdlib transitional
exemptions, `moveMask` thread-local and its runtime plumbing.
*(Local-declaration-`#` retirement approved 2026-07-11; rev 2 caller
discretion approved 2026-07-12.)*

### 8.2 Retained
Drop chain + entries; owned-element teardown walks (re-keyed from
instantiation mode to entry bits); typed value `clone()`; mode-erased
dispatch; the fresh-return check (`FRESH_RETURN_NEEDS_TRANSFER`, §4.2.2);
`#V`/`TRANSFER_REQUIRED` as the opt-in must-own edge (rides sub-fork A);
String/slice share machinery.

### 8.3 Known fallout to schedule
1. Plain field/array stores that today transfer implicitly (LinkedList,
   HashMap internals, user code) must gain `#` — a stdlib sweep with
   compile errors pointing at each site.
2. `HashMap<K, UserClass>` runtime crash found 2026-07-10 (plain put +
   indexer read segfaults) — must be fixed en route; it blocks any
   container validation.
3. Signature updates shrink to near-zero under rev 2: container mutators
   and `remove` keep plain signatures; `operator#[]` additions remain;
   existing stdlib `#V` formals either stay (sub-fork A: keep) or respell
   plain — call sites are unchanged either way, since `#x` args are legal
   against both.
4. Element-ownership spec/plan disposition: supersession note added, Unit 8
   replaced by this spec's plan.
