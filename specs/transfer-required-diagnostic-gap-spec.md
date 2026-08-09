# transfer-required-diagnostic-gap — a missing `#` reached RUNTIME where §2.4 requires a compile error

## 1. Definition

Opened 2026-08-09 from the `Placement.tierNote` hotfix (cajeta main
`60d9404d`), which was:

```cajeta
-        Placement.tiered.add(k);
+        Placement.tiered.add(#k);
```

`uniform-transfer-semantics` **§2.4** states the contract this violates:

> No silent behaviour change at a lend call site: `m.put(k, v)` with a
> plain `v` must become a **compile error naming the fix**
> (`m.put(k, #v)`), not a quiet copy or a runtime throw.

So the missing `#` should have been rejected at the call site. It was
not — it compiled, shipped to main, and had to be hotfixed.

## 2. Why this is a gap and not an un-migrated signature

Both halves of the mechanism are already in place, which is what makes
the miss interesting:

- `HashSet.add` **is** migrated: `public void add(#T value)`
  (`runtime/src/cajeta/collection/HashSet.cajeta:105`).
- The diagnostic **is** implemented:
  `CAJETA_ERROR_TRANSFER_REQUIRED` is raised from
  `MethodCallExpression.cpp:10021` (and `BinaryOpExpression.cpp:1120`),
  and the uniform-transfer plan pins it for `list.add(v)`
  (its 2.1.1 / `OwnershipLeakProbe`).

The signature was right and the check exists, yet this call passed. The
one structural difference from the pinned cases is the RECEIVER SHAPE:
the pins use a local (`list.add(v)`), while the miss is a **static
field** (`Placement.tiered.add(k)`). That is the first hypothesis to
test, not a conclusion.

## 3. The open question

Should the borrow checker have caught this at the call site — and if
so, why didn't it? Concretely:

- **3.1** Does `CAJETA_ERROR_TRANSFER_REQUIRED` fire for a `#T` formal
  when the receiver is a static field? An instance field? An array
  element? A method-call result? The pins only cover a local receiver.
- **3.2** If receiver shape is the discriminator, the check is keyed on
  something that has nothing to do with the argument's ownership, which
  would make every non-local receiver a silent hole.
- **3.3** Is the argument's shape also a factor — `k` here is a plain
  local, the same shape the pinned `list.add(v)` case uses, which argues
  the argument side is fine and the receiver side is not.

## 4. Why it matters beyond one hotfix

This is the third ownership-transfer defect this cycle that surfaced at
runtime rather than compile time (the others:
`hashmap-string-value-drop`, `owned-array-element-move`). §2.4 exists
precisely so these are caught at the call site; a hole in it means the
uniform-transfer migration's safety story is weaker than the spec
claims, and every container call with a non-local receiver is
unverified.

## 5. Requirements (proposed, pending the answer to §3)

- **5.1** A `#T` formal passed a plain owned source is
  `CAJETA_ERROR_TRANSFER_REQUIRED` regardless of RECEIVER shape —
  local, static field, instance field, element, or call result.
- **5.2** Regression pins for each receiver shape, not just the local
  one.

## 6. Reproduction

`Placement.tiered.add(k)` without the `#`, i.e. cajeta main at
`60d9404d^`, against `HashSet.add(#T)`. A smaller repro should be
written as step one: a static-field `HashSet<T>` and a plain local
argument.

## 7. Measured 2026-08-09 — both hypotheses DISPROVEN

Built against cajeta main + the collection ownership reversal. Two
probes, each an explicit `#T` formal (`Sink.take(#Bx)`):

- **7.1 Receiver shape is NOT the discriminator.** §3.1/§3.2 proposed
  that a non-local receiver was the hole. A **static field** receiver
  (`R.stat.take(k)`, `k` a plain owned local) is REJECTED correctly with
  `CAJETA_ERROR_TRANSFER_REQUIRED`. The §3.2 worry — that every non-local
  receiver is a silent hole — is unfounded.
- **7.2 An unprovable-ownership source is NOT a hole either.** Passing a
  BORROWED formal onward to a requiring formal (`fwd(Bx b)` calling
  `R.stat.take(b)`) is also REJECTED. The `!callerOwns → continue` path
  does not leak in this shape.

So the gap does not reproduce in either hypothesized shape. Either
intervening work closed it, or `Placement.tiered.add(k)` had a third
shape not yet identified. **§5.1/§5.2 remain worth pinning**, but the
receiver-shape framing in §3 is withdrawn.

### 7.3 A real defect found while probing

The §7.2 rejection advised "write `#b`" — which
`CAJETA_ERROR_TRANSFER_OF_BORROW` then rejects for the same underlying
reason, sending the reader in a circle. Fixed: when the source is
provably a borrowed formal, the diagnostic now says so and advises
declaring the parameter `#b` (taking ownership from the caller) or
passing a value the frame owns, rather than suggesting a spelling that
cannot work.
