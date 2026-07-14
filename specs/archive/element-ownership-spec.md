# element-ownership — `#` type arguments: ownership at instantiation

> **SUPERSEDED (2026-07-14) by [title-tracking](title-tracking-spec.md).**
> The type-argument ownership layer specified here shipped through Unit 7
> of its plan and was then replaced by title-tracking rev 2 (caller
> discretion): per-call runtime titles on the hidden transfer word, `#`
> only at call/store/return sites, and `CAJETA_ERROR_TYPE_TRANSFER_RETIRED`
> on every type-position `#` this spec introduced. What survives: the `#`
> sigil itself, `TRANSFER_REQUIRED` on authored `#T` formals, clone()
> semantics (§6), and the slice/share machinery. Unit 8 as written never
> ran. Archived per title-tracking plan 7.3.1; the element-slot store
> story continues in [title-stores](title-stores-spec.md).

## 1. Definition

### 1.1 Purpose
Give a generic container a way to declare, **per instantiation**, whether it
**owns** its class-typed elements or merely **borrows** them, by prefixing a
type argument with the existing transfer sigil `#`:

```cajeta
HashMap<#String, Session> registry = heap HashMap<#String, Session>(64);   // owns keys
HashMap<String, int32>    scratch  = stack HashMap<String, int32>(16);      // borrows keys
```

Ownership of a stored element becomes a **static property of the type**, fixed
at instantiation and checked at the point of storage, instead of a runtime fact
carried by a per-call transfer mask.

### 1.2 Problem
Cajeta today disambiguates element ownership at runtime. `HashMap.put(K key)`
reads a thread-local transfer mask (`Cajeta.moveMask()`, set by the caller's
`#`) to decide whether the map took ownership of `key`, and records the answer
in a per-slot `owned` bit array so `~HashMap` knows which entries to drop.

This runtime-optional ownership is unsound at the boundary the compiler cares
about — the **store**. A String field store `slot.key = key` cannot be compiled
correctly, because whether it should **move** (transfer, one owner) or
**copy/share** (two owners) is not known until run time:

- Compiling it as a **move** dangles a borrowed source: the caller's scope
  frees a wrapper the slot still points at (the `SliceCallArgEscapeTests` /
  `NgramIndexTests` failure).
- Compiling it as a **copy** (the escape-resolution "silent resolve" of slices
  §4.2) strands an owned source: the caller transferred with `#`, so its drop
  was deactivated, but the callee copied instead of moving — the original is
  neither in the slot nor dropped (the `OwnershipLeakProbe` total leak — every
  `HashMap<String,V>` key leaks; root-caused to commit 9929c3e6, plan
  docs-refactor 15.12.1).

Three site-local patches were attempted and each fails for a structural reason
(recorded in full in docs-refactor plan 15.12.1):

1. **Store-site copy, then mark heap references shared** (the current code).
   Creates a second owner at a site the ownership ledger can't see → leaks the
   transferred source, and injects an implicit ownership copy on every use.
2. **Drop the stranded source inside `put`.** Double-frees class-typed keys
   (only String stores copy; class stores still move) → verified SIGSEGV.
3. **`#`-move the store inside `put`.** Rejected by the path-insensitive
   use-after-move checker.

The move that motivates a *signature*-level fix — make every parameter transfer
(`#K` formal) or every parameter borrow (`put(K)`) — also fails, because **the
same container class legitimately serves both roles**:

- As **storage**, a map must own its keys/values and drop them at teardown;
  `put` must demand a transfer.
- As **scratch** for sort / subset / dedup / group-by, a map is built over
  borrowed items, processed, and the *structure* discarded; `put` must take a
  plain borrow with no copy and no drop.

A single `put` signature cannot be right for both. Forcing the choice onto the
method yields dual APIs (`put`/`putRef` on every container) or drives ownership
back to runtime. The correct axis of variation is not the method and not the
call — it is the **instantiation of the element type**.

### 1.3 Scope
- `#` as an optional prefix on a **class-typed** type argument
  (`grammar typeArgument`), meaning "this instantiation owns elements at this
  position."
- `#` as an optional prefix on a **type-parameter declaration**
  (`grammar typeParameter`, `class Cache<#K, V>`), meaning "this parameter is
  owning-required" — non-dissolvable, and **contagious through inheritance**
  (§8.6). This is the two-meanings-of-`#` distinction (§4.1.5) and replaces a
  `requires`-style keyword.
- The three-point ownership agreement: **author markers** in the class body,
  **instantiation gating** by `#`, **call-site** transfer syntax — all three
  cross-checked at compile time.
- Confinement of borrow-mode (plain-instantiated) containers.
- Cross-mode transitions (borrow value → owned storage).
- The compile-time transfer check on parameterized classes: a `#`-transfer into
  a non-`#` type-argument position is an error (§3).
- Making the static path unnecessary to `moveMask` **for parameterized
  containers** (which migrate to static `#K`/`#V`), while `moveMask` is retained
  for concrete dual-role classes (§7). Gating the store-site escape resolution
  on source ownership so it stops copy-stranding owned sources, and fixing the
  missing-destructor element leak — the two actual bugs (§7).

### 1.4 Constraints
- **`moveMask` is retained (decision 2026-07-04).** The runtime transfer signal
  stays a supported primitive. It is proven sound — a concrete `moveMask` +
  owned-bit + drop-owned-at-teardown class leaks on neither the owned nor the
  borrow path (the `StringSet` probe, §7). This design **adds** a static
  ownership option (`#` type arguments) that coexists with `moveMask`; it does
  not delete it. A container author may choose static ownership (per
  instantiation, zero runtime cost) or the runtime mechanism (per call, for
  heterogeneous/mixed instances).
- **No *new* runtime ownership state.** The static path must not introduce a new
  runtime disambiguation mechanism; it resolves ownership at instantiation via
  monomorphization. Existing `moveMask`/owned-bits are kept as the runtime
  option, not extended.
- **Zero cost for the common cases.** A borrow-mode store is a pointer-width
  write; an owning-mode store of an already-owned value is a move (no byte
  copy). No allocation or refcount traffic on read-only element access
  (`get`, `containsKey`, iteration) in either mode.
- **Monomorphization only.** Ownership mode resolves at template instantiation;
  a generic body compiles to two codegens with no runtime branch on mode. (The
  toolchain already monomorphizes per type argument.)
- **Sigil consistency.** `#` means ownership at every position it appears
  (return, parameter, local, call argument, and now type argument). It never
  means anything else.
- **Fail loud, not silent.** Every ownership mistake must surface as a
  compile-time diagnostic or a structurally impossible state — never a silent
  leak and never a UAF.

### 1.5 Non-goals
- **Lifetime relaxation for borrow-mode containers.** v1 confines a
  borrow-instantiated container to its declaring scope. Proving that a
  borrow-mode collection whose elements outlive it may itself escape (return,
  field store) is deferred.
- **Per-element mixed ownership.** A single container instance owning some
  elements and borrowing others is explicitly removed — it is the capability
  that forced ownership to run time, and no real workload is known to need it.
- **Implicit cross-mode assignability.** `HashMap<#String,V>` and
  `HashMap<String,V>` are distinct types; converting between them is an
  explicit, named operation.
- **Removing `moveMask`.** The runtime transfer signal is retained (§1.4). This
  design adds a static option and constrains where `#`-transfer is legal on
  parameterized classes; it does not delete the runtime mechanism, which remains
  the sanctioned path for concrete dual-role classes.
- **Reference/`ref` element types as a general feature** beyond container
  element positions (i.e. this is not a general borrowed-field or
  reference-variable feature; those remain governed by the borrow/`#` rules
  already in the language).

### 1.6 Relationship to prior work
This supersedes the **silent-resolution rows of the slices plan §4.2** (the
copy-≤256B / share-large / static-alias escape-resolution table applied
implicitly at String field stores). Escape resolution is retained but **gated**:
it no longer runs *ungated* at every store to guess ownership; it fires only for
a borrow source into an owning position (the automatic materialize, §6.1.1) and
as an allocation-mode optimizer, while owned sources move. The rc
substrate built for slices (`__cajeta_shared_*`, the tagged value layout, the
synthesized value copy/drop hooks, `emitValueSharedOp`) is reused, not
discarded — it becomes the mechanism behind owning-mode element storage and the
zero-copy borrow→owned promotion.

This is the durable fix for the leak tracked as docs-refactor 15.12.1 (and the
sibling container-teardown leak 15.13); landing it lets those items close by
removing the store-site copy-strand and giving owned elements a drop, rather
than by another site-local patch. `moveMask` itself is retained — it is not the
cause of either leak (proven by the leak-free `StringSet` probe, §7).

## 2. Element ownership modes

### 2.1 Requirements
1. A class-typed type argument MAY be prefixed with `#`. `HashMap<#String, V>`
   instantiates a map that **owns** its keys; `HashMap<String, V>` instantiates
   a map that **borrows** its keys. The two are distinct instantiations
   (§8.3).
2. In an **owning** instantiation, the container is the sole owner of each
   stored element at that position: elements are dropped when the container
   drops (they join the container's normal field-drop walk), and storing an
   element requires the caller to transfer ownership (§3).
3. In a **borrowing** instantiation, the container holds references it does not
   own: storing an element is a pointer-width write, the container's teardown
   drops the **structure only** (never the elements), and the container is
   scope-confined (§5).
4. Mode is independent per type-argument position. `HashMap<#String, Session>`
   owns keys and borrows values; `HashMap<#String, #Session>` owns both;
   `HashMap<String, #Session>` borrows keys and owns values.
5. `#` on a **value-semantics** type argument (a primitive, or a `@ValueType`
   POD with no shared-capable fields) is meaningless and is a compile error
   (§8.1) — the element carries no separable ownership.

### 2.2 Use cases
1. As a **service author** building a session registry that must outlive any
   request, when I declare `HashMap<#String, Session>` and insert keys, then the
   map owns the keys and frees them at teardown, and I cannot accidentally
   insert a key the map won't free.
2. As a **data-processing author** grouping borrowed rows for a one-shot
   aggregation, when I declare `HashMap<String, ArrayList<Row>>` over borrowed
   rows, then insertion copies nothing, the scratch map drops no rows when it
   goes out of scope, and the compiler stops me from letting the scratch map
   escape (§5).
3. As a **library author**, when I write one `HashMap` class, then both callers
   above use it unchanged — the storage/scratch distinction is theirs to make
   at instantiation, not mine to duplicate into two APIs.

## 3. The three-point agreement

Ownership is expressed at three points; the compiler checks that all three
agree, and disagreement is always a diagnostic.

| Point | Syntax | Written by | Meaning |
|---|---|---|---|
| Class body | `#K` on a declaration | container author | "ownership *may* apply at this position" |
| Instantiation | `HashMap<#String, V>` | user | "ownership *does* apply for this instance" |
| Call site | `m.put(#k, v)` | user | "here is the handoff" |

### 3.1 Requirements
1. **Author markers.** In the class body, the author writes `#K` (or `#V`) at
   exactly the positions where element ownership can apply — the storage field
   and the parameters/returns that transfer elements in or out. Plain `K` in the
   class body is **always** a borrow, in both instantiation modes (§4.1).
2. **Instantiation gating.** A `#`-marked position materializes as a real
   ownership position only when the corresponding type argument is `#`-prefixed.
   Under a plain (non-`#`) instantiation, the author's `#K` markers **dissolve
   to borrows** (§4.2) — every container is dual-mode by default.
3. **Call-site agreement.** At a call, the caller's transfer syntax must match
   the instantiation:
   - Storing into an **owning** position requires a transfer — `m.put(#k, v)`,
     or an rvalue that is itself owned (a fresh construction, a `#`-returning
     call result). A plain owned local passed without `#` is a compile error
     whose fix is stated: transfer with `#k`, or duplicate with `.clone()`
     (§6).
   - Storing into a **borrowing** position takes a plain borrow — `m.put(k, v)`.
     On a parameterized class, **attempting a `#`-transfer where the
     corresponding type argument is not `#`-marked is a compile error** —
     `HashMap<String,V> m; m.put(#k, v)` is rejected. This is not a style rule:
     a borrow-mode container drops nothing, so transferring an owned value into
     it moves ownership to a container that never frees it — a guaranteed leak.
     The error names both fixes: instantiate `HashMap<#String,V>` to take
     ownership, or drop the `#` to store a borrow.
4. **Scope of the type-argument check.** The agreement in 3.1.3 governs
   **parameterized** classes, where the type argument statically carries the
   ownership mode and the compiler checks each `#` (or its absence) at the call.
   A **concrete** (non-generic) class has no type argument; it MAY accept an
   optional `#` at run time via `moveMask` (§7) — its `add(T)` legitimately reads
   the transfer intent per call. The two mechanisms never overlap: generic
   element ownership is static and checked; concrete dual-role ownership is
   runtime. A generic container therefore cannot be `moveMask`-based — a `#` into
   a plain type-argument position is an error, not a runtime-resolved transfer.
5. **One diagnostic family.** All three checks report through the same
   escape/ownership diagnostic (the `FieldBorrowEscape` lint the language
   already sketches), promoted to an **error** where the analysis is certain and
   a **warning** where it is not — never a silent copy.

### 3.2 Use cases
1. As a **developer** who forgets the transfer on an owning map
   (`registry.put(k, s)`), when I compile, then I get an error at the call site
   naming the fix (`#k` / `.clone()`), not a silent leak discovered in
   production.
2. As a **developer** who mistakenly transfers into a scratch map
   (`scratch.put(#k, n)`), when I compile, then I get an error telling me the
   map borrows and won't free the key — not a double-drop at teardown.
3. As a **reviewer**, when I read `HashMap<#String, Session>` at the declaration
   site, then I know the ownership contract of every `put` on that value without
   reading `HashMap`'s source.

## 4. Author-side declaration and gating

### 4.1 Requirements
1. **Storage positions carry the marker.** The element-storage field is
   declared with the sigil — e.g. `MapEntry<#K, #V>[] slots` — so an owning
   instantiation stores owned elements (dropped by the field-drop walk) and a
   borrowing instantiation stores borrows.
2. **Mutators that take ownership carry the marker.** `public void put(#K key,
   #V value)` — under an owning instantiation these are real transfer formals
   (the existing `#T`-parameter semantics); under a borrowing instantiation they
   gate to plain borrows.
3. **Read-only accessors never carry the marker.** `containsKey(K key)`,
   `V get(K key)`, iteration/stream element yields — all use plain `K`/`V` and
   are borrows in **both** modes. Gating never adds ownership to a plain-`K`
   position, so `get` can never become element-stealing.
4. **Removal has two forms; extraction opts in explicitly.** Un-storing an
   element splits by what happens to it:
   - **Drop-remove** — `boolean remove(K key)` unlinks the entry and, in owning
     mode, **drops** the owned element (returns whether it was present); in
     borrowing mode it just unlinks (drops nothing). Plain `K` parameter, valid
     in both modes.
   - **Extract-remove** — a method that hands the removed element *out* by
     ownership declares `#V` on its return (`#V take(K key)`, or
     `Optional<#V> take(K key)` for the may-be-absent shape). It transfers the
     owned element to the caller (the container relinquishes it, dropping
     nothing). Valid **only on an owning instantiation**; on a borrowing
     instantiation it is a compile error (there is no owned element to hand
     out — the caller should read the borrow via `get` and `.clone()` it if
     needed).
   Gating never turns a plain-`K` accessor into an extractor; extraction is
   always an explicit `#`-return the author writes.
5. **Two meanings of `#`, and owning-required parameters.** `#` marks ownership
   at two authoring positions, with different force:

   | `#` position | Meaning | Dissolves under a plain instantiation? | Contagious through inheritance? |
   |---|---|---|---|
   | **Use / storage** — `#K slots`, `put(#K)` (§4.1.1–2) | ownership *permitted* — dual-mode | **Yes** — dissolves to a borrow | No |
   | **Type-parameter declaration** — `class Cache<#K, V>` | ownership *required* | **No** — `Cache<String,V>` is an error | **Yes** (§8.6) |

   A plain-declared parameter (`class HashMap<K, V>`) is **dual-mode**: its
   use-site `#K` markers permit owning and dissolve to a borrow under a plain
   instantiation. A `#`-declared parameter (`class Cache<#K, V>`) is
   **owning-required**: every instantiation must supply `#` (`Cache<#String,V>`
   is valid; `Cache<String,V>` is a compile error). Reach for declaration-`#`
   when a class has owning-only methods — a `#V take(...)` extractor (§4.1.4) —
   and would rather reject borrow-mode instantiation up front than let it compile
   and fail at that method. This is the whole of "requires owning": the sigil on
   the declaration carries it, so there is no `requires` keyword.

### 4.2 Use cases
1. As the **HashMap author**, when I write `MapEntry<#K, #V>[] slots`,
   `put(#K, #V)`, `containsKey(K)`, `get(K)`, and a plain `~HashMap()`, then
   both `HashMap<#String,V>` and `HashMap<String,V>` compile from that one body
   — owning drops keys via the field walk, borrowing drops nothing.
2. As a **cache author** whose `take(...)` extractor is owning-only, when I
   declare `class Cache<#K, V>` (declaration-`#`), then a caller who writes
   `Cache<String,V>` gets a clear instantiation error up front instead of one
   buried at the `take` call site.
3. As an **author of a returns-owned method**, when I mark `#K removeKey(...)`,
   then it is available on owning maps and rejected on borrowing maps at compile
   time.

## 5. Borrow-mode confinement

### 5.1 Requirements
1. A borrow-instantiated container (any type argument left plain where the
   author marked `#K`) holds references it does not own, and therefore **must
   not outlive its elements.** v1 enforces this conservatively: such a container
   value is **scope-confined** — it may not be stored into a field, returned via
   `#`, moved into an owning container, or captured by an escaping closure.
2. Confinement is enforced by the same borrow-store diagnostic (§3.1.5) applied
   to the container value itself, not only to its elements.
3. Read-only pipelines over a borrow-mode container — `sort`, `subset`,
   `dedup`, `groupBy`, iteration, fold — are unaffected: they permute or read
   and do not escape the structure.
4. A generic function that is **mode-agnostic** — one that only permutes or
   reads elements, e.g. `void sort<T>(ArrayList<T> xs)` — compiles once and
   accepts both owning and borrowing instantiations, because it neither stores
   nor drops elements.
5. **An element borrow does not outlive its container — in either mode.** The
   borrow returned by a read-only accessor (`get`, `containsKey`'s probe,
   iteration/stream yields, §4.1.3) references storage the container owns
   (owning mode) or forwards (borrowing mode); it must not outlive the container.
   The enforcement follows the general rule (§6): flowing that borrow into an
   **owning** position (owned field, owning container, `#`-return) **auto-
   materializes** it into an independent owned value — safe, no dangle, no method
   call. It can only *dangle* if it escapes as a **raw borrow** into a
   longer-lived borrowing position; that is a confinement error, and the same
   destination-place check as §3.1.5 / §6.1.7 catches it (the container is the
   base whose lifetime bounds the borrow). This is why accessors stay plain in
   both modes — the *access* is always a borrow, and the compiler owns the
   borrow→owned step wherever it's needed; `clone()` (§6.1.3) and an explicit
   extractor (§4.1.4) remain as the *deliberate* ways to take an owned value.

### 5.2 Use cases
1. As a **developer** sorting a borrowed slice of records, when I build
   `ArrayList<Record> scratch` over borrows, sort it, and read the top N, then
   there is zero element copy and zero element drop, and the scratch list frees
   only its backing array at scope exit.
2. As a **developer** who tries to return a borrow-mode scratch collection out
   of the function that built it, when I compile, then I get a confinement error
   pointing me at the fix (build an owning collection, or `.clone()` at the
   boundary — §6), rather than a dangling container.
3. As a **library author** of `sort<T>`, when I write it once against
   `ArrayList<T>`, then it serves both `ArrayList<#String>` and
   `ArrayList<String>` callers with no duplication.

## 6. Cross-mode transitions, and `clone()`

Two separate things, distinguished up front because conflating them is what
caused the leak: (a) the **automatic** borrow→owning transition — no method call;
and (b) **`clone()`**, a general, *explicit* duplicate the developer reaches for
deliberately. `clone()` is not the transition mechanism; the transition is
automatic.

### 6.1 Requirements
1. **Borrow → owning is compiler-automatic and safe — no method call.** When a
   borrow flows into an owning position (an owning container element, an owned
   field, a `#`-return), the compiler **materializes** it. This is safe for a
   reason the old implicit copy was not: a **borrow owns nothing**, so
   materializing it strands no owner. (The leak came from copying an *owned*
   source and deactivating its drop as if moved — §1.2. A borrow has no drop to
   deactivate.) The materialization is provenance-chosen and reuses the slices
   value hook (`emitValueSharedOp`), runtime-dispatched on the value's mode tag
   when the root isn't statically known:
   - **immutable heap root** (String bytes, immutable `Slice`) → promote
     `owned→shared` and take a stake — **no byte copy**; the owning destination
     co-owns the root by refcount and releases it on drop.
   - **stack root, or a mutable/non-shareable backing** → materialize a real
     owned heap copy (a stack backing dies with its frame; a mutable backing must
     not be shared or the copy would alias).
   This is the store-site resolution of old, but now **gated** to borrow sources
   into owning positions (§7.1.3) — where it is always correct — instead of firing
   on every store including owned sources.
2. **`#` opts an owned source into a move.** For an *owned* source into an owning
   position, `#x` **moves** it (consumes `x`, no copy) — the efficient path.
   A plain owned source into an owning position is a compile error that forces
   the choice explicit: `#x` to move, or `x.clone()` to store a copy and keep the
   original (§6.1.3). For a *borrow* source there is no move — nothing is owned to
   transfer — so the plain form auto-materializes per 6.1.1 and needs no `#`.
3. **`clone()` is the general explicit-duplicate operation — retained and, for
   reference types, necessary.** Distinct from the automatic transition,
   `clone()` produces an independent owned duplicate on demand: signature
   `#T clone()`, never consumes the receiver. It is:
   - **the overridable `Object.clone()` for reference types** — *irreplaceable*,
     because assignment of a reference **aliases** it; `clone()` is the only way
     to obtain a second independent heap object. (`Object.clone()` already exists
     as a `null` placeholder "until ownership semantics stabilize" — this spec is
     that stabilization.)
   - **compiler-synthesized `#T clone()` for shared-capable value types** (String,
     `Slice<T>`, value aggregates) — for explicitly copying an owned value while
     keeping the original, or forcing a copy where the automatic transition would
     have shared.
   `clone()` is *not* required to store a borrow into an owning container (that is
   automatic, 6.1.1); it is the tool for deliberate duplication.
4. **`clone()` is copy-on-write**, by the same provenance rules as the automatic
   materialize (6.1.1): immutable heap root → share via refcount (no byte copy;
   sound because immutability makes a shared and a copied duplicate observably
   identical); stack/mutable backing → real copy; primitive/POD → plain value
   copy.
5. **Duplicating an owned *value* is also just assignment.** For a value type,
   `String b = a;` copies through the COW hook, yielding an independent `b`. So
   explicit `clone()` on a value type is only needed when you want the copy to be
   conspicuous; the case where `clone()` is *load-bearing* is reference types,
   where assignment aliases and there is no other way to duplicate.
6. **Escape analysis becomes an optimizer, not a gate.** The escape analysis
   (`nameEscapesScope`) no longer decides borrow-vs-copy at a store (its former,
   unsound-when-wrong role). It runs to *elide* work: a value proven never to
   escape stays a plain borrow with no stake; a value proven to flow only into
   owning positions may be **born owned** at its definition so its later
   transfer is a move rather than a materialize. A miss in either direction
   costs at most a wasted refcount or a heap allocation that could have been
   avoided — never a dangling pointer. This is the correct fail-safe polarity
   (slices spec §4.3): over-approximate toward *more* ownership/retention, never
   toward less.
7. **Escape is classified from the destination place, and its decidable core
   is syntactic.** "Could this assignment leave the owning scope?" is answered
   by classifying the **destination place's lifetime relative to the current
   scope**, not by tracing the source. General escape (a heap object reachable
   through locals whose own escape is uncertain) is undecidable, so the analysis
   is a **sound conservative approximation** — but two large ends are decidable
   *syntactically*, with no dataflow fixpoint:
   - **Provably non-escaping:** the destination place's base is a same-scope
     local (of a stack value, or of a heap object whose handle provably does not
     itself escape), and the value does not also flow to a `#`-return, a
     `#`-argument bound to an owning formal, or an escaping closure capture.
     These stay plain borrows.
   - **Provably escaping:** the destination is a field of `this`, a parameter, a
     static, or an element of a longer-lived container; or the value is a
     `#`-return operand or a `#`-argument matched to an owning `#T` formal; or it
     is captured by a closure that escapes. These are owned/retained.
   - **Uncertain middle** (a base whose own escape is not proven) is treated as
     **escaping** — the safe fail direction of 6.1.6. A false positive costs a
     refcount or an owned allocation, never a dangle.
   The recognizer is the destination-place classifier described above (base
   lifetime, recursing through `.field`/`[index]` to the base identifier), a
   re-pointing of the existing sub-node walker (`collectSubNodes` /
   `nameEscapesScope`) from "does this name appear under a blocking construct"
   to "does this destination place outlive the scope."
8. **The `#`-instantiation design removes escape from the correctness path.**
   Because ownership is static per instantiation, the container store itself
   (`slot.key = key` in an owning `HashMap<#K,…>`) is unconditionally a move —
   the compiler does not run escape analysis there at all; the instantiation
   already declared that the slot owns. The escape query survives in exactly one
   place: the allocation / materialize decision (6.1.6) — "born owned because it
   will escape" vs "kept a cheap local borrow." There a wrong answer is only a
   performance question. The design thus converts a correctness-critical escape
   query (today's dangling store) into a performance-only one (allocation-mode
   selection): the compiler is precise exactly where being wrong is merely slow,
   and never needs precision where being wrong would be unsafe.

### 6.2 Use cases
1. As a **developer** inserting a freshly built key into an owning map
   (`registry.put(#k, s)` for an owned `k`), when I compile, then `#` moves it in
   — no copy — and a plain `registry.put(k, s)` is an error telling me to pick
   `#k` (move) or `k.clone()` (copy, keep the original).
2. As a **developer** storing a heap-backed substring **view** into an owning map
   (`registry.put(view, s)`), when I compile, then the compiler materializes it
   automatically — the immutable root is promoted to shared and the map takes a
   stake, no bytes copied, no method call — and I never touch `clone()`.
3. As a **developer** storing a **view over a stack buffer** into an owning map,
   when I compile, then the compiler materializes an owned heap copy
   automatically (the one case a copy is unavoidable); it is silent but safe —
   never a dangle.
4. As a **developer** who wants a *second independent copy of a heap object*
   (a reference type), when I write `obj.clone()`, then I get a distinct object —
   because a plain `Object b = obj` would only alias the same instance. This is
   the case `clone()` exists for.
5. As a **compiler**, when I prove a local flows only into an owning collection,
   then I allocate it owned at birth so its transfer is a move; when I prove a
   local never escapes, I keep it a plain borrow — and if I am wrong either way,
   the result is wasted work, never a UAF.

## 7. Coexistence with the runtime transfer mechanism; store and drop correctness

`moveMask` is retained (§1.4). The two ownership mechanisms are kept disjoint by
the type-argument check (§3.1.3–4): a **parameterized** container expresses
element ownership statically via `#` type arguments and never consults
`moveMask`; a **concrete** container with no type argument MAY use `moveMask` +
owned-bits + a drop-owned destructor for per-call runtime ownership. That
runtime triple is proven leak-free — the `StringSet` probe (a concrete
`moveMask`-based set) nets zero live objects on both the owned-transfer path
(`add(#k)`) and the borrow path (`add(ref)`). The two leaks in the stdlib are
therefore *not* caused by `moveMask`; they are two independent bugs at the store
and the drop, fixed below regardless of which mechanism a container uses.

### 7.1 Requirements
1. **`moveMask` retained; mechanisms disjoint.** A parameterized container
   resolves element ownership at instantiation (checked at the call, §3.1.3) and
   does not read `moveMask`. A concrete container MAY read `moveMask` for
   per-call ownership. `HashMap`, being parameterized, migrates from its current
   `moveMask` + `owned`-bit implementation to static `#K`/`#V`; a concrete
   dual-role class keeps the runtime triple (as `~StringSet` demonstrates).
2. **owned-bits removed only where a container goes static.** A parameterized
   container that adopts `#K`/`#V` drops its per-slot `owned` array — teardown
   follows the instantiation via the field-drop walk over `#`-marked storage. A
   concrete container that keeps `moveMask` keeps its owned-bits; they are the
   correct mechanism there.
3. **Store-copy bug fixed by *gating*, not blanket removal.** The materializing
   store-site resolution (`__cajeta_string_resolve` at the assignment site) is
   **retained but gated on statically-known source ownership**: it fires only for
   a **borrow** source into an owning position (where materializing is correct —
   a borrow strands nothing, §6.1.1), and is **suppressed** for an **owned**
   source, which compiles as a **move** for every LHS shape — array-element
   (`slots[i] = v`) *and* field-of-element (`slots[i].key = k`). The HashMap leak
   (15.12.1) was precisely the *ungated* version copying owned sources and
   deactivating their drops; the `#`-design makes source ownership static, so the
   compiler emits move-for-owned / materialize-for-borrow instead of copy-for-all.
   Required whether the container is static or runtime.
4. **Element-drop completeness fixed independently of the mechanism.** Owned
   class-typed elements must be dropped at teardown. Static path: a `#K`/`#V`-
   marked storage field joins the automatic field-drop walk via a synthesized
   loop bounded by a container-designated **live-count field** (an explicit
   count marker — the array header word is capacity, not live count, so an
   unmarked walk would drop garbage in `[size..capacity)`), dropping elements
   with no hand-written destructor **for contiguous count-bounded containers**;
   a non-contiguous slot store (HashMap's ctrl-mapped `MapEntry[]` shape) keeps
   a bespoke ctrl-aware destructor. Element drops route through the idempotent
   live-set claim, so aliased elements free exactly once. *(Amended 2026-07-06,
   plan 3.2.2 sign-off, per the prior owning-collections design.)* Runtime
   path: the container drops owned
   elements in its destructor (as `~HashMap` and `~StringSet` do). This closes
   the second leak family (15.13): `ArrayList`, `HashSet`, `Heap`, and the trees
   have **no** element-drop destructor today (`~HashMap` is the only one in the
   collection package), so `ArrayList<#T>` leaks one payload per owned element —
   fixed by marking their storage `#T` (static → auto-drop).
5. **Failure modes.** Static path: the leak and the double-free are
   *structurally impossible* (owned stores move; owned elements auto-drop), and
   the dangle is caught by confinement (§5) and the store diagnostic (§3.1.5).
   Runtime path: correct by construction — the `StringSet` probe is empirically
   leak-free on both the owned and borrow paths, with balanced shared population.

### 7.2 Use cases
1. As a **runtime maintainer**, when this lands, then I remove the
   `__cajeta_string_resolve` store-site copy (the 15.12.1 leak) and give the
   destructor-less containers an element drop (the 15.13 leak) — the two actual
   bugs — while leaving `moveMask` intact as the runtime option; `HashMap`'s own
   `owned`-bits go away only because it migrates to static `#K`/`#V`.
2. As a **library author** writing a concrete dual-role collection, when I use
   `moveMask` + owned-bits + a drop-owned destructor, then it compiles and runs
   leak-free (the `StringSet` shape), because the language still supports the
   runtime mechanism where a type argument can't carry the mode.
3. As a **developer**, when I insert into a parameterized container, then whether
   the element is owned or borrowed is decided by the type argument I wrote, is
   visible at the declaration, is checked at the call, and cannot silently differ
   from what the container's teardown assumes.

## 8. Interactions and edge cases

### 8.1 Requirements — value-semantics arguments
1. `#` on a primitive type argument (`ArrayList<#int32>`) is a compile error:
   primitives carry no separable ownership.
2. `#` on a `@ValueType` POD with no shared-capable (heap-owning) fields is
   likewise an error; on a value type that *does* transitively own heap payload
   (e.g. a record with a `String` field), `#` is meaningful and gates that
   payload's ownership through the synthesized value copy/drop hooks.

### 8.2 Requirements — composition and nesting
1. `#` composes through nesting, and the mode is read per position (`#` on a
   position = that position is owned). Worked examples:
   - `HashMap<#String, #ArrayList<#Session>>` — owns keys, owns the lists, lists
     own their sessions. Fully owning; drops everything at teardown.
   - `HashMap<#String, ArrayList<#Session>>` — owns keys, **borrows** the lists
     (no `#` on the value position). The lists are owning-mode (they own their
     sessions), so borrowing them is fine; the map is then **confined** to the
     lists' scope (§5).
   - `HashMap<String, ArrayList<Session>>` — borrows everything. Scratch;
     confined.
2. **You cannot `#`-own a borrow-mode container.** `HashMap<#String,
   #ArrayList<Session>>` — owning (`#`) a value that is a *borrow-mode*
   `ArrayList<Session>` — is a **compile error**. A borrow-mode container is
   scope-confined and owns nothing; storing it into an owning position would
   launder its borrowed `Session`s past their scope through a longer-lived owner
   — exactly the dangle confinement (§5.1.1) forbids. The fix the error names:
   make the inner container owning (`#ArrayList<#Session>`) so the outer owner
   genuinely owns a self-contained subtree, or borrow it (drop the outer `#`) and
   accept the outer container's confinement. This makes "own a borrow-mode
   container" structurally impossible rather than "conservatively confined," and
   removes the need for deep confinement propagation in v1 (the only nesting that
   confines is *borrowing* an inner container, which §5 already handles). Deep
   *relaxation* — letting a borrowing outer container escape when its borrowed
   subtree provably outlives it — remains deferred (§1.5).

### 8.3 Requirements — type identity and assignability
1. `HashMap<#String, V>` and `HashMap<String, V>` are **distinct types**. There
   is no implicit conversion or assignability between them.
2. Crossing modes at the **container** level — building an owning collection
   from a borrowing one — is the explicit `collectOwned()` operation (materialize
   each element into an owning collection), so the cost has a name and a site.
   (Crossing modes for a single *value* is the automatic materialize of §6.1.1;
   `collectOwned()` is its bulk, container-level counterpart.)

### 8.4 Requirements — grammar
1. `grammar typeArgument` gains an optional `REFERENCE` (`#`) prefix on its
   `typeType` alternative: `REFERENCE? typeType | primitiveType | integerLiteral
   | annotation* '?' …`. This is the **type-argument** `#` (instantiation).
   `#` here is unambiguous (it is never infix) and reuses the existing
   `REFERENCE` token that already prefixes return, parameter, local, and argument
   positions. `<#` occurs nowhere in the current grammar or sources.
2. `grammar typeParameter` gains the same optional `REFERENCE` (`#`) prefix on
   the parameter identifier: `annotation* REFERENCE? identifier (EXTENDS …)? …`.
   This is the **declaration** `#` (owning-required parameter, §4.1.5) — a
   distinct position from item 1, reusing the same token. `class Cache<#K, V>`
   parses `#` as the `REFERENCE` prefix on the `K` `typeParameter`.

### 8.5 Requirements — method-level templates and wildcards
1. A method-level type parameter (`U f<U>(...)`) may carry `#` at its use
   positions under the same author-marker/gating rules; a `#U` binding in a
   method template obeys §3–§4.
2. Wildcard arguments (`?`, `? extends T`) interact with `#` as ordinary
   type-argument syntax; a `#`-prefixed wildcard is out of scope for v1 (a
   wildcard names an unknown type, for which an ownership commitment cannot be
   checked) and is a compile error until specified.

### 8.6 Requirements — inheritance and owning-required parameters
1. An owning-required parameter (declaration-`#`, §4.1.5) is **contagious
   through inheritance**: a subclass extending a base with a `#`-declared
   parameter must **preserve the `#`** — either **satisfy** it with a `#`-type or
   **reproject** it as the subclass's own `#`-declared parameter. Dropping the
   `#` is a compile error.
   - **Satisfy:** `class StringCache extends Cache<#String, V>` ✓
   - **Reproject:** `class MyCache<#K, V> extends Cache<#K, V>` ✓
   - **Launder:** `class Leaky<K, V> extends Cache<K, V>` — **error**: `K` is
     owning-required in the base, so a plain reprojection would let
     `Leaky<String,V>` present a borrow-mode subclass of an owning-required base,
     defeating the requirement. The error names the two fixes (satisfy or
     reproject with `#`).
2. The rule keys on the **declaration**-`#` only. A dual-mode base (`class
   HashMap<K, V>` with use-site `#K` *storage*, §4.1.1) imposes nothing on
   subclasses: a plain reprojection `class Sub<K, V> extends HashMap<K, V>`
   forwards the mode faithfully (`Sub<#String,V>` owns, `Sub<String,V>` borrows).
   Only the *required* form propagates, because only it is a constraint there is
   anything to preserve.
3. The check is structural and shallow — one edge of the `extends` graph at a
   time. A subclass that satisfies with a `#`-type terminates the contagion
   (nothing downstream to preserve); a subclass that reprojects `#K` carries the
   requirement onto its own `K`, so its subclasses are bound by the same rule.

### 8.7 Use cases
1. As a **developer**, when I write `ArrayList<#int32>`, then I get an error
   telling me `#` is meaningless on a primitive, not a silently ignored sigil.
2. As a **developer** writing the fully-owning nest `HashMap<#String,
   #ArrayList<#Session>>`, then every level owns and all three drop at teardown,
   with no annotation beyond the `#`s I wrote; and if I slip and write
   `#ArrayList<Session>` (own a borrow-mode list), I get the §8.2.2 error naming
   the fix rather than a silent dangle.
3. As a **developer** who assigns a `HashMap<String,V>` where a
   `HashMap<#String,V>` is expected, then I get a type error, and the fix
   (`collectOwned()`) is the honest cost of the conversion.
4. As a **developer** subclassing an owning-required base
   (`class Leaky<K,V> extends Cache<K,V>` where `Cache<#K,V>`), when I compile,
   then I get the §8.6.1 error telling me to satisfy (`Cache<#String,V>`) or
   reproject (`class MyCache<#K,V> extends Cache<#K,V>`) — I cannot silently
   launder the ownership requirement into a borrow-mode subclass.
5. As a **developer** subclassing a dual-mode base
   (`class Sub<K,V> extends HashMap<K,V>`), when I compile, then it just works
   and stays dual-mode — the inheritance rule fires only for declaration-`#`
   bases, so ordinary generic subclassing is unaffected.

## 9. Migration and validation

### 9.1 Requirements
1. **Stdlib sweep.** Container classes (`HashMap`, `HashSet`, `ArrayList`,
   `LinkedList`, `Cache`, the tree maps, `Collectors`, the stream sources) mark
   their storage fields and transfer mutators with `#K`/`#V`, leaving accessors
   plain. The idiom is already partly present — stores already read
   `put(#owned, #bucket)`, `out.add(#g)` — so the sweep formalizes existing
   intent rather than rewriting call sites.
2. **The stdlib's two leaks have *different* causes; both are fixed and
   `moveMask` is kept.** Established empirically (the `StringSet` / `Iso`
   probes, 2026-07-04):
   - **HashMap** stores keys via a *field-of-element* form
     (`slots[i].key = key`, a `DotExpression` LHS), which triggers the
     store-site escape resolution and compiles as a **copy** — stranding the
     transferred source (15.12.1). Its `~HashMap` correctly drops what sits in
     the slot, but that is the copy; the original leaks.
   - **ArrayList, HashSet, Heap, the trees** store via a plain *array-element*
     form (`data[i] = v`, an `ArrayIndexExpression` LHS), which the resolution
     does **not** touch — it compiles as a correct move. Their leak is the
     opposite: they have **no element-drop destructor** (`~HashMap` is the only
     one in the collection package), so owned elements are never freed (15.13).
     Measured: `ArrayList<String>` owned-insert ×1000 → live +1006.
   The `#K`/`#V` marker fixes both: the marked field-store compiles as a move
   (no resolution to copy it), and marked storage joins the field-drop walk (its
   elements get dropped). `HashMap` migrates off `moveMask`/owned-bits to static;
   the destructor-less containers gain element drop via the marker. `moveMask`
   the primitive is retained for concrete classes (§7); `HashMap`'s own
   `owned`-bits disappear only because it goes static, not because the mechanism
   is deleted.
3. **Concrete field stores need no new annotation and lose the ctor-skip
   hack.** Non-generic classes that take ownership already use **fixed `#T`
   formals** (`Exception(#String)`, `StackFrame(#String …)`, `Command(#String[])`,
   the `Optional` ctor) — statically owned, never a runtime question — and store
   into class fields that are owned by default. With the resolution **gated on
   source ownership** (§7.1.3), `this.field = param` where `param` is a `#`-formal
   at its last use is simply a **move** (the owned source suppresses the
   materialize; deactivate the source drop, hand the payload to the owned field).
   This retires the current "resolution SKIPPED inside constructors" special
   case: constructors stop being special because the gate — not a ctor exception
   — is what keeps a statically-owned source from being copied. Concrete fields are not
   dual-role (an `Exception` always owns its `message`), so they need no
   `#`-on-field marker; only container *element* positions are parametric, which
   is exactly what the type-argument `#` addresses.
4. **Simultaneous oracles.** The design is correct only if all three existing
   probes pass **at once**, which no site-local patch has achieved:
   - `OwnershipLeakProbe` (the leak side) — owned stores must move, not copy.
   - `SliceCallArgEscapeTests` / `NgramIndexTests` (the dangle side) — escaping
     views must keep a stake or be confined.
   - `sharedPopulation` balance checks (the accounting side) — promotions and
     releases must net to zero.
5. **New tests.** Instantiation-mode agreement (owning store requires `#`;
   borrowing store rejects `#`), borrow-mode confinement (return/field-store of a
   borrow-mode container is an error), automatic borrow→owning materialize (heap
   root promotes without byte copy; stack root copies; owned source into owning
   plain-form errors, `#` moves), `clone()` producing an independent duplicate
   (reference type yields a distinct object; value type COW),
   owning-required-parameter declaration (`Cache<#K,V>`; plain `Cache<String,V>`
   rejected) and inheritance contagion (`extends Cache<K,V>` laundering the `#`
   is rejected; satisfy/reproject accepted; dual-mode base subclassing
   unaffected), and the value-semantics and grammar edge cases of §8.

### 9.2 Use cases
1. As a **maintainer landing this**, when I run the three legacy probes plus the
   new agreement/confinement/transition tests, then green across all of them is
   the acceptance signal — the first time the leak and the dangle are closed by
   the same change.
