# ToDo

Open work only. Shipped items are in git history (`git log --oneline`) and the
design docs under `cajeta-docs/`. When something lands, delete it from here —
don't accumulate ✅ entries.

Last triage: 2026-05-24.

---

## P1 — compiler gaps blocking natural idioms

Both surfaced during the lambda-walker / parallel-correlation work (commits
`bad612a`, `9e38fc8`, `aebb740`). Each currently forces a workaround in the
test suite or the stdlib; lifting them removes those workarounds and unblocks
shapes users will naturally write.

1. **Block-body lambda return-type inference inside generic-method args.**
   `s.fold(0, (int32 acc, int32 x) -> { ... return ...; })` builds the
   lambda with a `void` return type because `R` from `fold<R>` isn't
   propagated to the lambda before body type-check. The body's `return t;`
   then trips JIT verify ("Found return instr that returns non-void in
   Function of void return type"). Current workaround: hoist the lambda to
   a typed local `(int32, int32) -> int32 accFn = ...;` then pass `accFn`.
   See `LambdaNestedBlockPatternsTests` for the two pinning sites. Fix: when
   resolving a lambda argument whose target parameter type is a function
   type, propagate the target's return type into the lambda before
   body resolution.

2. **LLVM alloca-cast assertion on lambda writes to array-field of
   captured class.** Inside a lambda body, `cap.arr[i] = x` (where `cap`
   is captured and `arr` is a class-typed array field) trips
   `dyn_cast<AllocaInst>` on a non-existent value during codegen. Side-
   channel correlation tests had to use scalar fields only — see header
   comment in `ParallelDispatchCorrelationTests.cpp`. Reproducer is a
   one-liner; root-cause likely lives near the captures-struct lowering
   path for indexed assignments on captured class fields.

## P2 — language surface

1. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted
   `CajetaStruct` test files contained ~105 tests; many exercised happy-path
   behaviour still valid under the unified-class model. Pick up between
   bigger pieces.

2. **Template wildcards — future work** (not blocking; v1 + bounded wildcards
   shipped in `1f9d388`..`2e8cb03`):
   - **Capture conversion proper** (`Stream<? extends Number>` produces
     `Number` at read sites, etc.). Extends method resolution to carry
     capture identities through the dispatch.
   - **Stdlib producer/consumer signature migration** to use bounded
     wildcards where they'd express PECS variance more clearly than the
     current concrete instantiations.
   - **Lint rules from `cajeta-docs/LintRules.md` "Future rules"**:
     `wildcard-materialize-in-loop`, `wildcard-crosses-hot-boundary`,
     `wildcard-field-in-small-class`, `discarded-wildcard-next`. Lands on
     the v1 lint infrastructure pattern (see existing `uncaught-throws`).

3. **Stream parallelism — loose ends** (v1 substantially shipped; P1–P5
   phases from `cajeta-docs/stdlib/StreamParallelism.md` landed across
   `a83fa06`..`fba0506` plus collect/error helpers in `1d706ee` /
   `feb4085`):
   - **HashMap stream views as Splittable** (P4) — `HashMapEntryStream`
     and the Key/Value variants exist as wrappers but need `trySplit()` /
     `estimateSize()` to participate in the parallel driver. Currently
     fall back to sequential through the parallel terminals.
   - **Lint passes** `[parallel-stateful-op]` and
     `[parallel-collector-no-combiner]` from the design doc P4. The
     combiner-required check now throws at runtime
     (`CAJETA_ERROR_STREAM_PARALLEL_FOLD_NO_COMBINER`); a compile-time
     lint would catch the misuse earlier.
   - **`pickSplitCount` reads scheduler core count** (P5) — current
     impl uses a fixed split count; should read core count from the
     fiber scheduler for better load balance.

4. **Stream<T>.fold<R> overload resolution for block-body lambdas.** Same
   root cause as P1#1 above but the fix-vs-workaround tradeoff lives at
   the language-surface layer (do we keep requiring typed locals, or
   fix inference). Track here so the discussion happens with the P1
   fix planning.

## P3 — Lombok polish

1. **`@Builder.Default` for per-field defaults — ~0.5 session.** Requires
   field-initializer parsing wired through to the synthesized builder body.

2. **`@Getter(level="private")` visibility tightening — ~0.5 session.**

3. **`@ToString(format=TO_STRING_JSON)` — trivial.** Synthesizer hook is
   unblocked now that `Json.toBytes<T>` ships; currently throws
   `CAJETA_ERROR_TOSTRING_JSON_NOT_IMPLEMENTED`. Delegate to
   `Json.toBytes<T>(this)`.

4. **Return-type `@NonNull` — deferred.** Parameter-position shipped;
   return-position adds a post-call null check at every callsite.

## P4 — debug-mode features (`CompilerModes.md`)

All `[debug]`. Each ~0.5–1 session unless noted.

1. **`--live-set=strict`** — unbounded growth + rehash; assert on
   duplicate-add (catches compiler codegen bugs). `off` shipped.
2. **`--poison-free=on`** — memset freed body with sentinel before
   `__cajeta_free`.
3. **`--drop-chain-validate=on`** — per-push/pop linked-list integrity
   checks; assert + diagnostic on corruption.
4. **`--diag-hints=on`** (~1 session) — "did you mean..." for typo'd
   identifiers; recommend `#`-transfer when a borrow violates lifetime;
   suggest `@SuppressLint(...)` for noisy lints.
5. **Stack-trace capture on throw** (~1 session) — `backtrace(3)` + DWARF
   + source-map symbolization in the exception payload.
6. **`--use-after-move-rt=on`** — sentinel in moved slot header; trap on
   read. Backs up the static use-after-move tracker.
7. **`--ub-traps=on`** — trap instructions for divide-by-zero, oversized
   shift, unaligned atomic. Signed-overflow + bounds-trap already shipped
   via `--overflow-checks` / `--bounds=trap`.

## P5 — release-mode features (`CompilerModes.md`)

All `[release]`.

1. **`--null-checks=on/off/trap` codegen** (~0.5 session) — today null-check
   generation is implicit; expose the flag so users can opt out at high
   `--release` confidence. `@NonNull` integrates here.
2. **`--profile-counters=on`** (~1 session) — per-method invocation counter
   + wall-time tally for PGO collection. Default on under `--debug-release`.

---

## Design notes

- **No Rust-style lifetime annotations.** Cumbersome and confusing. The
  multi-parameter borrow-return problem (free functions can't return a
  borrow over multiple inputs — `MemoryModel.md:307`) needs a different
  solution. Open design question.

- **`super.~Class()` chaining** — implicit destructor chaining shipped, so
  the explicit upcall form is purely cosmetic. No compiler change needed
  unless users actually want the spelling for documentation.

- **Closure heap-allocation (L3-3)** — `LambdaL3Tests` currently asserts
  borrow-escape compiles cleanly only for value-/transfer-only captures;
  heap-allocating escaping closures would let `return () -> arr.count();`
  with a borrow capture work via lifetime extension. Not on the roadmap
  — flagged so the constraint isn't mistaken for an oversight.
