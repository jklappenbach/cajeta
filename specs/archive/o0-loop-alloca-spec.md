# Spec: loop-body local allocas must not grow the native stack

## 1. Definition

### 1.1 Purpose
A local variable declared inside a loop body must reserve its stack slot **once**, not
once per iteration. A function with a hot loop containing local declarations must run in
O(1) native stack regardless of iteration count, at every optimization level (including
`-O0` / the JIT).

### 1.2 Problem
`StackField`/`HeapField::getOrCreateAllocation` emit their `alloca` at the **current
builder insertion point**. For a local declared inside a loop body, that point is the
loop block — so the `alloca` instruction lives in the loop. An LLVM `alloca` allocates on
the current stack frame **each time it executes**; an alloca in a loop therefore grows the
native stack every iteration and never reclaims until the function returns. At `-O3` SROA/
mem2reg promotes the slot away and hides the bug, but at `-O0` (the JIT / debug builds) a
loop running O(n log n) / O(n^2) iterations (e.g. `Sort.sort`'s comparator loop, or any
temp-heavy loop) overflows the native stack and SIGSEGVs at the guard page. The DropEntry
alloca in the same code path already uses the correct entry-block idiom; the field slot
does not.

### 1.3 Scope
- `src/cajeta/field/StackField.cpp`, `HeapField.cpp` (and `ParameterField.cpp` for
  consistency) — `getOrCreateAllocation` must emit the slot `alloca` in the function's
  **entry block**, not at the current insertion point. The initializer evaluation and the
  store stay at the current point (the per-iteration value computation must NOT move).
- Any other per-local/per-value `CreateAlloca` reachable from a loop body
  (`LocalVariableDeclaration` `stack X()` body allocas) if the tests show they leak too.

### 1.4 Non-goals
- 1.4.1 General scope-frame reuse / stack-slot coloring at `-O0` (an optimization).
- 1.4.2 Changing release (`-O3`) behavior — it already eliminates these slots; the fix
  must not regress it.
- 1.4.3 `alloca`s that are intentionally dynamic / loop-variant in size.

## 2. Feature: entry-block local slots

### 2.1 Use cases
- 2.1.1 As a developer, a loop `while (i < N) { int64 t = ...; ... }` runs with constant
  native-stack usage for any N (no overflow), at `-O0`.
- 2.1.2 As a developer, the same for a value-type local, a reference (`heap`) local, and
  multiple locals declared in the loop body.
- 2.1.3 As a developer, `Sort.sort` (and other comparator/temp-heavy loops) runs at
  n=50000+ under the JIT without a native-stack overflow.
- 2.1.4 As a developer, the local's value semantics are unchanged: each iteration still
  re-initializes the slot (the initializer/store stay in the loop), so reads see the
  current iteration's value, not a stale one.
- 2.1.5 As an existing program, every existing test (value types, drops, scope-frame
  elision, nested scopes) behaves identically; release output is unaffected.

## 3. Acceptance themes
- 3.1 Correctness: a million-iteration loop with body locals returns the right value with
  no crash at `-O0`; the loop-body `alloca`(s) appear in the **entry block** in the IR.
- 3.2 Non-regression: the full existing suite (esp. value-type, drop, scope-frame) stays
  green; release behavior/perf unchanged.
