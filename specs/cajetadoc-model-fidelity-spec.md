# cajetadoc model fidelity — spec (draft)

Origin: docs-refactor 15.1 (unit-10 stdlib-reference verification, 2026-07-03).

## 1. Definition

`cajeta doc --emit-model-json` is the machine-readable source of truth for a
class's public surface — the stdlib reference and coverage checks are seeded
from it. Two fidelity gaps force consumers to fall back to reading source:

1. **Operator methods are omitted.** `operator==`, `operator[]`, and the
   rest of the operator-overload family do not appear in the model at all,
   although they are public API.
2. **Generic method signatures render Java-style.** The model prints
   `<T> void sort(...)` where the source (and the language grammar) use the
   postfix form `void sort<T>(...)` (method-level templates, L-22).

## 2. Features

### 2.1 Operator methods in the model
The model JSON carries one entry per operator overload, with a `kind:
"operator"` discriminator and the operator token (`==`, `[]`, `+`, …).
Use cases:
1. As the stdlib-reference generator, when a class overloads `operator==`,
   then the generated method table lists it without a hand-written patch.
2. As the coverage check, when an operator is public and undocumented, then
   the gap is reported (today it is invisible).

### 2.2 Source-faithful generic signatures
Signatures in the model reproduce the source's postfix type-parameter form.
Use cases:
1. As a doc reader, when I copy a signature from the reference into code,
   then it parses (`xs.sort<int64>()`-style call syntax matches).
2. As the doc tool's snippet checker, when a signature is embedded in a
   docstring example, then round-tripping model → doc → compile succeeds.

## 3. Non-goals
Rendering changes beyond the two gaps; the HTML/site pipeline; tag semantics
(covered by docs-refactor unit 3).
