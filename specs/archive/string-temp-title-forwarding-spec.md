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

## 2. Root cause (FINAL — three cooperating defects, 2026-08-10)

The String ownership protocol is NOT the class-title protocol: String
formals get no drop entries (`Method::emitFormalDropEntries` excludes
String by design — "Strings transfer by share/resolve, never by entry"),
String slots ALWAYS own their wrappers (a plain store dual-role-RESOLVES
a fresh wrapper; only a real transfer forwards the wrapper itself), and
anonymous String temps are reclaimed CALLER-side after the consuming call
(the element-ownership 3.4.3 block). The reversal broke three points of
that protocol at once:

- **2.1 The instantiated element store ignored the transfer word.**
  `ArrayList<String>::add`'s `this.data[i] #= v` emitted
  `__cajeta_string_elem_store(slot, wrapper, takes=1)` — a STATIC 1.
  The word argument arrived in `add` and was never read (verified in
  the emitted IR). Every add took the caller's wrapper, whatever the
  caller did.
- **2.2 The 3.4.3 reclaim then freed what the slot had adopted.**
  A fresh String temp handed to a plain formal is reclaimed after the
  call (`__cajeta_string_drop`) on the old-world premise that a plain
  formal can never retain it. With 2.1 moving the wrapper into the slot,
  the reclaim freed the slot's resident wrapper → the read SIGSEGVed.
- **2.3 The first fix (30bc256e) fed 2.2 instead of fixing it.** Setting
  a static transfer bit for `#R` call results told the callee to keep
  the wrapper while the caller-side reclaim STILL freed it. The named
  lend shapes only survived by luck: name-bound concats are arena-routed
  and `__cajeta_string_drop` no-ops on them.

### 2.4 The §4 "contradiction" was a build artifact

Re-measured at the tip with a 14-variant matrix (window whole/sub ×
end computed/constant × extra locals × which slot read): EVERY variant
reading the temp's slot faulted; reading the surrendered `#s` slot was
clean. The recorded "probe 1 clean / probe 2 faults under one build"
anomaly did not reproduce and is attributed to a stale build (it has a
precedent this cycle: probing a stdlib edit without rebuilding the
embedded stdlib).

## 3. Resolution (landed with this spec's closure)

- **3.1 Mode-carrying moves of entry-less formals**
  (`Expression.cpp`, MoveExpression identifier arm): a plain formal with
  no drop entry captures `runtimeTitleFlag` from the enclosing method's
  ABI transfer word — the store-form twin of the existing 6.2.1 call-arg
  word-bit forward.
- **3.2 String element stores honor the runtime mode**
  (`BinaryOpExpression.cpp`, both String element branches): a
  MoveExpression RHS carrying a runtime flag branches at runtime —
  bit 1 → `set_owned` (take the wrapper), bit 0 → `set_alias` /
  `takes=0` (resolve a copy). Mirrors the field-store `maskWord` branch
  that already existed.
- **3.3 String temps never send a transfer bit**
  (`MethodCallExpression.cpp`, the `ownRet` moveMask arm now excludes
  `cajeta.lang.String`): a fresh String temp rides bit 0, the callee
  resolves its own wrapper, and the 3.4.3 reclaim frees the temp —
  correctly, and without reintroducing the ignore-case leak that the
  reclaim exists to prevent (a bit-set-but-unstored temp would leak:
  String formals have no entries to reclaim it).
- **3.4 The `#R` return contract is enforced at the return**
  (`Statement.cpp`, mode-carrying-claim §5.4): `return #x` in a
  `#R`-declared method with a RUNTIME title flag of 0 panics
  `CAJETA_PANIC_TITLE_MISS` instead of forging a title. `return #= x`
  (mode-carrying) is exempt by design. This is what restores the
  `secondExtractionPanics` pins: `operator#[]`'s body claim now forwards
  a borrowed slot's 0 bit, and the contract check moved to the return.

## 4. Requirements (all verified against the ACTUAL tests)

- **4.1** `a.add(s.substring(...))` stores a value the list owns; reading
  it back after any number of further adds is valid. ✔
- **4.2** Exactly one drop — no leak (`Cajeta.liveCount()` delta 0) and no
  double free. ✔ (`stringElementTransferSpellings` returns 10.)
- **4.3** A `#R` temp handed to a READ-ONLY callee is still dropped
  exactly once (`peek(make())` → 41). ✔
- **4.4** A LENT named String survives the list and the list's teardown
  (slot owns a resolved copy). ✔ (`stringElementModeMatrix` returns 20.)
- **4.5** `operator#[]` on a borrowed or already-extracted slot still
  panics TITLE_MISS (`secondExtractionPanics` × 2). ✔

## 5. Method note

Both early failed attempts were validated on probes that did not
reproduce the test; the resolution above was verified against
`OwnershipLeakProbe.stringElementTransferSpellings` itself, the full
14-variant matrix, and the consolidated `stringElementModeMatrix` pin
that replaced it.
