# Spec: ternary (`?:`) integer-result codegen soundness

## 1. Definition

### 1.1 Purpose
A conditional expression `cond ? a : b` (the `BooleanSwitchExpression`) used as an
**integer-valued** expression must generate valid LLVM IR and produce the correct
value for all well-typed operand shapes — in particular when the two arms have
different integer widths or when one arm is a non-trivial arithmetic expression.

### 1.2 Problem
Assigning a ternary to an integer local where the condition is a boolean and the arms
mix an arithmetic expression with an integer literal —
`int32 x = boolCond ? a * 2 : 128;` — emits a **malformed `ICmp`** (operands of
different types), tripping the LLVM verifier assertion
`ICmpInst::AssertOK: Both operands to ICmp instruction are not of the same type`
at codegen. Surfaced authoring `StringBuilder.grow` during the StringBuilder-SSO work;
worked around there with an `if/else`. This is a real codegen defect, distinct from the
known "ternary in a string concatenation renders empty" issue.

### 1.3 Scope
The integer-result ternary path in `BooleanSwitchExpression::generateCode` (condition
lowering to `i1`, arm evaluation, width reconciliation, and the result `phi`), plus any
contributing path (e.g. the boolean-field condition load, or an arm's
overflow-checked arithmetic) that injects the mismatched comparison.

### 1.4 Non-goals
- 1.4.1 General type promotion / least-common-ancestor unification of arbitrary arm
  types (the existing codegen-time coercion is retained; only the unsound IR is fixed).
- 1.4.2 The separate "ternary in string concatenation renders empty" defect.
- 1.4.3 Float/reference-typed ternary arms beyond confirming they remain unaffected.

## 2. Feature: sound integer ternary

### 2.1 Use cases
- 2.1.1 As a developer, when I write `int32 x = cond ? a * 2 : 128;` (boolean `cond`,
  `a` an `int32` field/local), then it compiles to valid IR and `x` equals `a*2` when
  `cond` is true and `128` otherwise.
- 2.1.2 As a developer, the same holds when the condition is a **boolean class field**
  (`this.flag`), a boolean local, and a comparison expression (`a > b`).
- 2.1.3 As a developer, the same holds when the arms have **different integer widths**
  (e.g. `int64 ? : ` with an `int32` arm, and an `int32` result with an `int8` arm) —
  the result is the declared/expected width with the correct value.
- 2.1.4 As a developer, an arm that is itself a non-trivial arithmetic expression
  (multiply, add) under the default overflow-checking mode does not corrupt the
  generated comparison.
- 2.1.5 As an existing program, ternaries that already compiled (e.g. simple
  identifier/literal arms, float arms, reference arms) keep their behavior unchanged.

## 3. Acceptance themes
- 3.1 Correctness: the 2.1.* integer-ternary forms compile to valid IR (pass the LLVM
  verifier) and return the right value across both branches.
- 3.2 Non-regression: the existing ternary/conditional test surface stays green; the
  `if/else` workaround in `StringBuilder.grow` can be reverted to the ternary form and
  still passes (proving the fix at the original call site).

---

**CLOSED — verified fixed on cajeta 0.14.0 (8ca5b362), 2026-08-01.** Re-ran this
spec's repro against a freshly built 0.14.0 compiler; the defect no longer
reproduces. Archived per td-project-workflow (spec -> archive, INDEX row dropped).
