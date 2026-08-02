# Moved-local borrows — spec

Status: draft · Authored 2026-08-02

## 1. Definition

### 1.1 What

After a local's ownership is transferred with `#`, reading that local is
currently a compile error. This spec makes **borrow-reads legal** and keeps
**re-transfer illegal**.

```cajeta
Tag orig = heap Tag();
Tag owner #= orig;

orig.setValue(5);      // 1.1.1 — today: USE_AFTER_MOVE. Under this spec: legal.
Tag other #= orig;     // 1.1.2 — today and under this spec: an error.
```

Both names denote the same instance. Only ownership moved.

### 1.2 Why

`MemoryModel.md`'s guarantee table already contains the answer, split across
two rows that disagree:

| Row | Hazard | Current mitigation |
|---|---|---|
| 536 | use-after-move | prevented — per-variable moved-state tracking |
| 543 | use-after-free of an aliased field whose source dropped first | **programmer responsibility at v1** (Phase 6+ lifetime tracker) |

The same hazard is prevented when reached through the moved name and permitted
when reached through an alias:

```cajeta
Tag alias = t;         // alias of the same instance
map.put(#t, 42);
map.get(alias);        // permitted (543)
map.get(t);            // rejected (536)
```

The alias route is the more dangerous of the two — it survives the transfer
silently. Protecting only the name is not defensible on safety grounds, and it
produces false positives on code that is provably safe (1.1.1, where `owner`
is a local in the same scope).

Row 543 is the language's stated v1 position. This spec brings 536 into line
with it.

### 1.3 What the current check actually does

`Scope::isMoved()` is a flat name-set lookup walking parent scopes.
`IdentifierExpression::generateCode` (`Identifier.cpp:51`) rejects any read on
a hit; `DotExpression.cpp:236` does the same for paths. There is no
classification of the use and no lifetime reasoning. The behaviour is a gap in
the check's granularity, not a design position that was argued for.

### 1.4 Scope

In scope:
- Splitting the moved-state check by **kind of use** (borrow vs transfer).
- Locals, parameters, captures, and moved **paths** (`obj.f`, `arr[i]`).
- Reconciling `MemoryModel.md` rows 536 and 543.

### 1.5 Non-goals

- **Move-while-borrowed enforcement.** Rust rejects a move while a borrow is
  live (E0505). Cajeta will not. Per row 543, an alias outliving its source is
  the programmer's responsibility at v1. Adding the check would box in the
  language for a hazard the model already assigns to the developer.
- The Phase 6+ lifetime tracker.
- Any collections change. Those sit downstream and get their own spec.
- `CAJETA_ERROR_TRANSFER_REQUIRED`, `CAJETA_ERROR_DANGLING_LEND`, and
  definite-assignment are untouched.

### 1.6 Non-guarantee, stated plainly

A borrow of a moved-from local dangles once the new owner drops it. The
compiler will not diagnose this. It is the same exposure row 543 already
accepts for aliases, now reachable through one more spelling.

```cajeta
Tag orig = heap Tag();
{ Tag owner #= orig; }     // owner drops here; the instance is freed
orig.setValue(5);          // use-after-free — compiles, faults at runtime
```

---

## 2. The read rule

### 2.1 Requirement

A local in moved state permits every use except one that would transfer it
again.

| Use of a moved-from local | Result |
|---|---|
| identifier read (`orig`) | allowed |
| field-path read (`orig.n`) | allowed |
| method call (`orig.setValue(5)`) | allowed |
| plain argument (`f(orig)`) | allowed |
| explicit re-transfer (`#orig`) | `CAJETA_ERROR_USE_AFTER_MOVE` |
| implicit re-transfer (`x #= orig`) | `CAJETA_ERROR_USE_AFTER_MOVE` |
| plain argument at a `#T` formal | `CAJETA_ERROR_TRANSFER_REQUIRED` (unchanged) |

### 2.2 Use cases

- 2.2.1 As a developer, when I transfer a local to another local and then call
  a method on the original, the call compiles and operates on the same
  instance.
- 2.2.2 As a developer, when I read a field of a moved-from local, the read
  compiles and yields the live instance's field.
- 2.2.3 As a developer, when I transfer a local and then attempt to transfer it
  again with `#`, I get `CAJETA_ERROR_USE_AFTER_MOVE` naming the identifier.
- 2.2.4 As a developer, when I transfer a local and then use it as an implicit
  move source (`x #= orig`), I get the same error — the spelling differs, the
  violation does not.
- 2.2.5 As a developer, when I pass a moved-from local to a `#T` formal without
  `#`, I get `CAJETA_ERROR_TRANSFER_REQUIRED`, not a move error. The diagnostic
  names the real problem.
- 2.2.6 As a developer, when I reassign a moved-from local
  (`orig = heap Tag();`), it leaves moved state and behaves as a fresh binding.
- 2.2.7 As a developer, when I read a moved-from local whose new owner has
  already dropped, the compiler does not stop me (1.6).

### 2.3 Paths

- 2.3.1 The rule applies identically to moved paths recorded by
  `markMovedPath` — a transferred `obj.f` or `arr[i]` may be read, not
  re-transferred.
- 2.3.2 Reading a *sibling* path of a moved path is unaffected: moving `obj.f`
  says nothing about `obj.g`.

---

## 3. Map and container consequences

### 3.1 Requirement

Nothing in the collections changes. The relaxation removes the reason the
workarounds existed.

### 3.2 Use cases

- 3.2.1 As a developer, when I insert with `map.put(#str, #val)` and then look
  up with `map.get(str)`, the lookup compiles. `str` denotes the instance the
  map now owns, so an identity-hashed class key resolves correctly.
- 3.2.2 As a developer, when I want to replace a value, I call
  `map.update(str, #anotherVal)` — the key is borrowed, the value transferred.
  (The `put`/`update` split is specified separately; this spec only makes the
  borrowed key at that call site legal.)
- 3.2.3 As a developer, when I transfer the same key twice
  (`map.put(#str, …)` then `map.put(#str, …)`), I get
  `CAJETA_ERROR_USE_AFTER_MOVE`. Insertion consumes; you cannot insert the same
  key twice.
- 3.2.4 As a maintainer, migrating a lend call site to an owning container is a
  one-character change — add `#` at the mutator, leave every lookup alone. No
  alias, no handle, no fresh-key reconstruction.

### 3.3 Effect on in-flight work

- 3.3.1 `uniform-transfer-semantics` 4.2.1 reverts to the mechanical migration
  its plan originally estimated. The 32-site alias rewrite was working around
  the false positive this spec removes.
- 3.3.2 `uniform-transfer-semantics` 4.2.4 (own vs borrow keys) resolves toward
  owning keys. The objection was that a surrendered class key became
  unreachable; 3.2.1 removes it.
- 3.3.3 `HashMapTests.resizeClearsTombstones` stays broken and is rewritten as
  a test defect. `m.remove(k)` then `m.containsKey(k)` is a use-after-free the
  model assigns to the developer (1.6) — the test is wrong, not the language.
- 3.3.4 `OwnedKeyLookupTests` is rewritten: its subject was which lookup routes
  survive a surrender, and under this spec the direct route survives.

---

## 4. Documentation

### 4.1 Requirement

`MemoryModel.md` must state one rule, not two that disagree.

### 4.2 Use cases

- 4.2.1 As a reader of `MemoryModel.md`, row 536 states that use-after-move is
  prevented for *re-transfer*, and that reads of a moved-from binding are
  permitted and carry the row 543 exposure.
- 4.2.2 As a reader, the two rows no longer describe the same hazard with
  opposite outcomes.
- 4.2.3 As a reader of the ownership documentation, the worked example (1.1)
  appears with its rationale: both names denote one instance; only ownership
  moved.
- 4.2.4 As a developer reading the `language-ownership` skill, the guidance
  matches the compiler — a moved-from local is readable, not re-transferable.

---

## 5. Risks

| | Risk | Response |
|---|---|---|
| 5.1 | Relaxing a safety check permits code that faults at runtime | Already the v1 position for aliases (543); this makes it consistent rather than new |
| 5.2 | `CAJETA_WARN_LAST_USE_TRANSFER` may fire differently once reads after a move are legal | Verify; the warning is advisory and never fails a build |
| 5.3 | Existing tests may depend on the rejection | Expected to be few — the rejection produces false positives, so tests asserting it are asserting the bug. Enumerate during implementation |
| 5.4 | The relaxation could mask a genuine double-transfer through an unclassified path | 2.1 enumerates the transfer spellings; any new one must be classified explicitly |

## 6. Open questions

- 6.1 Should a read of a moved-from local emit an advisory **warning**? It is
  legal but is the shape that dangles. A warning would flag it without boxing
  the language in. Recommendation: no warning in v1 — 3.2.1 makes this the
  ordinary spelling for map lookups, and warning on ordinary code trains people
  to ignore warnings.
- 6.2 Does the rule extend to a local moved by a **closure capture**
  (`Expression.cpp:4189` marks captures moved)? A captured-and-moved local read
  after the closure is constructed is the same shape, but the closure's
  lifetime is less obvious than a sibling local's. Recommendation: include it,
  for consistency with 1.5's stance.
