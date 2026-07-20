# Channel payload ownership (the owning channel) — spec (draft)

Origin: primavera Phase-5 handoff harness, 2026-07-19 (cajeta-primavera
`plan/primavera-plan.md` Phase 5, finding 6: "transferring a FiberContext
through Channel/Optional slots SIGSEGVs in the receiving fiber").
Rev 3 (same day): surface settled as **`peek()` + single-shot flagged
`get()`** — no `take()`, no new compiler work. Decision: **Optional is a
temporary wrapper (an envelope), not a vehicle for containership** — its
mode is fixed at construction (`Optional(true, #v)` carries the title,
`Optional(true, v)` a borrow), `peek()` inspects without consuming, and
`get()` unwraps once, yielding exactly what the producer put in.

## 1. Definition

The concurrency model promises a **move** through a channel; the channel
**lends**. Three documents and one implementation disagree, and the result is
a use-after-free that crashes the documented idiom:

- `docs/specification/concurrent/Concurrency.md` § Sendability: a value
  crosses a thread boundary "**Transferred** via `#` … the receiving thread
  is the new owner. **Always safe.**"
- `docs/specification/concurrent/FiberLocal.md` Layer 3 prescribes exactly
  that for the unstructured handoff:
  `channel.send(WorkItem(payload, #ctx))` … "The snapshot is transferred
  with `#` (single new owner), matching the `detach`/channel captures rule."
- `runtime/src/cajeta/concurrent/Channel.cajeta` `send()` **borrow-stores**
  its slot (`T lent = item; this.buf[this.tail] = lent;`) — with an in-source
  comment conceding "An owning channel — `#=` here + a fused claim in
  receive() — is a deliberate future conversion".
- Under the rev-2 transfer rules (`OwnershipTransfer.md` interaction matrix),
  a plain formal **accepts** a caller's `#x` transfer, and an unconsumed
  transfer **drops in the callee**.

Chain the four: `ch.send(#x)` makes `send` the runtime owner of `x`, the
slot takes only a borrow, and **the payload drops when `send` returns**. The
slot dangles. For `FiberContext`, `~FiberContext` frees the runtime snapshot,
so the receiver's `ctx.run(...)` calls `fiberContextInstall` on freed memory
— the observed SIGSEGV (crash in the receiving fiber, drop-chain entries at
the receive/run site; reproduced 2026-07-19 under toolchain 0.9.2 in
primavera's `requestAcrossHandoff`).

There is today **no way to move a payload through a channel at all**: `#` at
the send site kills it; a plain send lends, which is only sound when the
sender outlives the receiver's use (the structured case a channel is
explicitly *not* for, per Layer 3).

**Secondary gap — the receiver cannot claim ownership out of the
`Optional`.** `receive()` hands its result back in an `Optional<T>`, and:

1. `receive()` returns `stack Optional<T>(true, #item)` where `item` is a
   borrow of the slot — the borrow flag forwards, so the Optional holds a
   borrow of channel state the next `send` may overwrite.
2. `Optional.get()` is borrow-only. Even an Optional that OWNS its value
   (the ctor's `this.value #= value` store is dual-capable) can never hand
   the title onward — a moved payload dies when the receiver's temporary
   Optional drops. And there is no inspection/unwrap split, so no verb is
   free to take on unwrap semantics.
3. `Tasks.selectReceive` compounds it: `SelectResult<T>(i, r.get())` wraps a
   borrow of a temp Optional that drops on return.

Workaround in the wild (primavera `PrimaveraTest.requestAcrossHandoff`):
hold the snapshot in an owned static and rendezvous over `Channel<int32>`.
It works, but it is not the documented idiom and cannot generalize to a
worker pool serving many concurrent items.

## 2. Doctrine: Optional is an envelope

`Optional<T>` is a **temporary wrapper** — the vehicle a producer uses to
hand a maybe-value across one boundary (a return, a channel receive), to be
unwrapped promptly and discarded. It is not a container: no membership, no
long-lived storage, no entry-bit bookkeeping beyond the one value slot. Two
consequences drive the whole design:

- **The wrapper's mode is fixed at construction**, by the producer's
  spelling — `Optional(true, #v)` carries the title; `Optional(true, v)`
  carries a borrow. The ctor's dual-capable store already implements this;
  the mode is a fact about what the producer put in, not a per-access
  choice.
- **Inspection and unwrap are different verbs, not different spellings.**
  `peek()` looks; `get()` unwraps once and empties the envelope, yielding
  the value in whatever mode the producer provided (a flagged return).
  Container doctrine (title-tracking §6.2 read / §6.3 distinct-name
  extract, panic-on-no-title) does not govern here — that split exists for
  structures with membership. An envelope has exactly one thing, one look
  (`peek`), and one hand-over (`get`).

## 3. Features

### 3.1 `peek()` + single-shot flagged `get()`

Two verbs, both signature-plain, all machinery shipped:

- **`peek()`** — the inspection verb: always returns a borrow, in both
  modes; never consumes; the envelope is unchanged. (Today's `get()` body,
  renamed in role.) Empty → `NoOptionalValueException`.
- **`get()`** — the unwrap: a **flagged return** (title-tracking §4.2.1,
  the mechanism `remove` already uses). Body: absence check first
  (`NoOptionalValueException`), then extract the value from the field
  place with its resident mode (title-tracking 3C), mark the envelope
  **empty**, and forward — title if the producer moved in, borrow if the
  producer lent. The receiver's drop entry activates only if a title
  actually rode. Single-shot: a second `get()` (or a `peek()` after
  `get()`) finds an empty envelope and throws — the envelope doctrine made
  mechanical.
- `orElse(fallback)` — peek-flavored: borrow-or-fallback, unchanged,
  non-consuming.

The idiomatic acceptance spelling is `#=` — `FiberContext ctx #= o.get();`
— documenting at the site that a title may arrive; runtime correctness
rides the flag word either way (as with `V v = map.remove(k)` today), and
the loud-plain-store diagnostic nudges a plain `=` that receives a title.
There is no `take()` and no panic path: "unwrap a lent envelope" is not an
error — a borrow is exactly what the producer sent, and lending was the
producer's lifetime contract. A receiver that *requires* ownership asserts
`Cajeta.owned(v)` after the unwrap.

Use cases:
1. As the channel receiver, when I write `FiberContext ctx #= o.get();`,
   then I hold the snapshot in the mode the sender chose — owned if moved,
   borrowed if lent — and the temp Optional's drop is a no-op on it.
2. As an inspector, when I write `if (o.peek().tag == 42)`, then nothing is
   consumed — the envelope still carries the value, in its mode.
3. As a receiver who unwraps twice, when the second `get()` runs, then I
   get `NoOptionalValueException` — the envelope is spent, loudly.
4. As a borrower-side consumer (the `getCause()` walk), when I bind
   loop-scoped `Optional` locals and `peek()`, then every iteration's
   envelope drop is a no-op on the chain — the optional-borrow-ownership
   guarantees, unchanged.
5. As an owning producer whose envelope is dropped unclaimed, when nobody
   calls `get()`, then the envelope's drop releases the value exactly once
   (the §2.2.4 pin, unchanged).

Migration note: existing `get()` call sites that INSPECT (read without
needing the value past the envelope) migrate to `peek()`; sites that
consume keep `get()` and gain the honest single-shot semantics. The stdlib
sweep is a plan unit; out-of-tree code keeps compiling (same signature) and
changes behavior only on the previously-pathological double-`get()` of an
owning envelope.

### 3.2 Dual-capable `send` — caller-spelled, matching `add`/`put`

`send(x)` lends, exactly as today; `send(#x)` moves the title into the slot
(`Cajeta.owned(item)` branch → `this.buf[this.tail] #= item`, slot ownership
bit set) — the established title-stores pattern from `ArrayList.add` /
`HashMap.put`. A sender parked on a full channel holds the title in its
frame until the store happens (no early drop).

Use cases:
1. As the Layer-3 producer, when I `ch.send(WorkItem(payload, #ctx))` and my
   extent ends before the worker runs, then the worker receives a live
   payload — the channel owns it in between.
2. As an existing structured user, when I `ch.send(x)` inside a scope that
   outlives the receiver's use, then nothing changes — no new copies, no
   new obligations.

### 3.3 Mode-forwarding `receive` / `tryReceive`

`receive()` forwards the slot's mode into the envelope: an owned head slot
moves its title into the returned `Optional` (slot bit clears — the "fused
claim" the send comment anticipated); a borrowed slot flows through as a
borrow. The envelope's mode thus mirrors the sender's spelling, which is
exactly what §2's doctrine requires — the producer decides, the envelope
carries, the consumer claims. The closed-and-drained placeholder path
(`T none = this.buf[this.head]`) keeps its plain read — it must never
extract or fabricate a title.

Use cases:
1. As the worker, when the sender moved the payload, then `#= o.get()`
   hands me the title and no subsequent `send` can touch it.
2. As a mixed-use consumer, when the sender lent, then my plain `get()`
   borrows and my `#= o.get()` panics — each mode behaves, loudly.

### 3.4 Teardown owns the tail

Dropping a channel with undelivered **owned** items drops each exactly once
(the `T[] buf` slot-ownership bits make the walk exact — verify the
synthesized field/array drop covers it; otherwise `~Channel` drains
explicitly). The class-doc note "Does NOT drain buffered items" is updated:
lent items remain the sender's; owned items free with the channel. The same
exactly-once rule applies to an owning envelope dropped unclaimed.

Use case: as a server shutting down, when I drop a channel with queued
work items, then their destructors run — no leak, no double-free.

### 3.5 `selectReceive` preserves the title

With flagged `get()`, `selectReceive` needs no mode branch at all:
`SelectResult<T>(i, r.get())` unwraps the temp envelope in whatever mode
the sender chose, and `SelectResult`'s dual-capable ctor store (audit; fix
if plain) carries it onward. One line, both modes.

## 4. Non-goals

- **Layer-3 lifetime semantics are unchanged.** A `FiberContext` snapshot
  still does not extend the lifetime of the `FiberLocal` boxes it points at;
  a producer whose bindings the continuation reads must still outlive that
  use (primavera's rendezvous shape remains the correct discipline for
  request bags). This spec makes the *snapshot object* move soundly; it does
  not make snapshots keep bindings alive.
- **No container-doctrine changes.** `ArrayList`/`HashMap` keep §6.2/§6.3
  (borrow reads, distinct-name `operator#[]` extraction, panic-on-no-title):
  membership structures answer a different question than envelopes. The
  claim-on-flagged-return mechanism introduced here MAY later subsume
  `operator#[]`, but that unification is out of scope.
- No channel capacity/fairness/MPMC changes; no `detach` capture-rule
  changes; no Send/Sync-style traits (Sendability stays derived).
- No broader Optional surface (Some/None factories, combinators).

## 5. Systems

- **No compiler changes.** Flagged returns (§4.2.1), field-place
  extraction (3C), and dual-capable stores are shipped; this is a stdlib
  conversion onto them.
- `runtime/src/cajeta/lang/Optional.cajeta` — `peek()` (today's borrow
  body), `get()` re-bodied as the single-shot flagged unwrap (absence
  check → field-place extract with resident mode → mark empty → forward);
  class doc rewritten around the envelope doctrine; stdlib `get()`-site
  sweep (inspect → `peek()`).
- `runtime/src/cajeta/concurrent/Channel.cajeta` — send / receive /
  tryReceive / `~Channel` + class docs.
- `runtime/src/cajeta/concurrent/Tasks.cajeta` (`selectReceive`) +
  `SelectResult.cajeta`.
- Docs: `Concurrency.md` (channel section gains the lend-vs-move table),
  `FiberLocal.md` Layer 3 (receiver side becomes `ctx #= o.get()`),
  `OwnershipTransfer.md` / title-tracking notes (claim-on-call-result as a
  new claim context), `Optional`/`Channel` class docs + skills.
- Verification: `cajeta jit-run` repro program (§6) per house convention;
  ecosystem acceptance = cajeta-primavera's `requestAcrossHandoff` reverted
  to the documented channel-carried idiom.

## 6. Acceptance repro

One program, five assertions (drop-tracking fixture `Tracked` with a
destructor counter; full source staged in the plan):

- **A (send is a move, not a kill):** `ch.send(#t)` → `Tracked.drops == 0`
  after `send` returns, while the item is undelivered.
- **B (unwrap yields the sent mode):** worker binds
  `Tracked t #= o.get();` after a moved send — reads `tag` correctly;
  `drops == 1` only after the unwrapped local drops, not at the envelope's
  drop. After a LENT send, the same line yields a borrow and the drop count
  never moves.
- **B′ (peek does not consume; get is single-shot):** `peek()` on an owning
  envelope consumes nothing (its later drop releases exactly once when
  unclaimed); a second `get()` throws `NoOptionalValueException`.
- **C (the documented idiom runs):** `FiberContext` moved through a channel;
  the worker claims and installs it; the continuation runs — no SIGSEGV.
- **D (teardown):** drop a channel holding an undelivered owned item →
  `drops` increments exactly once; a lent undelivered item → zero.

Status: **draft** — blocked on nothing; runs deferred while the current
sweep occupies the machine (2026-07-19).
