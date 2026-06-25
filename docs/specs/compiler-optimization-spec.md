# Compiler Optimization — Spec (TBAA + Stream devirtualization/fusion)

## 1. Definition

### 1.1 Purpose
Close the two architectural performance-gap clusters the benchmark-gap-sweep surfaced
and deferred, by improving the **compiler**, not individual benchmarks:

- **Phase A — TBAA emission.** Cajeta emits **no** type-based-alias-analysis metadata,
  so LLVM treats an array-element store as potentially aliasing the enclosing object's
  fields. Loop-invariant field loads (and the `count` read-modify-write) can't be
  register-promoted across the store. This taxes every field-heavy hot loop:
  `arraylist-append` (5.3× off Vec), `hashmap-int` (2.6×), and a share of the stream
  and JSON costs.
- **Phase B — Stream devirtualization / fusion.** The stream pipeline is a pull-based
  chain of `heap` wrapper objects whose `next()` is a **virtual** call returning an
  `Optional<T>` by value. For `stream-filter-map-reduce` (102×) and
  `stream-parallel-reduce` (127×) each element pays 2–3 virtual `next()` dispatches plus
  indirect lambda calls; C++ `views` and Java's JIT fuse the whole chain to one loop.

### 1.2 Scope
Backend/codegen changes in `src/cajeta/`. Both phases are validated against the full
test suite (correctness is paramount — alias metadata and devirtualization are both
miscompile-prone) and measured on the profile suite. Built incrementally: the most
conservative sound version first, then refinements gated on measurement.

### 1.3 Non-goals
- A full mid-level IR rewrite. Phase B reuses the existing CIR/closure-devirt machinery
  (`feature/closure-devirt` work) where possible rather than building new infrastructure.
- `time-*` (value-type in-place reassign — separate limitation), `task-spawn` (Go
  scheduler), `md5`/`blake3` (asm references). These stay out of scope.
- Auto-vectorization of the fused stream loop (a later, separate lever).

## 2. Phase A — TBAA emission

### 2.1 Soundness model
Array buffers (`__cajeta_new_array_header`) and object field storage are **always
disjoint allocations**, so an array-element access and an object-field access can never
alias. TBAA correctness rule: two accesses carrying **different** type tags must never
actually alias; an **untagged** access conservatively aliases everything. Therefore
tagging *only* array elements and object fields (leaving everything else untagged) is
sound, and disjoint tags let LLVM hoist field loads across element stores.

### 2.2 Escape hatches (correctness)
- **Raw/byte access** (`int8[]` buffers, `memcpy`/SWAR paths, `@Native` interop,
  reinterpreting casts) → the TBAA "omnipotent char" node, which aliases all. Never a
  disjoint tag.
- **Ambiguous provenance** (a pointer whose access kind isn't statically known at the
  load/store site) → no tag (conservative).
- **Inline `T[N]` array fields** (StringBuilder SSO) → treated as array elements; their
  bytes never overlap a *different* field, so disjointness still holds.

### 2.3 Use cases
- 2.3.1 As the optimizer, when `ArrayList.add` stores `data[count]=v` in a loop, then
  the `count`/`data`/`capacity` field loads are register-promoted across the store
  (they carry a field tag disjoint from the element tag), so the loop drops from ~5
  memory ops/push toward Vec's ~1.
- 2.3.2 As the optimizer, when a hashmap probe loop reads bucket array elements and
  table fields, then field loads hoist across element writes.
- 2.3.3 As a developer relying on byte buffers (String, hashing), when bytes are
  accessed via `int8[]` or `memcpy`, then results are bit-identical (char node aliases
  everything — no reordering hazard). **The full test suite passes unchanged.**

### 2.4 Acceptance
- Full `ctest` suite green (no miscompiles).
- `arraylist-append` and `hashmap-int` measurably improved; fidelity gate green.
- Coarse two-node TBAA (field vs array-elem) first; struct-path per-field and
  per-element-type refinements only if measurement justifies them.

## 3. Phase B — Stream devirtualization / fusion

### 3.1 Use cases
- 3.1.1 As the optimizer, when a stream chain (`ArrayStream → filter → map → reduce`) is
  built from locally-constructed, never-reassigned wrappers of statically-known concrete
  type, then the `next()` calls devirtualize to direct calls and inline, collapsing the
  chain toward a single fused loop.
- 3.1.2 As a developer, when I run the same pipeline, then the result is identical to
  the sequential pull semantics (filter drops, map transforms, reduce folds in order).

### 3.2 Acceptance
- `stream-filter-map-reduce` and `stream-parallel-reduce` materially closed (target:
  out of the >2× gap tier, ideally into the compiled-peer field).
- All stream tests green; parallel path still correct.

## 4. Cross-phase acceptance
- Full suite green after each phase.
- Site regenerated; standings re-read; before/after recorded.
- Findings recorded in memory (extends `reference_devirt_final_and_missing_tbaa_levers`).
