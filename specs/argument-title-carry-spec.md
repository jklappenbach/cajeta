# argument-title-carry — a plain formal must carry the argument's ownership mode

## 1. Definition

Opened 2026-08-09, from the collection ownership reversal ("none of the
collections should force ownership"). Relaxing the 12 collection formals
from `#T` to `T` is correct by design but **unsound as implemented**: a
plain formal erases the argument's ownership mode, so an owned value
passed plainly is freed at the caller's scope exit while the collection
keeps the pointer.

`MethodCallExpression.h:20` states the current rule:

> the call-site transfer machinery fires the drop deactivation when
> EITHER this is true OR the matching formal is `#T`-marked.

Relaxing the formals removed the second disjunct. Nothing else replaced
it, so a plain call no longer surrenders title.

### 1.1 Measured

200-key `HashMap<String,int32>` round trip, two runs of the same binary:

| how the key is passed | run 1 | run 2 |
|---|---|---|
| temporary `m.put("k"+i, i)` | 1/200 | 95/200 |
| named local `m.put(k, i)` | 2/200 | 2/200 |
| explicit `m.put(#k, i)` | 200/200 | 200/200 |

Nondeterminism across runs of one binary is the use-after-free
signature. This is the cause of the `HashMapTests.stringKeysThousandRoundTrip`
failure.

## 2. Why the two obvious fixes are wrong

Both fail on a case the other handles, which is what makes the offer
protocol (§3) necessary.

- **2.1 Callee searches the drop chain for the argument and disarms it.**
  A *borrowed* argument also has a live entry — owned by an **outer**
  frame. Disarming it steals title from the true owner:
  `outer() { Box b = heap Box(); inner(b); }` would let `inner`'s store
  take `b` out from under `outer`.
- **2.2 Caller disarms unconditionally when the argument is owned.**
  A callee that merely *reads* the value then leaves it freed under the
  caller's feet: `String k = ...; foo(k); use(k);` — `use(k)` is a UAF.

The title must therefore be **offered**, not surrendered: the caller
advertises that it is willing to give title, and the transfer happens
only if the callee actually takes it.

## 3. Design — the tenderable title offer

One spare bit in `struct cajeta_drop_entry`. The base entry is 32 bytes
(`obj` 8 + `drop_fn` 8 + `prev` 8 + `active` 1 + 7 pad), and the debug
variant carries `_pad[3]` at +25, so a flag at +25 costs **zero bytes in
both** and does not move `alloc_line`/`alloc_file`.

    int8_t active;      // +24  unchanged
    int8_t tenderable;  // +25  NEW — title is on offer to a callee

- **3.1 Caller.** For an object-typed argument passed to a *plain*
  formal, when the argument is statically owned in **this** frame (it has
  a drop entry here, or is an owned temporary), mark that entry
  `tenderable = 1` immediately before the call and clear it immediately
  after. A borrow is never marked — the caller has no entry of its own to
  offer.
- **3.2 Callee.** `#=` whose RHS is a plain formal walks the drop chain
  from the top for the first **active and tenderable** entry matching the
  object pointer. Found → disarm it (`active = 0`) and arm the slot's
  title bit: the slot now owns. Not found → store a borrow, arm nothing.
- **3.3 Unstored arguments need no cleanup.** If the callee never stores,
  the entry is simply never disarmed; the caller clears `tenderable` on
  return and drops normally at scope exit. There is no leak and no
  double-free, and neither side needs to know what the other chose.

### 3.4 Why this is sound where §2 is not

- §2.1 is excluded because an outer frame's entry is never marked
  `tenderable` — only the *immediate* caller marks, and only for
  arguments it owns itself.
- §2.2 is excluded because the caller's entry survives untouched unless
  the callee actually took title.
- The top-down search finds the caller's entry rather than an ancestor's:
  entries above the caller's belong to the callee's own frame and cannot
  reference an object that arrived from outside.

### 3.5 No ABI change

The mode rides in the drop chain, not the signature. No hidden
parameter, no pointer tagging, no object-header field — so vtables,
closures, FFI, and the JIT are all untouched.

## 4. The last-use hazard (open)

`m.put(k, i)` where `k` is **used after the call** now silently
invalidates `k` if the collection takes title. A move that the caller
cannot see is the one genuinely unpleasant corner of mode carry.

Two candidate resolutions, to be decided before implementation of §3.1:

- **4.1 Offer only at last use.** The caller marks `tenderable` only when
  the argument is dead after the call (always true for temporaries; for
  named locals, requires liveness). A still-live local is passed as a
  borrow. Sound, and keeps `#` as the only way to force a move of a
  live name.
- **4.2 Offer always, reject later use.** Mark unconditionally and treat
  a taken title as a static move, rejecting subsequent reads of the name.
  Requires the caller to know statically whether the callee took it —
  which it does not under virtual dispatch. Rejected unless a static
  approximation is found acceptable.

§4.1 is the recommended reading and is what §3.1 above assumes.

## 5. Requirements

- **5.1** An owned temporary passed to a plain formal and stored via `#=`
  transfers title: `m.put("k" + i, i)` round-trips 200/200,
  deterministically.
- **5.2** A borrowed argument passed to a plain formal and stored via
  `#=` stores a **borrow**: the outer owner retains title and drops
  exactly once.
- **5.3** An argument passed to a plain formal that the callee never
  stores is unaffected: the caller still owns it and may use it after the
  call.
- **5.4** `#x` at a call site is unchanged — it still demands ownership
  statically, and `CAJETA_ERROR_TRANSFER_OF_BORROW` still rejects `#x`
  where `x` is a borrowed formal.
- **5.5** Drop-entry size is unchanged in both release (32) and debug
  (40) modes.

## 6. Regression pins

- **6.1** The three §1.1 rows, as executable pins.
- **6.2** The §2.1 shape: `outer` owns, `inner` stores plainly into a
  collection, `outer` uses the value after `inner` returns and at scope
  exit it is dropped exactly once.
- **6.3** The §2.2 shape: `foo(k)` reads only; `use(k)` after the call is
  valid.
- **6.4** Restore the three mixed-ownership pins that the uniform-transfer
  migration rewrote: `mixedOwnershipMapDropsOnlyOwnedEntries`,
  `indexedBorrowStoreLeavesOwnerAlive`, `removeReturnsBorrowOwnerKeepsTitle`.
- **6.5** Invert the pins that encode the old mandatory-ownership design:
  `arrayListLendIsRejected`, `hashMapLendIsRejected`, `indexedLendIsRejected`,
  `arrayListStringLendIsRejected` — lending is now legal.

## 7. Blocking status

The working tree currently has the formals relaxed **without** §3, which
is the unsound intermediate state measured in §1.1. It must not ship.
Either §3 lands first, or the formal relaxation is reverted for the
release.
