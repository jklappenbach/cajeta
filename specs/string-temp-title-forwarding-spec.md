# string-temp-title-forwarding — a String call-result temp loses its title into a collection

## 1. Definition

Opened 2026-08-10. `OwnershipLeakProbe.stringElementTransferSpellings`
**SIGSEGVs (exit 139)** on branch `ownership/collections-borrow-by-default`.
**Verified regression**: the same test PASSES on pristine `main`
(`cbe40fd1`) and faults on the branch, so the collection ownership
reversal caused it.

    String s = "keep" + 7;
    ArrayList<String> a = heap ArrayList<String>();
    a.add(s.substring(0, s.count()));   // fresh copy — a #String temp
    a.add(#s);                          // surrender
    t = a.get(0).count() + a.get(1).count();   // faults

## 2. Root cause (established)

`MethodCallExpression::droppableTempClass()` explicitly returns nullptr
for `cajeta.lang.String`. That predicate gates `flagCarrier`, which is
what stashes a call result's runtime title flag into `argTitleFlags`. So
a **String call-result temp never forwards a title**, the transfer word
goes out as 0, and `ArrayList.add`'s `#=` records a BORROW of a
temporary that dies at end of statement.

Harmless while `add` declared `#T` — the transfer rode a different path.
Relaxing the formals made the plain path the only path.

`String.substring` IS declared `#String` (`String.cajeta:304`), so the
temp genuinely holds title. Nothing else owns it.

## 3. What has been tried — BOTH INSUFFICIENT

### 3.1 Static move-mask bit (COMMITTED, 30bc256e — insufficient)

Treat a call to a `#R`-declared method as a fresh owned rvalue, like the
existing `heap X()` creator and heap-array-literal arms, and set the
static `moveMask` bit.

- Fixed a minimal probe: `a.add(s.substring(0, s.count()))` then read
  slot 0 → rc=5, no fault.
- Did **NOT** fix the test. Still SIGSEGV.

### 3.2 Forward the RUNTIME flag for String (REVERTED — made it worse)

Allow String past `droppableTempClass` at the `flagCarrier` site so the
temp forwards `__cajeta_return_flag_get` instead of a static 1. Rationale:
a String's ownership lives in WRAPPER TAG BITS (bit31 rc stake, bit30
borrow view, bit29 static root — `String.cajeta:36-44`), so a static
"you own this outright" is too strong for a stake holder.

- Made it **worse**: even a SINGLE add + read of slot 0 faulted.
- Implies `substring`'s flagged return is not reaching the return-flag
  TLS, or is being clobbered before the stash reads it.

## 4. The unexplained contradiction — START HERE

Under the SAME build (§3.1, committed), two single-add probes disagree:

| probe | result |
|---|---|
| `a.add(s.substring(0, s.count()))`, read slot 0 | **rc=5, clean** |
| `a.add(s2.substring(0, 4))`, read slot 0 | **SIGSEGV** |

Differences between them, none yet ruled out:

- **4.1** computed end (`s.count()`) vs **constant** end (`4`)
- **4.2** the second probe declares TWO String locals (`s`, `s2`) with
  one unused; the first declares one
- **4.3** the receiver is the same local being read back vs a different one

Until this is explained, no fix should be believed. It is the reason both
attempts above looked like they worked on a probe and did not work on the
test.

## 5. Not yet checked

- **5.1** Whether the **scope-proven substringView downgrade**
  (`LocalVariableDeclaration.cpp:~604`) is firing here. It is documented
  as applying only to a String LOCAL initialized from `substring`/`trim`
  whose name never escapes — NOT to a direct argument — but the constant-
  index probe's behaviour is consistent with a borrow view, and the escape
  walk's notion of "escapes" was written when collections took `#T`.
  A collection store may no longer read as an escape.
- **5.2** Whether the fault is in the READ or in `ArrayList`'s GROW.
  Evidence is mixed: "adds only, no read" is clean, which points at the
  read; but a second add (any kind, lend or transfer) makes a previously
  clean single-add case fault, which points at the grow's String
  element-move take (`Expression.cpp` — "String elements are a TAKE: load
  the wrapper and NULL the slot").
- **5.3** Whether `__cajeta_return_flag_set` is emitted at all for
  `#String` returns, which §3.2's failure suggests it may not be.

## 6. Requirements

- **6.1** `a.add(s.substring(...))` stores a value the list owns; reading
  it back after any number of further adds is valid.
- **6.2** Exactly one drop — no leak (`Cajeta.liveCount()` delta 0) and no
  double free.
- **6.3** A `#R` temp handed to a READ-ONLY callee is still dropped
  exactly once. Pinned probe: `peek(make())` → 41, which BOTH attempts
  above preserved.
- **6.4** The pristine-`main` behaviour of `stringElementTransferSpellings`
  is restored: return 10 (leaked 0, t 10).

## 7. Method note

Both failed attempts were validated on a probe that did not reproduce the
test. Any candidate fix must be run against the ACTUAL test
(`OwnershipLeakProbe.stringElementTransferSpellings`) before it is
believed, not against a reduction of it — §4 exists precisely because two
reductions of the same shape disagree.
