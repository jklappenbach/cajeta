# Slices & the `shared` state — Specification

> Status: APPROVED (2026-07-02, consolidated; developer-approved same day). This spec defines a
> language primitive, `Slice<T>`, and a memory-model addition, the `shared` ownership state. Together
> they deliver zero-copy `String.substring`, array slicing and dataframe column cells, and they
> unblock the long-deferred `Object.clone()`. It is a layer-0 foundation. It sits below `record`,
> whose §2.6.5 text/array fields consume it, and beside the `cajeta.math` substrate, where
> `Tensor`/`Storage` unification is deliberately deferred (§7.4).
>
> This is a spec, so it states requirements and use cases, the why and the what. Remaining build decisions are
> `> **TBD (plan-time):**` markers collected in §11. It is outline-numbered for addressability.
>
> This spec amends or must reconcile with these companion design docs: `MemoryModel.md`,
> `BorrowSoundness.md`, `FieldOwnership.md`, `FieldBorrowEscape.md`, `OwnershipTransfer.md`,
> `Object.md`, `String.md`, `Views.md`. Its consumers are `../nucleo/records-spec.md` (§2.6.5),
> `../nucleo/nucleo-column-spec.md`, `../nucleo/nucleo-frame-spec.md`.
>
> Provenance: the initial draft was red-teamed on 2026-07-02 in a code-grounded adversarial pass, and this revision
> consolidates the surviving design. §12 records the resolved findings for traceability.
>
> **Amended by the field-store rule** (`specs/field-store-ownership-spec.md`). A plain `=`
> field or slot store never moves, so the "move when it is the last use" of §4.2 and the
> auto-move of §9 do not apply to it. An owned String or slice local kept in a field is
> spelled `#=`, which moves the window and its shared stake. A lent window stored with `#=`
> resolves to a copy up to 256 bytes and to a shared stake above that.

## 0. The model — cajeta's implicit smart-pointer family

Cajeta's memory model is an implicit smart-pointer system. It carries the safety disciplines of the
C++/Rust pointer family, and the compiler infers the kind. The developer never annotates it. There is
no `Rc<T>` in user code and no lifetime syntax, and `#` remains the only ownership sigil.

| State | Discipline | Family analog |
|---|---|---|
| **owned** | single responsible dropper; `#` transfers the stake | `unique_ptr` / `Box` |
| **borrow** | non-owning; **must not outlive its source** (static check) | `&T` |
| **shared** *(new)* | co-owned; runtime refcount; freed at the **last** drop | `shared_ptr` / `Rc` |
| *weak (future)* | *non-owning observer of a shared; breaks cycles* | `weak_ptr` / `Weak` |

The v1 scope of `shared` is the acyclic subset. `shared` applies only to immutable leaf
buffers, the byte/element stores behind slices. Such buffers hold no outgoing references,
so the shared graph is a DAG by construction. Cycles are impossible, `weak` is unnecessary, and
the refcount cannot leak. General shared objects, which are mutable, identity-bearing and cyclic, are a
deliberately deferred door (§4.2). Opening it later rides the same rc substrate plus `weak`.

The addition has a price. A class of lifetime management moves from compile time, the static borrow
check, to run time, a refcount. That cost is accepted as small and well-understood, and it is
confined to the acyclic subset.

## 1. Definition

### 1.1 Purpose
A slice is a value-type view into a contiguous run of elements owned by a backing buffer,
`{ backing, offset, len }`. Constructing a slice, whether a substring, a sub-array or a column cell, is
O(1), with no heap allocation and no element copy. It is also sound. A slice never dangles, even when
its backing was dynamically produced by a string concat, an IO read or a computed array, and even when
the slice outlives the value it was sliced from because it was stored in a `record` field, pushed into a
container or returned up the stack.

The load-bearing operation is `String.substring`, which today allocates and copies, because
view-mode slicing was deferred in `String.cajeta`. Generalizing the fix yields one primitive
that also gives Cajeta zero-copy array slices and backs dataframe column cells. That is one
memory story across string, array and column, with `Tensor` unification as a later option (§7.4).

### 1.2 Scope
- The value-type `Slice<T>` view, with O(1) sub-slicing (§2).
- The `shared` ownership state, in which buffers are co-owned by many slices, refcounted and freed
  once at the last drop, plus its promotion trigger and cost model (§3–§4).
- The escape-resolution rule, which says what the compiler does when a slice/borrow would outlive
  its source, whether copy, share or reject (§4).
- The immutable/mutable split that decides value-copy safety and `record`-field eligibility (§5).
- Copy semantics for values with heap-referencing fields, the synthesized shallow-copy-and-share
  hook, `#`-move, and `Object.clone()`, which this spec unblocks (§6).
- The `Utf8` string specialization and `String` re-expressed over it, making `substring`
  allocation-free (§8).
- The reductions: `String.substring`, `arr[a:b]`, column cells (§7).

### 1.3 Non-goals
- **Rust-style lifetime-bounded slices.** A static lifetime bound rejects the primary use case,
  which is storing a slice in a longer-lived record or column. This spec makes that case sound.
- **Garbage collection or universal refcounting.** There is no tracing and there are no pauses. The
  refcount is selective, since only buffers an escaping slice actually shares are counted. Every drop
  site is statically emitted, and which site frees is decided dynamically by the count reaching zero,
  deterministically at that drop.
- **General shared-mutable objects and `weak`.** This is the deferred door (§0, §4.2). v1 shares
  immutable leaf buffers only.
- **Growable / mutable-length views.** A slice's `len` is fixed at creation.
- **Structured binary overlays.** The `view` declaration in `Views.md` is a different feature. It
  may later layer on `Slice<int8>`, but it is not redefined here.
- **`Tensor`/`Storage` unification.** This was evaluated and deferred ([D3], §7.4).

### 1.4 Relationship to existing constructs
- **`cajeta.lang.String`** already carries `mode` (0 = owned, 1 = view), but view-mode only borrows
  static storage, which is precisely why `substring` must copy today. `String` is re-expressed as the
  rich facade over a `Utf8` core (§8), and `mode` collapses into the `Utf8` tag.
- **The live-set** (`MemoryManager`, `cajeta_rt_core.c`) is a double-free guard, in which the
  first dropper claims and frees and later drops no-op. It is not a last-drop protocol, so it
  cannot be the sharing mechanism. Reusing it use-after-frees when the source drops before a
  stored view, which is the PageCache-eviction UAF class. The `shared` state's `rc == 0` free path
  routes through the live-set claim, preserving the double-free guarantee (§3.5).
- **Existing escape machinery is a rejecter.** `arenaWalk` computes object escape to disqualify
  arena allocation, and `FieldBorrowEscape`, which is unimplemented and warn-biased, is designed
  to reject borrow stores. Neither soundly computes where a slice escapes so that it can be
  permitted. §4.3 specifies the new analysis and its fail-safe.
- **`Object.clone()`** is declared but is a null placeholder "until ownership semantics stabilize"
  (`Object.md` §clone). This spec is that stabilization. §6.4 defines `clone()` and lights up
  `__cajeta_object_clone`.
- **`cajeta.math.Storage`** already shares one buffer across many `Tensor` views, freed once via
  the live-set. It stays as it is in v1 (§7.4), and `Slice<T>` is a parallel primitive.

## 2. The `Slice<T>` primitive

A slice is a flat value, a reference to a backing store plus a `[offset, offset+len)` window.
```
Slice<T> = { backing store; int64 offset; int64 len }
```

**Use cases**
- **2.1** As a developer, when I take a sub-slice (`s.slice(a, b)`), then I get
  `{ s.store, s.offset + a, b - a }` in O(1), with no allocation and no element copy, reading the
  same underlying elements. Sub-slicing a shared slice attributes co-ownership to the root
  buffer and never to an intermediate slice.
- **2.2** As a developer, when I index a slice (`s[i]`), then I read `store[offset + i]`, and an index
  outside `[0, len)` is a bounds violation, the same contract as array indexing.
- **2.3** As a developer, when I ask a slice its length, then it is the window length, independent
  of the backing buffer's capacity.
- **2.4** As a compiler author, when a value produces a contiguous run (an array, a string buffer,
  a `.rodata` literal), then a slice over it needs no per-slice heap object. A slice is three
  machine words, or the 16-byte `Utf8` of §8, passed and copied by value.

## 3. The `shared` state

### 3.1 Definition
A backing buffer is in exactly one ownership state:
- **owned**, with one responsible dropper, which is today's model unchanged; or
- **shared**, co-owned by its remaining owner(s) and every escaped slice of it. A runtime
  count tracks the stakes, and the buffer is freed exactly once, when the last stake drops; or
- **static**, a `.rodata` literal, which is immortal, never counted and never freed.

Promotion is one-way, from `owned` to `shared`, and is performed by the compiler at an escape-resolution
site (§4). There is no demotion.

### 3.2 What counts toward the refcount
Only owning stakes count. Those are the original owner, each escaped slice, and each copy of an
escaped slice. Local borrows never count. A slice the compiler proves local remains a plain
borrow with zero rc traffic, and its soundness is guaranteed by the existing borrow check against its source.
The operations are:
- **copy** of a shared stake → `rc++`, through the synthesized copy hook (§6.1);
- **`#`-move** of a stake → rc-neutral, since the stake transfers with no bump (§6.2, §9);
- **drop** of a stake → `rc--`, and at zero, free.

### 3.3 Placement — the count-word tag bit + side table *(recommended)*
The `rc` must not change existing data layouts. `CajetaArray` is `{ i64 count; data… }`, and that
shape is load-bearing. String's SSO region mirrors it byte-for-byte, every runtime intrinsic
computes `data = base + 8`, and the `Storage`/Arrow/GPU paths assume `data == base + header`. Prepending
an `rc` word breaks all of it. Instead:
- Steal one bit of the existing `count` word as the shared flag, since lengths never need 63 bits.
  Owner-drop's fast path checks a bit in a word it already loads.
- Keep the actual count in a side table keyed by buffer base, touched only by
  promote/copy/drop of actually-shared buffers.

There is zero layout change, and the `rc == 0`/unshared fast path costs one predictable bit-test.

> **TBD (plan-time):** the side-table structure, for which the live-set's open-addressed hash is the model,
> and whether hot shared buffers earn an inline header variant later. **[D-rc]**

### 3.4 Atomicity
On the single-carrier path the count is a plain load-add-store, gated by the existing
`__cajeta_live_set_mt` flag exactly as the live-set already gates its own locking. Sending a
shared slice across a spawn/carrier boundary flips the process to atomic (acq-rel) counting,
which is the same escalation discipline the runtime already uses. Cross-carrier drops therefore remain
correct without taxing single-carrier code.

### 3.5 Free path
When the count reaches zero, the free routes through the live-set claim, claim-then-free, so
a racing auto-field-drop still no-ops. The existing double-free guarantee is preserved on top of
the new last-drop protocol.

### 3.6 Owner-drop semantics
Every drop of a sliceable buffer's owner gains one branch. When the shared bit is clear, it frees
exactly as today, which is the overwhelmingly common, predicted path. When the bit is set, it
decrements via the side table and frees at zero. Container element-drops run the same synthesized drop hook (§6.1) so stakes held inside
collections decrement correctly.

**Use cases**
- **3.7** As a developer, when I create and drop strings/arrays that are never sliced-and-stored,
  then they carry no rc traffic whatsoever, only one predicted bit-test at drop, and behavior and
  performance are unchanged from today.
- **3.8** As a developer, when I `substring` an IO-read `String`, store the result in a
  longer-lived record, and let the original drop, then the bytes remain valid and the buffer is
  freed exactly once, when the stored slice, the last stake, drops.
- **3.9** As a developer, when I slice a **literal**, then no counting ever occurs, because the state is
  static with an infinite lifetime, and the slice is pure arithmetic.

## 4. Escape resolution — when the compiler copies, shares, or rejects

### 4.1 The layered trigger
1. **Provably local.** The slice/borrow demonstrably cannot outlive its source, because it is used
   and dropped in scope or passed to a callee that provably does not retain it. It stays a plain
   borrow, with zero rc and zero copies. This is the common case and the fast path.
2. **Escapes, or cannot be proven local.** The compiler applies the resolution table (§4.2).

This replaces the borrow checker's unconditional rejection of "borrow may outlive its source"
with a resolution, for the eligible types below only. Everything else still rejects
exactly as today.

### 4.2 The resolution table
| Source of the escaping reference | Resolution | Rationale |
|---|---|---|
| **small value** (stack local; slice ≤ copy-threshold; Inline `Utf8`) | **copy** — or **move** when it is the last use | copying is cheaper than counting; stays two-state |
| **large immutable heap buffer** (long substring, big array slice) | **share** — promote the root buffer `owned → shared`, escapee becomes a stake | copying is the expense being avoided; sharing is transparent because the bytes are immutable |
| **arena-backed buffer** (frame-arena concat results — no individual free exists) | **copy, always** (any size) | arena memory is bulk-reset; it cannot be promoted |
| **mutable or identity-bearing object** | **compile error**, exactly as today | aliasing/identity sharing is the deferred general door |

The copy↔share boundary is a size threshold. It is the `Utf8` Inline cap (§8) for text and a
plan-time byte cap for `Slice<T>`, and it is the dial between "more copying" and "more counting."

> **TBD (plan-time):** the `Slice<T>` threshold value (lean: low hundreds of bytes). **[D-thresh]**

### 4.3 The soundness discipline
The escape analysis must be a sound over-approximation, and the fail-safe direction is fixed
by an asymmetry:
- **over-approximate**, resolving a slice that was actually local, costs a wasted copy or count
  pair, which is cheap and bounded;
- **under-approximate**, missing an escape, leaves a dangling pointer, which is fatal.

Therefore, where the analysis cannot decide, it resolves (unsure ⇒ copy/share per the table),
and it never silently borrows. The existing passes do not qualify, since they are rejecters (§1.4),
so a new slice-escape pass is a headline plan deliverable. It may start conservative, resolving more
than strictly necessary, and gain precision release over release without ever being unsound. The
false-positive rate is a measurable performance knob and never a correctness risk.

### 4.4 Retention amplification — the Java-substring lesson
A shared slice keeps its entire root buffer alive. A 16-byte slice of a 100 MB IO read, once
stored, retains 100 MB, which is the leak that drove Java to make `substring` copy in JDK 7u6. The v1 policy is:
- the threshold already copies small escapees, which are the dominant case of keys, symbols and tokens;
- `.clone()` (§6.4) is the explicit detach, which copies the window and drops the stake, for the
  developer who knows the buffer is huge and the slice long-lived;
- a ratio guard, which copies when `len ≪ buffer.count`, e.g. < 1/64th, even above the threshold, is
  specified as a plan-time knob that defaults to off. Ship simple, measure, then decide.

> **TBD (plan-time):** the ratio-guard default and diagnostics. A lint that flags a small-slice-of-
> huge-buffer share is cheap and useful. **[D-amp]**

**Use cases**
- **4.5** As a developer, when I use a substring only within its scope (`if s.substring(0,4) ==
  "http"`), then no copy, no count and no allocation occurs, because it is a borrow.
- **4.6** As a developer, when I store a 6-character venue code out of a parsed line into a
  record, then it is copied inline (≤ threshold ⇒ Inline `Utf8`), with no rc, and the parsed
  line's buffer is freed on schedule.
- **4.7** As a developer, when I store a 40 KB body slice of a 60 KB response into a cache that
  outlives the response, then the response buffer is promoted to shared and lives until the
  cache entry drops, with no 40 KB copy.
- **4.8** As a developer, when I try to store an escaping borrow of a mutable object, then the
  compiler rejects it exactly as today, and I `#`-transfer or `clone()` it (§9).

## 5. Mutability — the value-copy line

- **5.1** Immutable slices (`Utf8`/`String`, read-only column cells, immutable array slices)
  share the backing when copied, which is observationally identical to a value copy because the
  elements never change. Immutable slices are value-copy-clean and are the only slices eligible as
  `record` fields. That resolves `records-spec.md` §2.6.5, where the rule becomes "no
  owning/mutable heap identity" in place of "no pointers."
- **5.2** Mutable slices (a writable view for in-place algorithms, a write-through column
  view) make writes visible through every alias, and copying one aliases shared mutable state, so
  they are not value-copy-clean. In v1 a mutable slice is borrow-only. It cannot escape its source,
  which is a compile error, cannot be shared, and cannot be a record field. The shared-mutable door stays closed.
- **5.3** As a núcleo author, when a column hands out cell views, then the column's data region is
  frozen, meaning append-sealed, first, so §5.1's immutable-share contract holds. In-place column
  mutation requires the buffer to be unshared, with the shared bit clear.

## 6. Copy, move, equality, `clone()`

### 6.1 The synthesized value-copy (shallow copy + share)
A value-copy of an object with heap-referencing fields is a shallow copy, in which the field slots
are copied, plus one `rc++` per heap-referencing field. That promotes an `owned` field's buffer to
`shared` at that moment if it was not already. Its properties are:
- **Bounded by the static inline layout.** The walk recurses over nested value sub-aggregates,
  whose depth is finite and known at compile time because a value type cannot inline-contain itself,
  and it stops at every heap edge, where it shares and never traverses. It is O(fields) and
  cycle-proof, and it cannot deep-copy a runtime graph. This boundedness is a guarantee of the value-type size rule.
- **Transparent iff the heap fields are immutable**, since then share ≡ copy. A mutable heap field
  forces a deep copy or the deferred general door, which is exactly why record fields are
  immutable-only (§5.1).
- A value type is trivially copyable, by pure memcpy, iff all its fields are trivial. The memcpy is
  the optimization, and the synthesized walk is the definition. A record holding a Shared-capable slice is a
  non-trivial value type carrying this synthesized copy and the matching synthesized drop,
  `rc--` per shared field, which is the amendment `records-spec.md` §2.6.5 must adopt. Records'
  synthesized `with(...)` runs the same hook.

### 6.2 Move (`#`)
A `#`-move transfers the stake. It is a memcpy plus deactivation of the source's drop, and it is
rc-neutral, on a codegen path distinct from copy. Value-move today is a raw memcpy, and it gains this
hook only for non-trivial values. See §9 for `#`'s role in the model.

### 6.3 Equality and hashing are representation-independent
Two immutable slices are equal iff their window bytes are equal, regardless of
representation, so an Inline `"EURUSD"` equals a Shared `"EURUSD"`. Hashing hashes the window
bytes. The `Utf8` prefix (§8) is a comparison accelerator and never the definition.

### 6.4 `Object.clone()` — unblocked
`clone()` is the explicit sibling of the implicit value-copy. Value types copy implicitly on
assignment, and reference classes, whose `=` borrows, copy via explicit `clone()`. Both run the
§6.1 mechanic, a shallow copy plus `rc++`/promote per heap field. cajeta `clone()` is Java-shallow
clone plus rc. Java's shallow clone leans on GC to keep aliased referents alive, and cajeta has no
GC, so an aliased heap field after a shallow clone must be `shared`. Override `clone()` to
deep-copy a mutable field, which is Java's existing escape hatch. On a slice, `clone()` detaches.
It copies the window into a fresh owned buffer and drops the stake, which is §4.4's amplification valve.
The deliverable is that `__cajeta_object_clone` stops returning null.

## 7. The reductions

- **7.1 `String.substring(a, b)`** returns a `Utf8` (§8) over the same bytes in O(1), with no
  allocation and no element copy, with the SSO carve-out of §8.3. Escape resolution then applies per §4.
  Local use borrows, a stored short result is Inline, and a stored long result shares. The public signature
  keeps returning `String`, the facade, so call sites are source-compatible.
- **7.2 Array slice (`arr[a:b]`)** yields `Slice<T>` with the same contract. It is threshold-copied or
  shared on escape, and an arena-backed slice always copies.
- **7.3 Column cells.** Reading a cell of a string/array column yields a slice into the column's
  frozen data buffer, which is a borrow by default. The column's lifetime dominates a scan, so there
  is no rc traffic in filter/join/groupby loops. A cell that escapes the table goes through §4.2, and
  is typically Inline-copied for symbols or, for a long cell, a stake on the column buffer. Column
  ingest copies bytes into the contiguous Arrow data buffer. That is the one sanctioned, bulk,
  unavoidable copy, because Arrow requires contiguity, and everything downstream of ingest is zero-copy.
- **7.4 `Tensor`/`Storage`, deferred ([D3]).** Unification was evaluated and rejected for v1 on
  two grounds. First, the autograd tape stores views that outlive the forward scope as the norm,
  so tape-resident tensors would all promote, putting count traffic on the ML
  hot path. Second, `Storage`'s buffer layout is load-bearing for Arrow bit-identity and GPU upload
  base math. `Storage` keeps its current live-set discipline. Revisit only if the side-table
  design (§3.3, no layout change) plus a tape-aware policy solves both.

## 8. `Utf8` — the string specialization

### 8.1 Representation — a 16-byte tagged union
```
Utf8 (16 bytes, by tag):
  Inline : { u32 lenTag; i8[12] bytes }              // len <= 12: the bytes ARE the value
  Static : { u32 lenTag; i8[4] prefix; i64 ptr }     // .rodata literal window; immortal
  Shared : { u32 lenTag; u32 offset; i64 basePtr }   // window into a shared buffer
```
- The tag lives in reserved bits of the first word, `lenTag`, making all three forms
  self-describing. `basePtr` is the allocation base, so the shared-bit/side-table lookup (§3.3)
  is direct, and chained substrings always attribute to the root.
- **Normalization rule.** Any operation whose text result is ≤ 12 bytes produces an Inline,
  including slices of Static and Shared sources. Pointer forms therefore always carry `len > 12`,
  and the tag assignment is total and unambiguous.
- The Static form keeps a 4-byte prefix as a comparison accelerator. The Shared form spends
  those bytes on `offset` instead, since it holds long strings and equality falls back to the window bytes (§6.3).

### 8.2 `String` — the facade
`String` becomes the rich API (`indexOf`, `split`, codepoint ops, `charAt`, `cachedCpLength`) over
a `Utf8` core. The legacy `mode 0/1` collapses into the tag, `__cajeta_string_drop` becomes
tag-dispatch, and `substring` returns a slice instead of allocating. There is a migration risk to weigh at plan time.
String's field layout is pinned by JIT tests, literal codegen materializes view-mode Strings, the
SSO region mirrors the array header, and multiple intrinsics poke String fields directly. The
re-core is therefore a real migration with a regression surface, and the acceptance bar is the full string test
suite unchanged.

### 8.3 SSO interaction
- Slicing a string whose bytes live in the SSO region has two outcomes. A result ≤ 12 B is Inline,
  with the bytes copied into the slice, which is safe under moves by construction. A result of 13–24 B
  causes a one-time spill of the bytes to a heap buffer, which the slice then shares. The spill is a small
  allocation, and it is an explicit, documented carve-out from §10's no-allocation criterion, for 13–24 B
  substrings of SSO-resident strings only.
- **Asserted invariant.** The compiler never produces a Shared/Static `Utf8` pointing into a
  wrapper's `ssoData`, and always produces Inline-or-spill, because a pointer into an SSO region dangles when the
  wrapper value moves.

**Use cases**
- **8.4** As a developer, when I substring a venue code / currency / category (≤ 12 B), then I get
  an Inline `Utf8`, a 16-byte struct copy with no buffer and no counting, and a record/column full of
  them is trivially memcpy-able.
- **8.5** As a developer, when I substring a literal, then it is Static, or Inline if ≤ 12 B, and
  it is free forever.
- **8.6** As a developer, when I keep a long slice of a dynamic/IO string beyond its source, then
  it is Shared per §3–§4, alive exactly as long as needed and freed once.

## 9. The role of `#` *(decided 2026-07-02)*

The `shared` state generalizes `#`'s meaning without changing its shape. `#` transfers an owning
stake, which was formerly always the unique ownership and is now possibly one stake among co-owners, and a
move is always rc-neutral (§6.2).
- **`#` is the zero-cost escape path.** At an escape site the compiler's resolution costs a copy
  or a count (§4.2). `list.add(#s)` costs neither, because the stake transfers. The implicit
  machinery makes naive code sound, and `#` remains how deliberate code makes it free.
- **Signatures generalize cleanly.** `#T f()` is a forced transfer. The caller receives a
  stake, unique or shared, and must receive it with `#=`
  (`stdlib-ownership-convention-spec` §4.6). `T f()` is a transparent carry. The caller
  receives whatever ownership state the callee's FRAME held, decided at run time by the
  return flag, so it is a borrow exactly where the body returns nothing but interior reads
  (§2.2/§2.8). `#T` formals still mean "callee takes the stake."
- **The explicit trichotomy.** Borrow is the default (`=`), `#` is the explicit move, and
  `clone()` is the explicit copy/detach. Share has no spelling. It is exclusively a
  compiler-inserted resolution, preserving the no-annotation identity (§0).
- **The discipline boundary, RESOLVED (2026-07-02) as transparent-only resolution.** Silent
  resolution (§4) applies only where value semantics make it observationally transparent.
  Those are small values, where a copy is indistinguishable, and immutable buffers, where a share is
  indistinguishable. Identity-bearing / mutable objects keep today's error-and-`#`
  discipline, because there a silent resolution would change the meaning between aliasing and snapshot, so
  explicitness stays mandatory. The compiler decides only when the decision is invisible. The
  moment it is visible, it is the developer's.
- **Auto-move on last use, RESOLVED as allowed.** When the source is provably dead after the
  escape, the compiler upgrades the resolution from copy to move, inferring `#`. This is unobservable,
  and it is a pure optimization with no semantic change.
- **Visibility lint (plan deliverable).** A diagnostic tier flags each silent resolution
  ("escape resolved to a share; `#` would make it free"), keeping the cost surface honest for
  those who want the old strictness. It is informational and never an error.

## 10. Acceptance criteria (spec-level)
- **Construction** of a slice (`substring`, `arr[a:b]`, sub-slice, column cell) performs no heap
  allocation and no element copy, with the sole documented carve-out of §8.3's 13–24 B
  SSO spill. Escape resolution may copy small slices by design, and that is the policy working.
- A slice of a dynamic/IO backing stored in a value that outlives its source remains valid
  until the last stake drops. A literal slice is immortal. Nothing dangles, and every buffer is freed
  exactly once, through the live-set claim.
- Buffers whose slices never escape incur zero rc traffic beyond one predicted bit-test at owner
  drop. Local slices are plain borrows, and column scans count nothing.
- Immutable slices are value-copy-clean and `record`-eligible. Mutable slices are borrow-only,
  and an escape is a compile error. `records-spec` §2.6.5 is amended to "value semantics, synthesized
  copy/drop — memcpy iff all fields trivial."
- `String` is re-cored over `Utf8` with the full existing string suite green, `substring` is O(1),
  and equality/hash are representation-independent.
- `Object.clone()` is implemented per §6.4, with `__cajeta_object_clone` non-null, and slice `clone()`
  detaches.
- `Tensor`/`Storage` behavior is unchanged, with unification deferred.

## 11. Open questions (resolve at plan time)
- **[D-rc]** The side-table structure and count-word bit choice (§3.3). Revisit an inline header only if
  side-table drop cost proves hot.
- **[D-thresh]** The `Slice<T>` copy↔share byte threshold. The lean is low hundreds of bytes, and `Utf8`'s
  is the 12-byte Inline cap.
- **[D-amp]** The ratio-guard default, with a lean to off, and the small-slice-of-huge-buffer lint (§4.4).
- **[D-#] RESOLVED (§9).** Transparent-only resolution covers small values and immutable buffers.
  Identity/mutable objects keep error-and-`#`, auto-move on last use is allowed, and the visibility lint is
  a plan deliverable.
- **[D3]** `Tensor`/`Storage` unification is deferred, and the revisit condition is in §7.4.
- **Naming.** `Slice<T>` or `Span<T>`, `Utf8` or `Str`, and whether `Utf8` is a public type or
  `String`'s exposed core.
- **Mutable-slice surface**, which is borrow-only in v1. A distinct type such as `MutSlice<T>` or a
  qualifier, a question shared with the tensor-view and write-through-column stories.
- **Future gates.** A pointer-form `Utf8` in GPU kernel-visible types needs a gate when records become
  kernel args, because host pointers are meaningless on device. `weak` and the general shared-object
  door (§0) remain.

## 12. Design history — resolved red-team findings (2026-07-02)
A code-grounded adversarial pass against the initial draft produced eight findings, all resolved
in this revision: **S1** POD-vs-rc contradiction → §6.1 (synthesized copy/drop; memcpy iff
trivial); **S2** reused escape pass unsound → §4.1/§4.3 (new sound over-approximating pass;
unsure ⇒ resolve); **S3** `#`-move leak/double-free → §6.2 (move rc-neutral, distinct path);
**S4** 16-byte over-subscription → §8.1 (tagged union; Shared spends prefix bytes on offset);
**S5** rc atomicity cost + per-column hot cacheline → §3.4 (mt-gated non-atomic) + §7.3 (cells
are borrows); **S6** SSO 13–24 B spill vs no-alloc claim → §8.3/§10 (explicit carve-out);
**S7** `Storage` header + autograd-tape pinning → §7.4 (defer [D3]); **S8** Shared-into-SSO →
§8.3 (asserted invariant). Additional consolidation findings: retention amplification → §4.4;
array-header layout invariant → §3.3; mutable-slice escape story → §5.2; owner-drop semantics →
§3.6; atomic-from-day-one contradiction → §3.4.
