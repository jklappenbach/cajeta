# methodcall-decomposition — break up the 9,063-line `generateCode`

## 1. Definition

Opened 2026-08-09. `src/cajeta/asn/expression/MethodCallExpression.cpp`
is **10,609 lines**, the largest file in the compiler, against a project
norm of "no source module over ~1,000 lines."

### 1.1 The file is not the problem — one function is

Measured:

| region | lines |
|---|---|
| helpers before `generateCode` (~25 functions) | 1–1542 |
| **`MethodCallExpression::generateCode`** | **1543–10606 (9,063 lines)** |

So "split it into multiple .cpp files" cannot be the first move: a single
function cannot span translation units. The decomposition must extract
named helpers FIRST; only then can clusters of them move to sibling TUs.

### 1.2 Scale context

26 of 456 source files exceed 1,000 lines. The worst offenders after this
one: `CajetaClass.cpp` 6,765, `Expression.cpp` 6,453,
`KernelLowering.cpp` 5,810, `Compiler.cpp` 4,467,
`BinaryOpExpression.cpp` 4,179. This spec covers
`MethodCallExpression.cpp` only; the others are candidates for the same
treatment once the pattern is proven.

## 2. Sequencing — AFTER current work (developer instruction, 2026-08-09)

This must NOT land concurrently with:
- the `ownership/collections-borrow-by-default` branch (has commits in
  this file),
- the transfer/borrow staleness remediation (126 items, in flight),
- the 0.19.0 release sweep,
- any active sibling-clone session (cajeta-five was active this cycle).

A 9,000-line reshuffle makes every concurrent rebase brutal. Land those
first, take a green sweep, then start here from a quiet tree.

## 3. Approach

- **3.1 Extract by call shape.** `generateCode` is a long dispatch over
  call forms. Extract each arm into a named private helper returning
  `llvm::Value*`, keeping them in the same file initially so each step is
  a behaviour-preserving move reviewable in isolation.
- **3.2 Then split into sibling TUs.** Once the arms are named, move
  clusters out. Natural seams already visible in the file:
  - `_Transform.cpp` — the grad/vmap/fused/nested-grad statics
    (`emitGradBackward`, `emitGradAllBackward`, `emitVmapBatched`,
    `emitVmapOfGrad`, `emitFusedExpr`, `emitNestedGrad`,
    `emitTransformAnnotatedCall`) — ~900 lines, already self-contained.
  - `_Ownership.cpp` — transfer-word composition, drop deactivation, the
    TRANSFER_REQUIRED / borrow-shape diagnostics.
  - `_Intrinsics.cpp` — System stream detection, string-arg loading,
    closure-call emission.
- **3.3 One commit per extraction.** Each step must be individually
  revertable and individually green.

## 4. Code review — required, not optional (developer instruction)

The decomposition is also the audit. A 9,000-line function accumulates
things nobody can see; extraction is the moment they become visible. The
review must report:

- **4.1 Dead code.** Arms that no live call shape reaches, statics with
  no remaining callers, branches whose guard cannot be true. Note the two
  overloads of `synthesizeMakeClosure` (lines 636 and 669) as a first
  thing to check.
- **4.2 Redundancies.** Repeated logic across arms that should be one
  helper — argument coercion, receiver resolution and title-flag stashing
  are the likely candidates.
- **4.3 Cyclomatic complexity.** Measure per extracted helper. Anything
  left above an agreed threshold gets flagged with a proposed further
  split rather than quietly shipped.
- **4.4 Stale comments.** This file already produced confirmed-stale
  ownership comments in the 2026-08-09 sweep. Extraction is the moment to
  fix the rest rather than carry them into new files.

Findings are reported and triaged BEFORE deletion — dead-looking code in
a compiler is often reached by one exotic shape, so removal needs a pin
or an argued case, never just "no callers found."

## 5. Requirements

- **5.1** No behaviour change. The full sweep is green before and after,
  with the same pass count.
- **5.2** No file over ~1,000 lines when finished, `generateCode`
  included.
- **5.3** Every extracted helper has a name that says what call shape it
  handles.
- **5.4** The §4 review is delivered as a written report, not folded
  silently into the commits.
- **5.5** Each extraction commit is independently revertable and green.

## 6. Acceptance

- **6.1** `wc -l` on every file under `src/cajeta/asn/expression/` is
  under ~1,000.
- **6.2** Full sweep pass count identical to the pre-decomposition
  baseline.
- **6.3** The §4 report exists, with dead code / redundancy / complexity
  findings each either fixed or explicitly deferred with a reason.
