# Slices & the `shared` state — Specification

> Status: **APPROVED** (2026-07-02, consolidated; developer-approved same day). A **language primitive** (`Slice<T>`)
> and a **memory-model addition** (the `shared` ownership state) that together deliver zero-copy
> `String.substring`, array slicing, and dataframe column cells — and unblock the long-deferred
> `Object.clone()`. Layer-0 foundation — **below** `record` (whose §2.6.5 text/array fields
> consume it) and beside the `cajeta.math` substrate (`Tensor`/`Storage` unification deliberately
> **deferred**, §7.4).
>
> This is a **spec** — requirements and use cases (the *why/what*). Remaining build decisions are
> `> **TBD (plan-time):**` markers collected in §11. Outline-numbered for addressability.
>
> Companion design docs (this spec amends or must reconcile with them): `MemoryModel.md`,
> `BorrowSoundness.md`, `FieldOwnership.md`, `FieldBorrowEscape.md`, `OwnershipTransfer.md`,
> `Object.md`, `String.md`, `Views.md`. Consumers: `../nucleo/records-spec.md` (§2.6.5),
> `../nucleo/nucleo-column-spec.md`, `../nucleo/nucleo-frame-spec.md`.
>
> Provenance: initial draft red-teamed 2026-07-02 (code-grounded adversarial pass); this revision
> consolidates the surviving design. §12 records the resolved findings for traceability.

## 0. The model — cajeta's implicit smart-pointer family

Cajeta's memory model is an **implicit smart-pointer system**: the safety disciplines of the
C++/Rust pointer family, with the **kind inferred by the compiler** — never annotated by the
developer (no `Rc<T>` in user code, no lifetime syntax; `#` remains the only ownership sigil).

| State | Discipline | Family analog |
|---|---|---|
| **owned** | single responsible dropper; `#` transfers the stake | `unique_ptr` / `Box` |
| **borrow** | non-owning; **must not outlive its source** (static check) | `&T` |
| **shared** *(new)* | co-owned; runtime refcount; freed at the **last** drop | `shared_ptr` / `Rc` |
| *weak (future)* | *non-owning observer of a shared; breaks cycles* | `weak_ptr` / `Weak` |

**v1 scope of `shared` — the acyclic subset.** `shared` applies **only to immutable leaf
buffers** (the byte/element stores behind slices). Such buffers hold **no outgoing references**,
so the shared graph is a **DAG by construction: cycles are impossible, `weak` is unnecessary, and
the refcount cannot leak.** General shared-*objects* (mutable, identity-bearing, cyclic) are a
deliberately deferred door (§4.2); opening it later rides the same rc substrate plus `weak`.

The addition is honest about its price: a class of lifetime management moves from compile-time
(the static borrow check) to runtime (a refcount) — accepted as small and well-understood, and
confined to the acyclic subset.

## 1. Definition

### 1.1 Purpose
A **slice** is a value-type *view* into a contiguous run of elements owned by a backing buffer:
`{ backing, offset, len }`. Constructing a slice (a substring, a sub-array, a column cell) is
**O(1) — no heap allocation, no element copy** — yet **sound**: a slice never dangles, even when
its backing was dynamically produced (string concat, IO read, a computed array), and **even when
the slice outlives the value it was sliced from** (stored in a `record` field, pushed into a
container, returned up the stack).

The load-bearing operation is **`String.substring`**, which today *allocates and copies*
(view-mode slicing was deferred in `String.cajeta`). Generalizing the fix yields one primitive
that also gives Cajeta zero-copy **array slices** and backs **dataframe column cells** — one
memory story across string, array, and column, with `Tensor` unification as a later option (§7.4).

### 1.2 Scope
- The value-type **`Slice<T>`** view, with O(1) sub-slicing (§2).
- The **`shared` ownership state** — buffers co-owned by many slices, refcounted, freed once at
  the last drop — plus its promotion trigger and cost model (§3–§4).
- The **escape-resolution rule**: what the compiler does when a slice/borrow would outlive its
  source — copy, share, or reject (§4).
- The **immutable/mutable** split that decides value-copy safety and `record`-field eligibility (§5).
- **Copy semantics** for values with heap-referencing fields (the synthesized shallow-copy-and-share
  hook), `#`-move, and **`Object.clone()`** — which this spec unblocks (§6).
- The **`Utf8`** string specialization and `String` re-expressed over it, making `substring`
  allocation-free (§8).
- The reductions: `String.substring`, `arr[a:b]`, column cells (§7).

### 1.3 Non-goals
- **Rust-style lifetime-bounded slices.** A static lifetime bound *rejects* the primary use case
  (store a slice in a longer-lived record/column); this spec makes that case **sound**, not
  forbidden.
- **Garbage collection or universal refcounting.** No tracing, no pauses. The refcount is
  **selective** (only buffers an escaping slice actually shares); every drop site is statically
  emitted — which site frees is decided dynamically by the count reaching zero, deterministically
  at that drop.
- **General shared-mutable objects and `weak`** — the deferred door (§0, §4.2). v1 shares
  immutable leaf buffers only.
- **Growable / mutable-length views.** A slice's `len` is fixed at creation.
- **Structured binary overlays.** The `view` declaration (`Views.md`) is a different feature; it
  may later layer on `Slice<int8>` but is not redefined here.
- **`Tensor`/`Storage` unification** — evaluated and **deferred** ([D3], §7.4).

### 1.4 Relationship to existing constructs
- **`cajeta.lang.String`** already carries `mode` (0 = owned, 1 = view), but view-mode only borrows
  *static* storage — precisely why `substring` must copy today. `String` is re-expressed as the
  rich facade over a `Utf8` core (§8); `mode` collapses into the `Utf8` tag.
- **The live-set** (`MemoryManager`, `cajeta_rt_core.c`) is a **double-free guard**
  (first-dropper-claims-and-frees; later drops no-op) — *not* a last-drop protocol, so it
  **cannot** be the sharing mechanism (reusing it use-after-frees when the source drops before a
  stored view — the PageCache-eviction UAF class). The `shared` state's `rc == 0` free path
  **routes through the live-set claim**, preserving the double-free guarantee (§3.5).
- **Existing escape machinery is a *rejecter*, not a resolver.** `arenaWalk` computes object
  escape to *disqualify* arena allocation; `FieldBorrowEscape` (unimplemented, warn-biased) is
  designed to *reject* borrow stores. Neither soundly computes where a **slice** escapes so it can
  be *permitted*; §4.3 specifies the new analysis and its fail-safe.
- **`Object.clone()`** is declared but a null placeholder *"until ownership semantics stabilize"*
  (`Object.md` §clone). This spec is that stabilization; §6.4 defines `clone()` and lights up
  `__cajeta_object_clone`.
- **`cajeta.math.Storage`** already shares one buffer across many `Tensor` views (freed once via
  the live-set). It stays **as-is** in v1 (§7.4); `Slice<T>` is a parallel primitive.

## 2. The `Slice<T>` primitive

A slice is a flat value: a reference to a backing store plus a `[offset, offset+len)` window.
```
Slice<T> = { backing store; int64 offset; int64 len }
```

**Use cases**
- **2.1** As a developer, when I take a sub-slice (`s.slice(a, b)`), then I get
  `{ s.store, s.offset + a, b - a }` in **O(1)** — no allocation, no element copy — reading the
  same underlying elements. Sub-slicing a shared slice attributes co-ownership to the **root**
  buffer (never to an intermediate slice).
- **2.2** As a developer, when I index a slice (`s[i]`), then I read `store[offset + i]`; an index
  outside `[0, len)` is a bounds violation (same contract as array indexing).
- **2.3** As a developer, when I ask a slice its length, then it is the window length, independent
  of the backing buffer's capacity.
- **2.4** As a compiler author, when a value produces a contiguous run (an array, a string buffer,
  a `.rodata` literal), then a slice over it needs **no per-slice heap object** — a slice is three
  machine words (or the 16-byte `Utf8`, §8), passed and copied by value.

## 3. The `shared` state

### 3.1 Definition
A backing buffer is in exactly one ownership state:
- **owned** — one responsible dropper (today's model, unchanged); or
- **shared** — co-owned by its remaining owner(s) and every **escaped** slice of it; a runtime
  count tracks the stakes; the buffer is freed exactly once, when the last stake drops; or
- **static** — a `.rodata` literal: immortal, never counted, never freed.

Promotion is **one-way**: `owned → shared`, performed by the compiler at an escape-resolution
site (§4). There is no demotion.

### 3.2 What counts toward the refcount
**Owning stakes only**: the original owner, plus each **escaped** slice, plus each **copy** of an
escaped slice. **Local borrows never count** — a slice the compiler proves local remains a plain
borrow (zero rc traffic), its soundness guaranteed by the existing borrow check against its source.
Operations:
- **copy** of a shared stake → `rc++` (the synthesized copy hook, §6.1);
- **`#`-move** of a stake → **rc-neutral** (the stake transfers; no bump — §6.2, §9);
- **drop** of a stake → `rc--`; at zero, free.

### 3.3 Placement — the count-word tag bit + side table *(recommended)*
The `rc` must not change existing data layouts: `CajetaArray` is `{ i64 count; data… }` and that
shape is load-bearing (String's SSO region mirrors it byte-for-byte; every runtime intrinsic
computes `data = base + 8`; `Storage`/Arrow/GPU paths assume `data == base + header`). Prepending
an `rc` word breaks all of it. Instead:
- **Steal one bit of the existing `count` word** (lengths never need 63 bits) as the
  **shared flag**. Owner-drop's fast path checks a bit in a word it already loads.
- Keep the actual count in a **side table keyed by buffer base**, touched **only** by
  promote/copy/drop of *actually-shared* buffers.

Zero layout change; the `rc == 0`/unshared fast path costs one predictable bit-test.

> **TBD (plan-time):** side-table structure (the live-set's open-addressed hash is the model) and
> whether hot shared buffers earn an inline header variant later. **[D-rc]**

### 3.4 Atomicity
On the **single-carrier path** the count is a plain load-add-store, gated by the existing
`__cajeta_live_set_mt` flag exactly as the live-set already gates its own locking. **Sending a
shared slice across a spawn/carrier boundary flips the process to atomic (acq-rel) counting** —
the same escalation discipline the runtime already uses. Cross-carrier drops therefore remain
correct without taxing single-carrier code.

### 3.5 Free path
When the count reaches zero, the free **routes through the live-set claim** (claim-then-free), so
a racing auto-field-drop still no-ops — the existing double-free guarantee is preserved on top of
the new last-drop protocol.

### 3.6 Owner-drop semantics
Every drop of a sliceable buffer's owner gains one branch: **shared-bit clear → free exactly as
today** (the overwhelmingly common, predicted path); **set → decrement via the side table, free at
zero**. Container element-drops run the same synthesized drop hook (§6.1) so stakes held inside
collections decrement correctly.

**Use cases**
- **3.7** As a developer, when I create and drop strings/arrays that are never sliced-and-stored,
  then they carry **no rc traffic whatsoever** — one predicted bit-test at drop; behavior and
  performance are unchanged from today.
- **3.8** As a developer, when I `substring` an IO-read `String`, store the result in a
  longer-lived record, and let the original drop, then the bytes remain valid and the buffer is
  freed exactly once — when the stored slice (the last stake) drops.
- **3.9** As a developer, when I slice a **literal**, then no counting ever occurs (static state,
  infinite lifetime) — pure arithmetic.

## 4. Escape resolution — when the compiler copies, shares, or rejects

### 4.1 The layered trigger
1. **Provably local** — the slice/borrow demonstrably cannot outlive its source (used and dropped
   in scope, passed to a callee that provably does not retain it): it stays a **plain borrow**.
   Zero rc, zero copies. *This is the common case and the fast path.*
2. **Escapes, or cannot be proven local** — the compiler applies the **resolution table** (§4.2).

This replaces the borrow checker's unconditional *rejection* of "borrow may outlive its source"
with a *resolution* — **for the eligible types below only**; everything else still rejects
exactly as today.

### 4.2 The resolution table
| Source of the escaping reference | Resolution | Rationale |
|---|---|---|
| **small value** (stack local; slice ≤ copy-threshold; Inline `Utf8`) | **copy** — or **move** when it is the last use | copying is cheaper than counting; stays two-state |
| **large immutable heap buffer** (long substring, big array slice) | **share** — promote the root buffer `owned → shared`, escapee becomes a stake | copying is the expense being avoided; sharing is transparent because the bytes are immutable |
| **arena-backed buffer** (frame-arena concat results — no individual free exists) | **copy, always** (any size) | arena memory is bulk-reset; it cannot be promoted |
| **mutable or identity-bearing object** | **compile error**, exactly as today | aliasing/identity sharing is the deferred general door |

The **copy↔share boundary is a size threshold** — the `Utf8` Inline cap (§8) for text; a
plan-time byte cap for `Slice<T>` — the dial between "more copying" and "more counting."

> **TBD (plan-time):** the `Slice<T>` threshold value (lean: low hundreds of bytes). **[D-thresh]**

### 4.3 The soundness discipline
The escape analysis must be a **sound over-approximation**, and the fail-safe direction is fixed
by an asymmetry:
- **over-approximate** (resolve a slice that was actually local) → a wasted copy or count pair —
  cheap, bounded;
- **under-approximate** (miss an escape) → a dangling pointer — fatal.

Therefore: **where the analysis cannot decide, it resolves (unsure ⇒ copy/share per the table),
never silently borrows.** The existing passes do not qualify (§1.4 — they are rejecters); a new
slice-escape pass is a headline plan deliverable. It may start conservative (resolving more than
strictly necessary) and gain precision release over release **without ever being unsound** — the
false-positive rate is a measurable performance knob, not a correctness risk.

### 4.4 Retention amplification — the Java-substring lesson
A shared slice keeps its **entire root buffer** alive: a 16-byte slice of a 100 MB IO read,
stored, retains 100 MB (the leak that drove Java to make `substring` copy in JDK 7u6). v1 policy:
- the **threshold** already copies small escapees (the dominant case — keys, symbols, tokens);
- `.clone()` (§6.4) is the explicit **detach** — copy the window, drop the stake — for the
  developer who knows the buffer is huge and the slice long-lived;
- a **ratio guard** (copy when `len ≪ buffer.count`, e.g. < 1/64th, even above the threshold) is
  specified as a plan-time knob, default **off** — ship simple, measure, then decide.

> **TBD (plan-time):** ratio-guard default and diagnostics (a lint that flags a small-slice-of-
> huge-buffer share is cheap and useful). **[D-amp]**

**Use cases**
- **4.5** As a developer, when I use a substring only within its scope (`if s.substring(0,4) ==
  "http"`), then no copy, no count, no allocation occurs — it is a borrow.
- **4.6** As a developer, when I store a 6-character venue code out of a parsed line into a
  record, then it is **copied inline** (≤ threshold ⇒ Inline `Utf8`) — no rc, and the parsed
  line's buffer is freed on schedule.
- **4.7** As a developer, when I store a 40 KB body slice of a 60 KB response into a cache that
  outlives the response, then the response buffer is **promoted to shared** and lives until the
  cache entry drops — no 40 KB copy.
- **4.8** As a developer, when I try to store an escaping borrow of a mutable object, then the
  compiler rejects it exactly as today (`#`-transfer or `clone()` it — §9).

## 5. Mutability — the value-copy line

- **5.1** **Immutable slices** (`Utf8`/`String`, read-only column cells, immutable array slices):
  copying **shares** the backing — observationally identical to a value copy because the elements
  never change. Immutable slices are **value-copy-clean** and are the *only* slices eligible as
  `record` fields — resolving `records-spec.md` §2.6.5: the rule is not "no pointers" but **"no
  owning/mutable heap identity."**
- **5.2** **Mutable slices** (a writable view for in-place algorithms, a write-through column
  view): writes are visible through every alias; copying aliases shared mutable state — **not**
  value-copy-clean. **In v1 a mutable slice is borrow-only: it cannot escape its source (compile
  error), cannot be shared, and cannot be a record field.** The shared-mutable door stays closed.
- **5.3** As a núcleo author, when a column hands out cell views, then the column's data region is
  **frozen** (append-sealed) first, so §5.1's immutable-share contract holds; in-place column
  mutation requires the buffer to be **unshared** (shared-bit clear).

## 6. Copy, move, equality, `clone()`

### 6.1 The synthesized value-copy (shallow copy + share)
A value-copy of an object with heap-referencing fields is a **shallow copy** — the field slots are
copied — **plus one `rc++` per heap-referencing field** (promoting an `owned` field's buffer to
`shared` at that moment if it wasn't already). Properties:
- **Bounded by the static inline layout**: the walk recurses over nested *value* sub-aggregates
  (finite, compile-time-known depth — a value type cannot inline-contain itself) and **stops at
  every heap edge — it shares, it never traverses**. O(fields); cycle-proof; it cannot deep-copy a
  runtime graph. This boundedness is a guarantee of the value-type size rule, not a hope.
- **Transparent iff the heap fields are immutable** (share ≡ copy). A **mutable** heap field
  forces deep-copy or the deferred general door — which is exactly why record fields are
  immutable-only (§5.1).
- A value type is **trivially copyable (pure memcpy) iff all its fields are trivial** — memcpy is
  the *optimization*, not the definition. A record holding a Shared-capable slice is a
  **non-trivial value type** carrying this synthesized copy and the matching synthesized drop
  (`rc--` per shared field) — the amendment `records-spec.md` §2.6.5 must adopt. Records'
  synthesized `with(...)` runs the same hook.

### 6.2 Move (`#`)
A `#`-move transfers the stake: memcpy + deactivate the source's drop, **rc-neutral** — a codegen
path distinct from copy (value-move today is a raw memcpy; it gains this hook only for
non-trivial values). See §9 for `#`'s role in the model.

### 6.3 Equality and hashing are representation-independent
Two immutable slices are equal iff their **window bytes** are equal — regardless of
representation (an Inline `"EURUSD"` equals a Shared `"EURUSD"`). Hashing hashes the window
bytes. The `Utf8` prefix (§8) is a comparison *accelerator*, never the definition.

### 6.4 `Object.clone()` — unblocked
`clone()` is the **explicit sibling of the implicit value-copy**: value types copy implicitly on
assignment; reference classes — whose `=` *borrows* — copy via explicit `clone()`; **both run the
§6.1 mechanic** (shallow copy + `rc++`/promote per heap field). cajeta `clone()` = Java-shallow
clone **+ rc**: Java's shallow clone leans on GC to keep aliased referents alive; cajeta has no
GC, so an aliased heap field after a shallow clone *must* be `shared`. Override `clone()` to
deep-copy a mutable field (Java's existing escape hatch). On a slice, `clone()` **detaches**:
copies the window into a fresh owned buffer and drops the stake (§4.4's amplification valve).
**Deliverable: `__cajeta_object_clone` stops returning null.**

## 7. The reductions

- **7.1 `String.substring(a, b)`** returns a `Utf8` (§8) over the same bytes — O(1), no
  allocation, no element copy (SSO carve-out: §8.3). Escape resolution then applies per §4: local
  use borrows; a stored short result is Inline; a stored long result shares. The public signature
  keeps returning `String` (the facade), so call sites are source-compatible.
- **7.2 Array slice (`arr[a:b]`)** yields `Slice<T>` with the same contract (threshold-copied or
  shared on escape; arena-backed always copies).
- **7.3 Column cells**: reading a cell of a string/array column yields a slice into the column's
  frozen data buffer — **a borrow by default** (the column's lifetime dominates a scan; **no rc
  traffic in filter/join/groupby loops**). A cell that *escapes the table* goes through §4.2 —
  typically Inline-copied (symbols) or, for a long cell, a stake on the column buffer. Column
  **ingest** copies bytes into the contiguous Arrow data buffer — the one sanctioned, bulk,
  unavoidable copy (Arrow requires contiguity); everything downstream of ingest is zero-copy.
- **7.4 `Tensor`/`Storage` — deferred ([D3])**: unification was evaluated and rejected for v1 on
  two grounds: (a) the autograd tape *stores* views that outlive the forward scope — the norm,
  not the exception — so tape-resident tensors would all promote, putting count traffic on the ML
  hot path; (b) `Storage`'s buffer layout is load-bearing for Arrow bit-identity and GPU upload
  base math. `Storage` keeps its current live-set discipline; revisit only if the side-table
  design (§3.3, no layout change) plus a tape-aware policy solves both.

## 8. `Utf8` — the string specialization

### 8.1 Representation — a 16-byte tagged union
```
Utf8 (16 bytes, by tag):
  Inline : { u32 lenTag; i8[12] bytes }              // len <= 12: the bytes ARE the value
  Static : { u32 lenTag; i8[4] prefix; i64 ptr }     // .rodata literal window; immortal
  Shared : { u32 lenTag; u32 offset; i64 basePtr }   // window into a shared buffer
```
- The **tag** lives in reserved bits of the first word (`lenTag`), making all three forms
  self-describing; `basePtr` is the **allocation base**, so the shared-bit/side-table lookup (§3.3)
  is direct, and chained substrings always attribute to the root.
- **Normalization rule: any operation whose text result is ≤ 12 bytes produces an Inline** —
  including slices of Static and Shared sources. Pointer forms therefore always carry `len > 12`,
  and the tag assignment is total and unambiguous.
- The Static form keeps a 4-byte **prefix** as a comparison accelerator; the Shared form spends
  those bytes on `offset` instead (long strings; equality falls back to the window bytes — §6.3).

### 8.2 `String` — the facade
`String` becomes the rich API (`indexOf`, `split`, codepoint ops, `charAt`, `cachedCpLength`) over
a `Utf8` core. The legacy `mode 0/1` collapses into the tag; `__cajeta_string_drop` becomes
tag-dispatch; `substring` returns a slice instead of allocating. **Migration risk (plan-time):**
String's field layout is pinned by JIT tests, literal codegen materializes view-mode Strings, the
SSO region mirrors the array header, and multiple intrinsics poke String fields directly — the
re-core is a real migration with a regression surface; the acceptance bar is the full string test
suite unchanged.

### 8.3 SSO interaction
- Slicing a string whose bytes live in the **SSO region**: result ≤ 12 B → **Inline** (bytes
  copied into the slice — safe under moves by construction); result 13–24 B → a **one-time spill**
  of the bytes to a heap buffer, which the slice then shares. The spill is a small allocation —
  an explicit, documented **carve-out** from §10's no-allocation criterion (13–24 B substrings of
  SSO-resident strings only).
- **Asserted invariant: the compiler never produces a Shared/Static `Utf8` pointing into a
  wrapper's `ssoData`** (always Inline-or-spill) — a pointer into an SSO region dangles when the
  wrapper value moves.

**Use cases**
- **8.4** As a developer, when I substring a venue code / currency / category (≤ 12 B), then I get
  an Inline `Utf8` — 16-byte struct copy, no buffer, no counting — and a record/column full of
  them is trivially memcpy-able.
- **8.5** As a developer, when I substring a literal, then it is Static (or Inline if ≤ 12 B) —
  free forever.
- **8.6** As a developer, when I keep a long slice of a dynamic/IO string beyond its source, then
  it is Shared per §3–§4 — alive exactly as long as needed, freed once.

## 9. The role of `#` *(decided 2026-07-02)*

The `shared` state generalizes `#`'s meaning without changing its shape: `#` transfers **an owning
stake** (formerly always *the unique* ownership; now possibly one stake among co-owners), and a
move is always **rc-neutral** (§6.2).
- **`#` is the zero-cost escape path.** At an escape site the compiler's resolution costs a copy
  or a count (§4.2); `list.add(#s)` costs **neither** — the stake transfers. The implicit
  machinery makes naive code *sound*; `#` remains how deliberate code makes it *free*.
- **Signatures generalize cleanly:** `#T f()` = the caller receives a stake (unique or shared);
  `T f()` = the caller receives a borrow (must not outlive the receiver/argument — rules
  unchanged). `#T` formals still mean "callee takes the stake."
- **The explicit trichotomy:** **borrow** is the default (`=`); **`#`** is the explicit move;
  **`clone()`** is the explicit copy/detach; **share has no spelling** — it is exclusively a
  compiler-inserted resolution, preserving the no-annotation identity (§0).
- **The discipline boundary — RESOLVED (2026-07-02): transparent-only resolution.** Silent
  resolution (§4) applies **only where value semantics make it observationally transparent** —
  small values (a copy is indistinguishable) and **immutable** buffers (a share is
  indistinguishable). **Identity-bearing / mutable objects keep today's error-and-`#`
  discipline** — there a silent resolution would change meaning (aliasing vs snapshot), so
  explicitness stays mandatory. *The compiler decides only when the decision is invisible; the
  moment it is visible, it is the developer's.*
- **Auto-move on last use — RESOLVED: allowed.** When the source is provably dead after the
  escape, the compiler upgrades the resolution from copy to move (inferring `#`). Unobservable —
  a pure optimization, not a semantic change.
- **Visibility lint (plan deliverable):** a diagnostic tier flags each silent resolution
  ("escape resolved to a share; `#` would make it free"), keeping the cost surface honest for
  those who want the old strictness — informational, never an error.

## 10. Acceptance criteria (spec-level)
- **Construction** of a slice (`substring`, `arr[a:b]`, sub-slice, column cell) performs **no heap
  allocation and no element copy** — with the sole documented carve-out of §8.3's 13–24 B
  SSO spill. (Escape *resolution* may copy small slices **by design** — that is the policy
  working, not a violation.)
- A slice of a dynamic/IO backing stored in a value that **outlives its source** remains valid
  until the last stake drops; a literal slice is immortal; nothing dangles; every buffer is freed
  exactly once, through the live-set claim.
- Buffers whose slices never escape incur **zero rc traffic** (one predicted bit-test at owner
  drop); local slices are plain borrows; column scans count nothing.
- Immutable slices are value-copy-clean and `record`-eligible; mutable slices are borrow-only
  (escape = compile error); `records-spec` §2.6.5 is amended to "value semantics, synthesized
  copy/drop — memcpy iff all fields trivial."
- `String` re-cored over `Utf8` with the full existing string suite green; `substring` is O(1);
  equality/hash representation-independent.
- `Object.clone()` implemented per §6.4 (`__cajeta_object_clone` non-null); slice `clone()`
  detaches.
- `Tensor`/`Storage` behavior unchanged (unification deferred).

## 11. Open questions (resolve at plan time)
- **[D-rc]** Side-table structure + count-word bit choice (§3.3); revisit an inline header only if
  side-table drop cost proves hot.
- **[D-thresh]** The `Slice<T>` copy↔share byte threshold (lean: low hundreds of bytes; `Utf8`'s
  is the 12-byte Inline cap).
- **[D-amp]** Ratio-guard default (lean: off) + the small-slice-of-huge-buffer lint (§4.4).
- **[D-#] — RESOLVED (§9):** transparent-only resolution (small values + immutable buffers);
  identity/mutable objects keep error-and-`#`; auto-move on last use allowed; visibility lint is
  a plan deliverable.
- **[D3]** `Tensor`/`Storage` unification — deferred; revisit condition in §7.4.
- **Naming:** `Slice<T>` vs `Span<T>`; `Utf8` vs `Str`; whether `Utf8` is a public type or
  `String`'s exposed core.
- **Mutable-slice surface** (v1 borrow-only): distinct type (`MutSlice<T>`) vs qualifier — shared
  with the tensor-view and write-through-column stories.
- **Future gates:** pointer-form `Utf8` in GPU kernel-visible types (host pointers are meaningless
  on device — needs a gate when records become kernel args); `weak` and the general shared-object
  door (§0).

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
