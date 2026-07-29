# stack-drop-classref — correct field drops for stack-allocated classes

## 1. Definition

### 1.1 Purpose
Fix a latent codegen bug: dropping a **stack-allocated** class whose class-ref
field holds a **String literal** aborts the process (`free(): invalid size` /
`double free or corruption`), because the stack-drop path frees the literal's
static global. Also fix the sibling defect on the same path: base-subobject
class-ref fields are never dropped (silent leak). Deterministic 15-line repro
exists; currently aborts the language tour (`MultiInheritanceDemo`, incidentally —
the trigger is `stack` × String-literal field, not MI).

### 1.2 Root cause (established by investigation, 2026-07-03)
1. `getOrCreateStackDropFunction()` (`src/cajeta/type/CajetaClass.cpp:2667`, field
   walk `:2779–2795`) drops own class-ref fields by calling the per-class heap
   drop wrapper directly; that wrapper ends in an unconditional `__cajeta_free`.
2. The heap path is safe because `emitDropBodyInline` routes field drops through
   `__cajeta_class_virtual_drop`, whose `__cajeta_live_set_claim` guard no-ops on
   static/non-heap pointers. The stack path bypasses that guard.
3. String literals are static view-mode globals (since `ae25b0cb`), so the stack
   path hands a data-section address to `free()`.
4. The stack-drop walk covers own fields only — base-subobject class-ref fields
   are skipped entirely (acknowledged in the comment near `:2745`).
5. Latent since 2026-05-17/19 (`ebb124b9`, `f5715024`, `ae25b0cb`).

### 1.3 Constraints
1. The fix must preserve drop idempotency semantics (claims live in
   `virtual_drop`, not `__cajeta_free`) and String memory-mode awareness
   (`__cajeta_string_drop`).
2. No behavior change for heap-allocated instances.

### 1.4 Non-goals
Redesigning the drop-chain architecture or String memory modes.

## 2. Correct stack-drop of class-ref fields

### 2.1 Requirements
1. Dropping a stack-allocated instance releases each class-ref field through the
   same guarded path as the heap drop (`__cajeta_class_virtual_drop`, or the
   mode-aware `__cajeta_string_drop` for `cajeta.lang.String`).
2. Fields holding static literals, null, heap-backed values, and already-claimed
   values are all handled without abort or double free.
3. Base-subobject class-ref fields are dropped exactly once, in the established
   reverse-declaration order, including under multiple inheritance.

### 2.2 Use cases
1. As a **user**, when a `stack` local of a class with a String-literal field goes
   out of scope, the program continues (no abort).
2. As a **user**, when a stack instance's field was assigned a heap-backed String,
   scope exit frees it exactly once (no leak, no double free).
3. As a **user**, when my class inherits a base with class-ref fields, scope exit
   of a stack instance releases the base's fields too (no leak).
4. As a **tour runner**, `samples/tour/run.sh` exits 0 (unblocks
   docs-refactor §5.2.1).

## 3. Test coverage (currently absent)
`test/parser/DestructorChainTests.cpp` exercises destructor chaining with
counters/arrays only. Required new coverage: auto field-drop of class-ref fields
on stack-allocated classes — literal-holding, heap-backed, null, re-assigned, and
inherited-base variants, plus an end-to-end run asserting clean exit.
