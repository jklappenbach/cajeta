# Channel payload ownership (the owning channel) — spec (draft)

Origin: primavera Phase-5 handoff harness, 2026-07-19 (cajeta-primavera
`plan/primavera-plan.md` Phase 5, finding 6: "transferring a FiberContext
through Channel/Optional slots SIGSEGVs in the receiving fiber").

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

**Secondary gap — the receiver can't take ownership either.** Two more
links would break the chain even with an owning slot:

1. `receive()` returns `stack Optional<T>(true, #item)` where `item` is a
   borrow of the slot — the borrow flag forwards, so the Optional holds a
   borrow of channel state the next `send` may overwrite.
2. `Optional<T>` *can* carry a title (its ctor stores `this.value #= value`,
   dual-capable) but has **no owning extraction** — `get()` returns a
   borrow. A moved payload would die when the receiver's temporary Optional
   drops.
3. `Tasks.selectReceive` compounds it: `SelectResult<T>(i, r.get())` wraps a
   borrow of a temp Optional that drops on return.

Workaround in the wild (primavera `PrimaveraTest.requestAcrossHandoff`):
hold the snapshot in an owned static and rendezvous over `Channel<int32>`.
It works, but it is not the documented idiom and cannot generalize to a
worker pool serving many concurrent items.

## 2. Features

### 2.1 Dual-capable `send` — caller-spelled, matching `add`/`put`

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

### 2.2 Title-extracting `receive` / `tryReceive`

When the head slot holds a title, `receive()` extracts it (slot bit clears —
the "fused claim" the send comment anticipated) and constructs the returned
`Optional` with the title, which the ctor's dual-capable store already
accepts. A borrowed slot flows through as a borrow, as today. The
closed-and-drained placeholder path (`T none = this.buf[this.head]`) keeps
its plain read — it must never extract or fabricate a title.

Use cases:
1. As the worker, when I `receive()` a moved payload, then the Optional —
   not the channel slot — owns it; a subsequent `send` cannot overwrite it.
2. As a mixed-use consumer, when the sender lent, then I get a borrow and
   the sender's ownership is undisturbed.

### 2.3 `Optional.take()` — the owning extraction

New `public #T take()` on `Optional<T>`: present and holding a title → moves
the title to the caller and marks the Optional empty; empty → throws
`NoOptionalValueException` (same contract as `get()`); present but holding a
borrow → error, matching `operator#[]`'s "extraction requires a resident
owned element" precedent. Receiver idiom:

> **Design rationale (title-tracking §6.2/§6.3/§6.4).** `get()` stays a
> read — always a borrow, correct in both element modes (the
> optional-borrow-ownership loop-walk cases depend on it). The caller's
> `#=` spelling cannot make `get()` yield the title: moving ownership out
> requires the container's participation (presence flips, bit decays), so
> the model dispatches caller-`#` intent to a **distinct canonical name**
> (§6.3.1 forbids a `#`-only overload of one name). `take()` is that name
> for Optional, on the §6.3 *extract* contract — panic when no title is
> resident — chosen over the §6.4 *remove* contract (flagged return,
> title-or-borrow) deliberately: a channel receiver that silently got a
> borrow back from `take()` would recreate the dangling-payload bug this
> spec exists to fix. Generalizing §6.3 dispatch from indexed places to
> method places (`ctx #= o.get()` selecting a `#`-variant, as `#cache[k]`
> selects `operator#[]`) is a coherent future language feature and is
> explicitly out of scope here (§3).

```cajeta
Optional<FiberContext> o = ch.receive();
FiberContext ctx #= o.take();
ctx.run(() -> process());
```

Use cases:
1. As the worker, when I `take()` the payload, then it lives exactly as long
   as my local — the temp Optional's drop no longer frees it.
2. As any Optional user (channel or not), when a factory hands me
   `Optional` of an owned value, then I can finally assume ownership of it —
   the gap exists independently of channels.

### 2.4 Teardown owns the tail

Dropping a channel with undelivered **owned** items drops each exactly once
(the `T[] buf` slot-ownership bits make the walk exact — verify the
synthesized field/array drop covers it; otherwise `~Channel` drains
explicitly). The class-doc note "Does NOT drain buffered items" is updated:
lent items remain the sender's; owned items free with the channel.

Use case: as a server shutting down, when I drop a channel with queued
work items, then their destructors run — no leak, no double-free.

### 2.5 `selectReceive` preserves the title

`SelectResult` stores its value via a dual-capable ctor store, and
`selectReceive` moves an owned payload with `take()` instead of borrowing
from the about-to-drop temp Optional. Lent payloads flow as borrows.

## 3. Non-goals

- **Layer-3 lifetime semantics are unchanged.** A `FiberContext` snapshot
  still does not extend the lifetime of the `FiberLocal` boxes it points at;
  a producer whose bindings the continuation reads must still outlive that
  use (primavera's rendezvous shape remains the correct discipline for
  request bags). This spec makes the *snapshot object* move soundly; it does
  not make snapshots keep bindings alive.
- No new compiler machinery — dual-capable stores, array slot-ownership
  bits, and title extraction are shipped (0.9.2 element-ownership program);
  this is a stdlib conversion onto them.
- No channel capacity/fairness/MPMC changes; no `detach` capture-rule
  changes; no Send/Sync-style traits (Sendability stays derived).
- No broader Optional surface (Some/None factories, combinators) — `take()`
  only.

## 4. Systems

- `runtime/src/cajeta/concurrent/Channel.cajeta` — send / receive /
  tryReceive / `~Channel` + class docs.
- `runtime/src/cajeta/lang/Optional.cajeta` — `take()`; pin that an
  Optional owning its value drops it exactly once.
- `runtime/src/cajeta/concurrent/Tasks.cajeta` (`selectReceive`) +
  `SelectResult.cajeta`.
- Docs: `Concurrency.md` (channel section gains the lend-vs-move table),
  `FiberLocal.md` Layer 3 (receiver side gains `take()`), `Channel` /
  `Optional` class docs + skills.
- Verification: `cajeta jit-run` repro program (§5) per house convention;
  ecosystem acceptance = cajeta-primavera's `requestAcrossHandoff` reverted
  to the documented channel-carried idiom.

## 5. Acceptance repro

One program, four assertions (drop-tracking fixture `Tracked` with a
destructor counter; full source staged in the plan):

- **A (send is a move, not a kill):** `ch.send(#t)` → `Tracked.drops == 0`
  after `send` returns, while the item is undelivered.
- **B (receiver takes intact ownership):** worker `take()`s the payload,
  reads `tag` correctly; `drops == 1` only after the taken local drops.
- **C (the documented idiom runs):** `FiberContext` moved through a channel;
  the worker installs it and the continuation runs — no SIGSEGV.
- **D (teardown):** drop a channel holding an undelivered owned item →
  `drops` increments exactly once.

Status: **draft** — blocked on nothing; runs deferred while the current
sweep occupies the machine (2026-07-19).
