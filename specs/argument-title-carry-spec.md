# argument-title-carry — transfer must be caller-initiated, and a requiring formal must compel it

## 1. Definition

Opened 2026-08-09 from the collection ownership reversal. **The original
finding was wrong and is retracted — see §2.** `#=` does carry the
argument's ownership mode; the relaxed formals are sound. What remains
is a missing DIAGNOSTIC, not a missing mechanism.

The reversal relaxed 12 collection formals from `#T` to `T`, so
collections borrow by default and the developer opts into transfer. The
question this spec opened was whether that leaves owned values stranded.
It does not (§2). The two defects it did surface are a silent transfer
where the rule wants an error (§3) and an undiagnosed lifetime error
(§4.2).

## 2. Retraction — `#=` already carries the mode

The original claim was that a plain formal erases the argument's
ownership mode and that a title-offer protocol (a spare bit in
`cajeta_drop_entry`, caller-side marking, last-use liveness) was needed
to restore it. **That was an error**, inferred from reading
`Expression.cpp:185` — where `dst #= v` is described as the fused
spelling of `dst = #v` — without checking it against the measurements
already in hand.

If `#=` were the unconditional move that reading implies, the map would
take title in the plain case too, and the plain case would have survived.
It did not. The only difference between the surviving and failing runs is
caller-side, which is precisely mode carry working: `#=` arms the slot's
title only when title was tendered.

Measured against the relaxed (`T`, borrowing) formals, no compiler change:

| call | run 1 | run 2 |
|---|---|---|
| `m.put(#k, i)` — caller initiates | 200/200 | 200/200 |
| `m.put(#("k" + i), i)` — initiates on a temporary | 200/200 | 200/200 |
| `m.put(borrowed, i)`, owner outlives the map | 200/200 | 200/200 |
| `m.put("k" + i, i)` — borrow of a dying temporary | 1/200 | 95/200 |
| `m.put(k, i)` — borrow of a scope-local that dies first | 2/200 | 2/200 |

The argument grammar already admits `#` before an arbitrary expression
(`CajetaParser.g4:726`, `parameterLabel? REFERENCE? expression`), so the
caller can initiate on an unnamed temporary. No new syntax and no new
runtime machinery are required.

## 3. The transfer rule (corrected)

`#` at the call site always transfers. A `#T` formal never transfers on
its own — it only OBLIGATES the caller to write `#`.

| formal | argument | meaning |
|---|---|---|
| `#T` | `#x` | transfer |
| `#T` | `x` | **error** — a requiring site compels the caller's `#` |
| `T` | `#x` | transfer — the caller MAY initiate |
| `T` | `x` | borrow |

`MethodCallExpression.h:20-22` violates row 2: it fires the drop
deactivation when "EITHER this is true OR the matching formal is
`#T`-marked. Either suffices; either acknowledges transfer." The second
disjunct makes a missing `#` a SILENT TRANSFER where the rule wants a
compile error. This is almost certainly the root of
`transfer-required-diagnostic-gap` (the `Placement.tiered.add(k)`
hotfix).

## 4. What actually remains

- **4.1** Remove the "OR the matching formal is `#T`-marked" disjunct so
  a requiring site with a plain argument raises
  `CAJETA_ERROR_TRANSFER_REQUIRED` instead of silently transferring.
  Folds `transfer-required-diagnostic-gap` into this spec.
- **4.2** The last two rows of §2 are ordinary lifetime errors — a borrow
  outlived by nothing. They are UNDIAGNOSED: nothing rejects storing a
  borrow of a scope-local into a longer-lived collection. This is the
  real cost of collections that borrow by default, and the only new
  safety work the reversal implies. A lifetime/escape diagnostic is the
  open question; it does not block the reversal.
- **4.3** `HashMapTests.stringKeysThousandRoundTrip` uses
  `m.put("k" + i, i)` and was written against the old `#K` formal. It
  needs `m.put(#("k" + i), i)` — a test update, not a compiler fix.

## 5. Requirements

- **5.1** A requiring formal (`#T`) with a plain argument is a compile
  error naming the fix. No silent transfer.
- **5.2** `#x` against a plain formal transfers (caller-initiated) —
  pinned for a named local and for an unnamed temporary.
- **5.3** A borrow stored in a collection whose owner outlives it stays
  valid and drops exactly once.
- **5.4** `CAJETA_ERROR_TRANSFER_OF_BORROW` rejects `#x` where `x` is a
  borrowed formal — the caller cannot initiate a transfer of what it does
  not hold.
- **5.5** Drop-entry layout is UNCHANGED. The retracted design's spare
  bit is not needed.

## 6. Regression pins

- **6.1** The five §2 rows as executable pins (the last two as
  documented-lifetime-error cases, not as passing behaviour).
- **6.2** Restore the three mixed-ownership pins the uniform-transfer
  migration rewrote: `mixedOwnershipMapDropsOnlyOwnedEntries`,
  `indexedBorrowStoreLeavesOwnerAlive`, `removeReturnsBorrowOwnerKeepsTitle`.
- **6.3** Invert the pins encoding the old mandatory-ownership design:
  `arrayListLendIsRejected`, `hashMapLendIsRejected`, `indexedLendIsRejected`,
  `arrayListStringLendIsRejected` — lending is now legal.
