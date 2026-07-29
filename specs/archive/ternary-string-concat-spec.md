# Spec: ternary (`?:`) result in string concatenation

## 1. Definition

### 1.1 Purpose
A conditional expression `cond ? a : b` whose result is a `String` (or other class
reference) must be usable as an operand of string concatenation `+` and contribute its
actual value, not an empty/garbage string.

### 1.2 Problem
`"x" + (cond ? "a" : "b")` renders the ternary operand as **empty** — the produced
string is `"x"` instead of `"xa"`/`"xb"`. Root cause: the ternary produces an r-value
(a `phi` of two `String` pointers), but the l-value loader `loadIfLValue` has no
carve-out for `BooleanSwitchExpression`, so the class-reference catch-all loads *through*
the String pointer (reading the struct's first word, the vtable) instead of using the
pointer itself. The same family as the New/Spawn/MethodCall/concat carve-outs that
already exist. (A known issue; documented as the "ternary in string concat renders
empty" workaround.)

### 1.3 Scope
`loadIfLValue` (`BinaryOpExpression.cpp`) — recognize a `BooleanSwitchExpression` result
as an r-value. Applies wherever a ternary feeds an operand that load-checks (string
concat, assignment RHS, call arguments).

### 1.4 Non-goals
- 1.4.1 The integer-result ternary codegen fix (already shipped — `ternary-int-codegen`).
- 1.4.2 General type unification of ternary arms.

## 2. Feature: ternary result as an r-value operand

### 2.1 Use cases
- 2.1.1 As a developer, `"x" + (cond ? "a" : "b")` yields `"xa"` when `cond` is true and
  `"xb"` otherwise (the ternary operand contributes its real bytes).
- 2.1.2 As a developer, the ternary may be on either side: `(cond ? "a" : "b") + "x"`.
- 2.1.3 As a developer, a ternary whose arms are non-literal `String` expressions
  (a field, a method result, a concat) concatenates correctly.
- 2.1.4 As a developer, assigning a `String` ternary to a variable / passing it as a
  `String` argument carries the real value (no empty/garbage).
- 2.1.5 As an existing program, non-String ternaries and existing concatenations are
  unchanged.

## 3. Acceptance themes
- 3.1 Correctness: 2.1.* produce the exact expected strings.
- 3.2 Non-regression: existing string + ternary tests stay green.
