# ToDo

Open work only. Shipped items are in git history (`git log --oneline`) and the
design docs under `cajeta-docs/`. When something lands, delete it from here —
don't accumulate ✅ entries.

---

## P1 — compiler infrastructure

(open) None — both prior P1 items shipped (templated-interface vtables;
live-borrow pass v1 covering class-typed locals, nested-path borrow
prefix-writes). The borrow tracker can grow further (method-call returns,
loop-induced borrows) but the v1 catches the canonical alias-mutation
hazards; deeper coverage moves to P2 if/when use cases surface.

## P2 — language surface

1. **Stream parallelism — design + multi-session implementation.** Java-style
   fork/join parallel terminals over the existing pull protocol are the
   natural first step given Cajeta's ownership model (split once, merge
   once; data flow stays linear). Reactor-style reactive (push + async +
   backpressure + schedulers) would parallel rather than replace the pull
   protocol — deferred until the sequential surface is fully solid.

2. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted
   `CajetaStruct` test files contained ~105 tests; many exercised happy-path
   behavior still valid under the unified-class model. Pick up between
   bigger pieces.

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
