# ToDo

Open work only. Shipped items are in git history (`git log --oneline`) and the
design docs under `cajeta-docs/`. When something lands, delete it from here —
don't accumulate ✅ entries.

Last triage: 2026-05-24.

---

## P1 — compiler infrastructure

(open) None. Prior P1 items resolved:
- **Block-body lambda return-type inference** — fixed in `cf0d299`
  (nested-block walker descent + body-local pre-registration in the
  lambda's resolve-time scope).
- **Captured-class array-field writes** — turned out to be a downstream
  symptom of the lambda-walker gap fixed in `bad612a`; regression
  coverage pinned in `44e7d2c`.

Borrow tracker can still grow further (method-call returns, loop-
induced borrows) but the v1 catches the canonical alias-mutation
hazards; deeper coverage moves to P2 if/when use cases surface.

## P2 — language surface

1. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted
   `CajetaStruct` test files contained ~105 tests; many exercised happy-path
   behaviour still valid under the unified-class model. Pick up between
   bigger pieces.

2. **Template wildcards — future work** (not blocking; v1 + bounded wildcards
   shipped in `1f9d388`..`2e8cb03`):
   - **Capture conversion proper** (`Stream<? extends Number>` produces
     `Number` at read sites, etc.). Extends method resolution to carry
     capture identities through the dispatch. Also: lets the 4 wildcard
     lints fire cleanly inside method-template bodies — currently
     suppressed there because the wildcard sentinel doubles as a
     placeholder for uninstantiated T (see LintRules.md "Known
     limitation" lines).
   - **Stdlib producer/consumer signature migration** to use bounded
     wildcards where they'd express PECS variance more clearly than the
     current concrete instantiations.

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
