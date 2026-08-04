# Transfer demotes the source to a borrow — spec

Status: draft · Authored 2026-08-02 · Supersedes the "moved-local borrows" framing

## 1. Definition

### 1.1 What

Transferring a local's ownership with `#` currently poisons the name: every
later read is `CAJETA_ERROR_USE_AFTER_MOVE`. That is the wrong model.

**A transfer moves the title. It does not invalidate the binding.** The source
is demoted from *owner* to *borrow* and remains a usable reference to the same
live instance.

```cajeta
Tag t = heap Tag();
map.put(#t, 42);       // the map takes the title; `t` is now a BORROW

map.get(t);            // legal — an ordinary borrow read
Tag other #= t;        // error — you cannot transfer from a borrow
```

Nothing dropped out of scope. The instance is alive and the map owns it.

### 1.2 Why the current model is wrong

The implementation treats transfer as invalidating the name, which produces
false positives on code that is provably safe:

```cajeta
Tag orig = heap Tag();
Tag owner #= orig;
orig.setValue(5);      // rejected today. `owner` is a local in the same
                       //   scope, so the instance outlives every use.
```

And it is inconsistent: an alias reaches the same object with no complaint,
because aliases are not tracked as moved.

```cajeta
Tag alias = t;
map.put(#t, 42);
map.get(alias);        // permitted today
map.get(t);            // rejected today — same object, same access
```

The alias route is the more dangerous of the two, since it survives the
transfer silently. Protecting only the name buys no safety.

### 1.3 The unification

Under demotion there is no "moved state" to check on reads, and the two
transfer errors become one:

| Attempted | Source's ownership state | Error |
|---|---|---|
| `Tag x #= alias;` | borrow (alias of a live owner) | `CAJETA_ERROR_MOVE_OF_BORROW` |
| `Tag b #= t;` after `Tag a #= t;` | borrow (demoted by the first transfer) | `CAJETA_ERROR_MOVE_OF_BORROW` |

Transferring twice *is* transferring from a borrow. `CAJETA_ERROR_USE_AFTER_MOVE`
describes a state that does not exist under this model and is retired (4.2).

### 1.4 `#=` is conditional acquisition, not a transfer operator

`#=` means *take ownership if it is on offer*. Where the source's ownership is
statically known the compiler decides; where it is not — a plain formal, whose
ownership is fixed by the call site — the store forwards whatever the caller
did. Verified:

| Store | Source | Result |
|---|---|---|
| `Tag x #= alias;` | statically a borrow | `MOVE_OF_BORROW` |
| `h.c #= t;` | statically an owner | transfers |
| `void keep(Tag v) { this.c #= v; }` | plain formal — dynamic | compiles; forwards the caller's mode under lend, transfer, and fresh construction alike |

This is why the stdlib's hand-rolled `if (Cajeta.owned(v))` branches were
redundant: `#=` at a formal already does exactly that. They were redundant,
not wrong.

### 1.5 Scope

- Replace moved-name poisoning with an ownership-state demotion on the binding.
- Reads of a demoted binding are ordinary borrow reads.
- Transfer from a demoted binding is `CAJETA_ERROR_MOVE_OF_BORROW`.
- Locals, parameters, captures, and demoted **paths** (`obj.f`, `arr[i]`).
- Retire `CAJETA_ERROR_USE_AFTER_MOVE`.
- Reconcile `MemoryModel.md` rows 536 and 543.

### 1.6 Non-goals

- **Move-while-borrowed enforcement.** Rust rejects a move while a borrow is
  live (E0505). Cajeta will not. Row 543 already assigns an alias outliving its
  source to the programmer at v1; adding the check would box the language in
  for a hazard the model has already delegated.
- The Phase 6+ lifetime tracker.
- Any collections change — downstream, separate spec.
- `CAJETA_ERROR_TRANSFER_REQUIRED`, `CAJETA_ERROR_DANGLING_LEND`, and
  definite-assignment are untouched.

### 1.7 Non-guarantee, stated plainly

A demoted binding is a borrow, and a borrow dangles once its owner drops. The
compiler will not diagnose this — the same exposure row 543 already accepts.

```cajeta
Tag orig = heap Tag();
{ Tag owner #= orig; }     // owner drops here; the instance is freed
orig.setValue(5);          // use-after-free — compiles, faults at runtime
```

---

## 2. The demotion rule

### 2.1 Requirement

A transfer sets the source binding's ownership state to *borrow*. Everything
follows from that, with no separate rules for "moved" names.

| Use of a demoted binding | Result | Because |
|---|---|---|
| identifier read (`t`) | allowed | borrows are readable |
| field-path read (`t.n`) | allowed | borrows are readable |
| method call (`t.setValue(5)`) | allowed | borrows are readable |
| plain argument (`f(t)`) | allowed | borrows are lendable |
| `#t` | `CAJETA_ERROR_MOVE_OF_BORROW` | cannot transfer from a borrow |
| `x #= t` | `CAJETA_ERROR_MOVE_OF_BORROW` | source is statically a borrow (1.4) |
| plain argument at a `#T` formal | `CAJETA_ERROR_TRANSFER_REQUIRED` | unchanged |

### 2.2 Use cases

- 2.2.1 As a developer, when I transfer a local and then call a method on it,
  the call compiles and operates on the same instance.
- 2.2.2 As a developer, when I read a field of a transferred local, the read
  compiles and yields the live instance's field.
- 2.2.3 As a developer, when I transfer a local twice, I get
  `CAJETA_ERROR_MOVE_OF_BORROW` — the same error as transferring from any other
  borrow, because that is what it is.
- 2.2.4 As a developer, when I use a transferred local as a `#=` source, I get
  the same error; the spelling differs, the violation does not.
- 2.2.5 As a developer, when I pass a transferred local to a `#T` formal
  without `#`, I get `CAJETA_ERROR_TRANSFER_REQUIRED`. The diagnostic names the
  real problem.
- 2.2.6 As a developer, when I reassign a transferred local
  (`t = heap Tag();`), it is an owner again.
- 2.2.7 As a developer, when I read a transferred local whose new owner has
  already dropped, the compiler does not stop me (1.7).
- 2.2.8 As a developer, when I store a plain formal with `#=`, the store
  forwards my caller's mode — transfer if they surrendered, borrow if they
  lent (1.4).

### 2.3 Paths

- 2.3.1 The rule applies identically to paths demoted by `markMovedPath` — a
  transferred `obj.f` or `arr[i]` may be read, not re-transferred.
- 2.3.2 Demoting `obj.f` says nothing about `obj.g`.

---

## 3. Map and container consequences

### 3.1 Requirement

No collections change. Demotion removes the reason the workarounds existed.

### 3.2 Use cases

- 3.2.1 As a developer, when I insert with `map.put(#str, #val)` and then look
  up with `map.get(str)`, the lookup compiles — `str` is a borrow of the
  instance the map owns, so an identity-hashed class key resolves correctly.
- 3.2.2 As a developer, when I replace a value, I call
  `map.update(str, #anotherVal)` — key borrowed, value transferred. (The
  `put`/`update` split is specified downstream; this spec makes the borrowed
  key at that call site legal.)
- 3.2.3 As a developer, when I transfer the same key twice, I get
  `CAJETA_ERROR_MOVE_OF_BORROW`. The diagnostic speaks about ownership, not
  about maps or duplicate keys.
- 3.2.4 As a maintainer, migrating a lend call site to an owning container is a
  one-character change — add `#` at the mutator, leave every lookup alone.

### 3.3 Effect on in-flight work

- 3.3.1 `uniform-transfer-semantics` 4.2.1 reverts to the mechanical migration
  its plan originally estimated (done — 90a9f394).
- 3.3.2 `uniform-transfer-semantics` 4.2.4 (own vs borrow keys) resolves toward
  owning keys; the objection was that a surrendered class key became
  unreachable, which 3.2.1 removes.
- 3.3.3 `HashMapTests.resizeClearsTombstones` is a test defect, not evidence
  against the design: `m.remove(k)` then `m.containsKey(k)` is a
  use-after-free the model assigns to the developer (1.7).

---

## 4. Diagnostics

### 4.1 The rule, stated once

> **You cannot transfer ownership more than once, or from a borrow.**

- 4.1.1 `CAJETA_ERROR_MOVE_OF_BORROW` keeps its specifics — it names the actual
  owner (*"ownership belongs to `t`"*), which is more useful than the general
  rule alone. Where the new owner is a container the message names what it can.
- 4.1.2 No diagnostic describes the violation in terms of the container
  operation that triggered it. "You cannot insert the same key twice" reads as
  a restriction on duplicate keys, which is not the rule and is not true.

### 4.2 Retiring `CAJETA_ERROR_USE_AFTER_MOVE`

- 4.2.1 The identifier is removed. It names a state that does not exist under
  demotion, and every site that raised it either becomes legal (reads) or
  raises `MOVE_OF_BORROW` (transfers).
- 4.2.2 `Identifier.cpp:51` and `DotExpression.cpp:236` drop their read
  rejections outright rather than narrowing them.
- 4.2.3 `Scope::isMoved` is renamed to express ownership state rather than a
  poison flag.

---

## 5. Documentation

- 5.1.1 `MemoryModel.md` row 536 states the demotion rule: a transfer demotes
  the source to a borrow; reads are legal and carry the row 543 exposure.
- 5.1.2 Rows 536 and 543 no longer describe the same hazard with opposite
  outcomes.
- 5.1.3 The ownership documentation carries the worked example (1.1) with its
  rationale: the title moved, the binding did not die.
- 5.1.4 The `language-ownership` skill matches the compiler — a transferred
  binding is a readable borrow, not a corpse.
- 5.1.5 `#=` is documented as conditional acquisition (1.4), not as a transfer
  operator.

---

## 6. Risks

| | Risk | Response |
|---|---|---|
| 6.1 | Relaxing a check permits code that faults at runtime | Already the v1 position for aliases (543); this makes it consistent rather than new |
| 6.2 | `CAJETA_WARN_LAST_USE_TRANSFER` may fire differently once reads are legal | Verify; advisory, never fails a build |
| 6.3 | Tests may assert the current rejection | They assert the bug. Enumerate during implementation |
| 6.4 | Retiring an error identifier is user-visible | It cannot fire under the new model; leaving it would be dead surface |
| 6.5 | `MOVE_OF_BORROW` must name a useful owner when the new owner is a container | Verify the message degrades gracefully; a container owner may not have a source-level name |

## 7. Open questions

- 7.1 Should a read of a demoted binding emit an advisory **warning**? It is
  legal but is the shape that dangles. Recommendation: no — 3.2.1 makes this
  the ordinary spelling for map lookups, and warning on ordinary code trains
  people to ignore warnings.
- 7.2 Does demotion extend to a local moved by a **closure capture**
  (`Expression.cpp:4189`)? Recommendation: yes, for consistency with 1.6.
