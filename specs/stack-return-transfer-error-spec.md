# `return stack X()` under a `#X` return type — diagnostic — spec (draft)

Origin: docs-refactor 15.4 (unit-11 demo work, 2026-07-03).

## 1. Definition

A method declared with a transfer return type (`#X f()`) that executes
`return stack X(...)` compiles with **no diagnostic** and returns clobbered
memory at runtime: the stack-allocated value dies at scope exit, but the `#`
return promises the caller an owned, outliving value. This is silent
undefined behavior reachable from safe-looking source.

The compiler already ships the adjacent analysis: the
`[heap-optional-return]` warning recommends non-`#` + `stack` sret for
Optional-returning methods. This spec extends that machinery into a
hard diagnostic for every `return stack` under a `#` return type.

## 2. Features

### 2.1 Compile-time error on stack-return under transfer
`return stack X(...)` (direct form, and a local proven stack-allocated that
is returned under `#`) is a compile error, `CAJETA_ERROR_STACK_RETURN_ESCAPES`
(name TBD at plan time), with a fix-it suggesting `heap` allocation or a
non-`#` sret signature.

Use cases:
1. As a developer, when I write `#X f() { return stack X(1); }`, then the
   compile fails at the return site naming both the allocation and the `#`
   return — instead of garbage at runtime.
2. As a stdlib author, when a refactor flips a return type to `#X` and an
   old `return stack` path survives, then CI catches it at compile time.
3. As the tour/docs snippet checker, when an example demonstrates the
   pattern, then the checker rejects it (guides teach `heap` or sret).

### 2.2 Escape-analysis integration
The diagnostic reuses the existing return-escape classification (borrow
downgrade / `[heap-optional-return]` walk); no new analysis pass. Locals
whose allocation site is ambiguous (both arms of a conditional) err on the
conservative side (diagnose).

## 3. Non-goals
Automatic promotion of `stack` to `heap` at the return site (silent
allocation changes are worse than the error); lifetime extension.

## 4. Status — SHIPPED 2026-07-31

Implemented in `ReturnStatement::generateCode`
(`src/cajeta/asn/Statement.cpp`), gated on `Method::isReturnsOwnership()`.
`CAJETA_ERROR_STACK_RETURN_ESCAPES` is the id, as proposed in §2.1.

**Covered** (both were silent before — the direct form reached codegen and
died in the LLVM IR verifier with "return type does not match operand type";
the local form compiled clean and returned clobbered memory):

- `#X f() { return stack X(...); }` — direct construction, and the stack
  aggregate-initializer form (`Method::exprIsStackConstruction`).
- `#X f() { X c = stack X(...); return #c; }` — named local. The declaration
  site records the storage class on the field (`Field::setStackInstance`,
  set where `initIsStackAlloc` is already computed in
  `LocalVariableDeclaration`); the return unwraps `MoveExpression` to reach
  the identifier.

**Deliberately untouched** — the check fires only when the return type is
`#`, so non-`#` by-value returns (`@ValueType`, builtin by-value aggregates
like `Vector`/`Matrix`, NRVO sret) are unaffected. Verified: a non-`#`
`return stack Vec2(2, 4)` still compiles and runs. Lambdas cannot trip it
(their `(T) -> R` type syntax carries no `#`, so `isReturnsOwnership()` is
false). A stdlib scan found no site that the new error would newly reject.

**Known limitation** — the local form tracks only a direct
`X c = stack X(...)` declaration. Laundering through another local
(`X c = stack X(); X d = c; return #d;`) is not diagnosed; §2.2's
conditional-arm conservatism is likewise not implemented. Both are
false-negative gaps, never false positives.

Tests: `SignatureAbiTests.stackConstructionReturnedUnderTransferRejected`,
`stackLocalReturnedUnderTransferRejected`, and
`heapReturnUnderTransferStillCompiles` (the must-keep-working shape).
