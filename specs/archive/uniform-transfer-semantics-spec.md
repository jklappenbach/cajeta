# Uniform transfer semantics — a field behaves like a variable (draft)

Approved in principle by Julian 2026-08-02: *"I would expect a field to be the
same as a variable. So `v #= obj.field.v` should take ownership of the instance
referenced by v. If that requires a change to container semantics, then fix
it."* — and, on being shown the cost, *"I want the uniform version."*

Not started. This is an API-breaking release, and the diagnostic half and the
container half must land together or the stdlib stops compiling.

## 1. Definition

Today `#=` means two different things depending on the source:

| Source | `x #= src` | `x #= #src` |
|---|---|---|
| local / parameter | transfers the title; a borrow source is a COMPILE error | redundant — rejected as of `409d8ffe` |
| field / element | **asserts** a title; throws at RUNTIME when the slot holds a borrow | forwards the slot's mode VERBATIM ("fused claim") |

The asymmetry is the defect. A field should behave like a variable:
`v #= obj.field.v` takes ownership, full stop. Then `#` on the right of `#=`
is redundant in every position and `x #= #y` becomes a blanket error.

## 2. Requirements

- 2.1 `x #= field` / `x #= elem` transfers the title, exactly as from a local.
- 2.2 `x #= #y` is `CAJETA_ERROR_DOUBLE_TRANSFER` for EVERY source shape. The
  bare-identifier case already is; widen the predicate
  (`cajetaSharpOperandIsBareIdentifier` in `Expression.cpp`) to accept any
  source, and drop it from the three call sites' conditions.
- 2.3 Containers OWN their elements. A container may not hold a borrow, because
  a borrow it later hands out cannot satisfy 2.1. This is already the stated
  rule elsewhere — `language-ownership`: *"Containers take elements by transfer
  (`list.add(#g)`)"* — so this makes the implementation match the documented
  contract rather than inventing one.
- 2.4 No silent behaviour change at a lend call site: `m.put(k, v)` with a plain
  `v` must become a compile error naming the fix (`m.put(k, #v)`), not a quiet
  copy or a runtime throw.

## 3. Scope (measured 2026-08-02)

- **20** fused claims (`#= #`) in `runtime/src/cajeta/collection/` —
  ArrayList, BPlusTree, HashMap, Heap, LinkedList (+1 in a LinkedListNode
  comment).
- **10** container methods whose element formal becomes `#T` — `add`, `addTail`,
  `addHead`, `put`, `push`, `offer`, `insert`, `set`.
- **42** lending call sites inside the stdlib itself.
- Downstream: `dev.cajeta.ml` (0.4.0, released), `dev.cajeta.xgboost`, the tour,
  samples, and the test suite.

## 4. Why it cannot be staged

The blanket error (2.2) makes every existing fused claim a compile error, and
the fused claims exist because containers hold borrows (2.3). Landing either
half alone breaks the build. One commit, one sweep, one release.

## 5. Acceptance

- 5.1 `MemberBitmapTests.hashMapRemoveFlaggedContract` — the flagged-remove
  contract is the pin that fails first. Under 2.3 the BORROWED half of that test
  becomes invalid by construction (a map cannot hold a borrow); the test must be
  rewritten to the new contract deliberately, not deleted. Its
  `PROBE_removeOwnedOnly` / `PROBE_removeBorrowedOnly` siblings isolate the two
  halves.
- 5.2 Full `./cajeta_tests.sh` sweep. Title-tracking has a history of
  sweep-only regressions, and this touches every container.
- 5.3 `dev.cajeta.ml` and `dev.cajeta.xgboost` rebuild and pass against the new
  toolchain BEFORE the release is tagged — they are the migration's real test.
- 5.4 `language-ownership` updated: the `#=` rule stated once, without the
  field/local exception, and the container contract stated as enforced rather
  than conventional.

## 6. Notes for whoever picks this up

- Three double-sharp tests passed at one point for the WRONG reason — the
  stdlib's own double sharp threw during compile and masked the test's
  behaviour. Rewriting the stdlib is what exposed it. Do not trust a green run
  in this area without checking WHERE the error came from.
- `x #= field` already throws for a borrowed slot (runtime, `value=0x3`), so
  2.1 is less of a change than it sounds — the work is 2.3, making sure no slot
  ever holds a borrow in the first place.
