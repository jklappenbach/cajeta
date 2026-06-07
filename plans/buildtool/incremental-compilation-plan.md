# Incremental compilation — plan

Companion to the design doc
[`cajeta-docs/IncrementalCompilation.md`](../../cajeta-docs/IncrementalCompilation.md).
That document is the **architecture**; this is the **phased work
breakdown**. Checkbox convention: `- [ ]` open, `- [x]` shipped,
`- [~]` deferred-with-note.

This plan closes the build-tool-plan `[~]` items that were blocked on
"compiler cooperation":

- Phase 5b — *"Touching one source rebuilds only that file +
  dependents."*
- Phase 5b — *"Rebuild after eviction produces byte-identical IR."*
- Phase 5 end-to-end — *"First build of a real source tree succeeds
  end-to-end."*

Sequencing is deliberate: each phase is independently shippable and
sound on its own, so value lands incrementally and we never half-build
an unsound seam. Phases 0–1 are low-risk and unblock the byte-identical
criterion immediately; Phases 2–5 build the module-granular engine.

---

## Phase 0 — Whole-artifact cache (fallback, sound today)

**Goal:** a no-change rebuild is instant, with zero compiler-
architecture risk. Coarse granularity; superseded by Phase 5 for the
"only changed file" criterion but kept as the fast-path / fallback.

- [ ] Build tool computes a whole-build digest = `H(sorted per-source
      transitive digests ⊕ discriminator)` using the existing
      `SourceDigestRegistry` + `computeCacheDiscriminator`.
- [ ] On a hit (digest unchanged + prior artifact present), skip the
      compiler invocation entirely and re-publish the cached artifact;
      emit `cache: hit` as an action output.
- [ ] On a miss, build normally and record `(digest → artifact)`.
- [ ] `--no-cache` build param + `cajeta clean --deep` bypass.

**Acceptance**

- [ ] A second `cajeta build` with no source changes does not invoke
      the compiler and reproduces the same artifact path/sha.

---

## Phase 1 — Determinism prerequisites

**Goal:** a `.bc` is byte-stable across machines, so any cache that
trusts `.bc` bytes is trustworthy. Independently closes the
"byte-identical IR" criterion.

- [x] Route `setSourceFileName` (`CajetaModule.cpp:102`) through
      `--debug-prefix-map` so the embedded source name is the canonical
      `cajeta:`-relative path, not the absolute path. _Implemented via
      `CajetaModule::remapSourcePath` (pure, testable) +
      `canonicalizeSourceFileName()`, called from `Compiler::createModule`
      after `setFlags`. Falls back to a sourceRoot-relative name when no
      map is supplied (direct compiler invocation)._
- [x] Audit the emit path for other non-determinism. _Confirmed nothing
      in `src/` reads the embedded `getSourceFileName` (so the rewrite is
      side-effect-free); debug-info is off by default (no DWARF DIFile
      path embedding); module ID is the canonical name (root-independent).
      The end-to-end byte-identical test below compares the WHOLE `.ll`,
      so any other root-dependent embedding would fail it — none does._
- [x] Add a determinism test. _`test/compile/ReproducibleIrTests.cpp`:
      5 pure `remapSourcePath` cases + 2 `createModule` cases + 1 e2e
      that compiles the same source under two absolute roots and compares
      emitted `.ll` bytes. 8/8 green._

**Acceptance**

- [x] Same source + flags on two distinct paths yields byte-identical
      IR. _`ReproducibleIr.EmittedIrIsByteIdenticalAcrossRoots` proves it
      end-to-end. Substantively closes build-tool-plan Phase 5b
      "byte-identical IR" for the determinism axis; the
      rebuild-after-eviction variant additionally rides on the Phase 3/4
      cache-fed path._

---

## Phase 2 — Per-module codegen artifacts + obligation capture

**Goal:** the compiler can emit, per module, a `.bc` plus a sidecar
recording that module's **instantiation obligations** — without yet
skipping anything. Pure instrumentation; behavior unchanged.

- [ ] Emit each user module's `.bc` to an addressable path (extend the
      existing per-module emit; `WriteBitcodeToFile` already exists at
      `Compiler.cpp:1506`).
- [ ] Capture, per module, the set of `(template, type-args)` its
      codegen drove into stdlib (instrument the instantiation site that
      the `Compiler.cpp:759-767` loop comment describes).
- [ ] Serialize obligations to a per-module sidecar (stable ordering).
- [ ] Test: a module using `ArrayStream<int32>` records exactly that
      obligation; a module using none records an empty set.

**Acceptance**

- [ ] For a fixture tree, every module's recorded obligations exactly
      match the template instantiations observed during a full build.

---

## Phase 3 — Codegen skip + obligation replay (the sound seam)

**Goal:** skip codegen for clean modules, load their cached `.bc`, and
replay their obligations so stdlib still contains every symbol the
loaded `.bc` references. Declarations are still registered by parsing
(interface cache deferred to Phase 6).

- [ ] Driver computes the dirty set from transitive digests; clean
      modules are codegen-skipped, dirty modules recompiled.
- [ ] Clean modules: register declarations (parse for now), then load
      the cached `.bc` into the module slot via the `parseBitcodeFile`
      path (`CajetaModule.cpp:863`) instead of `generateCode()`.
- [ ] Before link, union live + all-clean obligations and ensure stdlib
      contains the full set (instantiate any missing).
- [ ] Guard: discriminator mismatch between manifest and compiler's own
      computed discriminator ⇒ ignore cache, full rebuild, warn.

**Acceptance**

- [ ] Touch one leaf source ⇒ only it is recompiled; its `.bc` and all
      others link; program runs identically.
- [ ] Touch a widely-imported source ⇒ it + all transitive dependents
      recompiled, the rest skipped (assert compile count).
- [ ] A clean module whose sole template use is skipped still links
      (obligation replay populated stdlib).

---

## Phase 4 — Build-tool ↔ compiler manifest protocol

**Goal:** the build tool drives Phase 3 across the process boundary via
the cache-manifest file.

- [ ] `--cache-manifest=<path>` parsed in `src/main.cpp` (match the
      `match(arg, "name", value)` convention) → `Compiler` setter.
- [ ] `cache-manifest-v1` loader (strict JSON via llvm::json/`JsonC`):
      per source, the interface/`.bc`/obligation slots to load (clean)
      or write (dirty), plus the manifest discriminator.
- [ ] Compiler-direct populate: dirty modules write `.bc` + obligation
      sidecar to the slots the manifest names.
- [ ] `BuildAction` (`actions/BuildAction.cpp`): enumerate sources,
      build `SourceDigestRegistry`, compute discriminator, assemble +
      write the manifest, pass the flag. (`BuildAction` does not use
      `IrCache`/`SourceDigest` today — this is the first wiring.)
- [ ] After a successful build, `IrCache::evict` per
      `settings.build.cache` (size cap + TTL).

**Acceptance**

- [ ] Build twice over a fixture: the second manifest marks all sources
      clean; the compiler skips all codegen; artifact unchanged.
- [ ] Cache size cap enforces eviction after the build.

---

## Phase 5 — End-to-end + the stuck criteria flip

**Goal:** the module-granular engine is the default `cajeta build`
path; the build-tool-plan `[~]` items flip to `[x]`.

- [ ] Make the manifest path the default for `cajeta build` (Phase 0
      whole-artifact cache becomes the no-change fast path layered in
      front).
- [ ] Real source-tree smoke (the stdlib or a sample app) builds
      end-to-end incrementally; wire into CI.
- [ ] Flip build-tool-plan Phase 5 / 5b acceptance items and update the
      "Acceptance (end-to-end — gated on compiler integration)" notes.

**Acceptance**

- [ ] build-tool-plan "touching one source rebuilds only that file +
      dependents" — `[x]`.
- [ ] build-tool-plan "rebuild after eviction produces byte-identical
      IR" — `[x]`.
- [ ] build-tool-plan "first build of a real source tree succeeds
      end-to-end" — `[x]` (CI smoke).

---

## Phase 6 — Interface cache (optional optimization)

**Goal:** stop re-parsing clean modules' bodies just to register their
declarations; load a serialized **module interface** instead. Pushes a
no-change rebuild toward true no-op cost. Deferred until Phases 0–5
prove the model (design open question I1).

- [ ] Define the serialized module-interface artifact (declared types,
      method signatures, field layouts, generic bounds).
- [ ] Emit it per module, keyed by the module's own content digest.
- [ ] Clean modules: load the interface into the archive without
      parsing the body; dependents resolve against it.
- [ ] Invalidate when the module's own digest changes.

**Acceptance**

- [ ] A no-change rebuild parses zero clean-module bodies (assert).
- [ ] A dependent of a clean module resolves all symbols from the
      cached interface alone.

---

## Risks

| Risk | Mitigation |
|---|---|
| Obligation capture misses a side effect of codegen (not just template instantiation) ⇒ link failures on skip | Phase 2 is pure instrumentation validated against full builds before any skip; Phase 3 guard falls back to full rebuild on link failure |
| `.bc` non-determinism beyond `setSourceFileName` silently busts the cache | Phase 1 audit + byte-identical test gate before trusting bytes |
| Stale manifest feeds wrong IR | discriminator cross-check (Phase 3/4 guard); content-addressed keys |
| Module granularity too coarse to matter on real edits | measure on the stdlib; escalate to finer grain or query-engine (design alternatives) only if data warrants |

## Ordering — quick view

```
0. Whole-artifact cache  ── sound today, coarse, fallback
1. Determinism            ── unblocks byte-identical, independent
        │
        ├─→ 2. Per-module .bc + obligation capture (instrument)
        │         │
        │         └─→ 3. Codegen skip + obligation replay (sound seam)
        │                   │
        │                   └─→ 4. Manifest protocol (build-tool wiring)
        │                             │
        │                             └─→ 5. Default path + criteria flip
        │
        └─→ 6. Interface cache (optional, after the model is proven)
```
