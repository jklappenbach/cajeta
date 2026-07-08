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
