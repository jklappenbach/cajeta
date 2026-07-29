# Incremental compilation — design

**Status:** design. Companion plan:
`plans/buildtool/incremental-compilation-plan.md`.
This document is the *architecture and rationale*; the plan is the
*phased work breakdown*.

## Goal

Make `cajeta build` reuse work across builds at **module granularity**:
touching one source recompiles only that source and the modules whose
meaning depends on it, not the whole program. Two observable
properties define done:

1. **Touching one source rebuilds only that file + its dependents.**
2. **A rebuild with no changes is a near-instant no-op**, and a rebuild
   after cache eviction reproduces **byte-identical** IR.

The build tool already has the bookkeeping for this — `IrCache`
(content-addressed `.bc` store) and `SourceDigestRegistry` (per-source
transitive digest) shipped in build-tool Phase 5b. What's missing is a
compiler that can *act* on that bookkeeping soundly. This document
explains why that's non-trivial and how to do it.

## Why the obvious approach is unsound

The naive plan — "for each unchanged source, load its cached `.bc`
instead of compiling it" — does not work against the compiler's actual
architecture. `Compiler::compile(entry, sourceRoot, archiveRoot)`
(`src/cajeta/compile/Compiler.cpp:699`) runs in two phases that are
**not** per-file:

**Phase A — declaration registration (per source, but globally
visible).** `prescanSourceRoot` (`Compiler.cpp:300`) then
`compile(module)` (`Compiler.cpp:682`) → `parse(module)` register every
module's classes, interfaces, methods, and field layouts into a
**global type archive**. Every module must be registered so that any
other module can resolve references to it.

**Phase B — whole-program iterative codegen.** A single loop
(`Compiler.cpp:808-826`) runs `getLlvmFunctionType()` + `generateCode()`
across **all** modules together, iterated to a fixed point. It must
iterate because a method body in one module can trigger a **template
instantiation that lands in the *stdlib* module** (e.g. `xs.stream()`
→ `ArrayStream<int32>`, instantiated into stdlib's structures).

Two consequences make naive `.bc` reuse incorrect:

- **You cannot skip a module's declarations.** If `Y` imports `X` and
  you touch `Y`, then `X` is unchanged but `Y` must recompile — and
  compiling `Y` needs `X`'s *declarations* in the archive to resolve
  types. A cached `.bc` carries lowered IR, **not** the semantic model,
  so `Y` would fail to resolve `X`'s symbols.
- **You cannot even just skip a module's codegen.** If `X` is the only
  module that uses `ArrayStream<int32>`, that template is instantiated
  *into stdlib* as a side effect of compiling `X`. Skip `X`'s codegen
  and the instantiation never happens, so `X`'s loaded `.bc` references
  a symbol absent from this build's stdlib module → **link failure**.

The one thing the architecture *does* give us for free: cross-module
references are emitted as module-local **extern declarations** via
`getOrInsertFunction` (`CajetaModule::ensureFunctionVisible` /
`ensureFunctionInModule`, `CajetaModule.cpp:919-953`) and resolved by
symbol name at link time. So a per-module `.bc`, once produced, **is**
linkable in isolation. The problem is purely about safely *not
regenerating* one, not about linking it.

## The model: dirty-set over a stable module interface

Incremental compilation here is three cooperating ideas.

### 1. Module interface (the declaration cache)

Separate each module's **interface** (its externally-visible
declarations: class/interface/enum names, method signatures, field
layouts, generic bounds — everything a *dependent* needs to resolve
against it) from its **implementation** (method bodies → IR).

The interface is what Phase A registers today by re-parsing. We persist
it: a serialized interface artifact per module, keyed by the module's
**own** content digest (not transitive). On a build, a *clean* module's
interface can be loaded into the archive **without re-parsing its
body**, so dirty dependents resolve against it. This is the cajeta
analogue of Rust's `.rmeta` / Clang's `.pcm` / a C++20 BMI.

Re-parsing to register declarations is cheaper than codegen, so an
interface cache is an optimization, not a correctness requirement — but
it is what lets a no-change rebuild approach true no-op cost. **Open
question (I1):** whether to start with "always re-parse for
declarations, cache only codegen" (simpler, sound, smaller win) and add
the interface cache later. See the plan's Phase 1 vs. Phase 3 split.

### 2. Dirty set (what must be regenerated)

A module is **dirty** if its transitive digest changed —
`SourceDigestRegistry` already computes `H(source ⊕ sorted transitive
import digests)`, so editing `X` dirties `X` and everything that
transitively imports `X`, and nothing else. The driver computes the
dirty set up front. Clean modules contribute cached `.bc` + cached
interface; dirty modules are fully recompiled.

### 3. Instantiation obligations (the shared-state side effects)

This is the crux the naive approach misses. When a *clean* module's
codegen is skipped, any **template instantiations it would have driven
into stdlib** must still be present, or its `.bc` won't link. So each
module's cache entry records its **instantiation obligations**: the set
of `(template, type-args)` it caused to be instantiated. On a build:

1. Recompile dirty modules live; collect their fresh obligations.
2. Union in every **clean** module's recorded obligations.
3. Ensure stdlib contains the full union before linking (instantiate
   any obligation not already produced by a live module).

Because instantiations are deterministic functions of `(template,
type-args, flavor)`, replaying a clean module's obligations yields the
same stdlib symbols its `.bc` was linked against. **Open question
(I2):** whether obligations can simply be *names* the driver ensures, or
whether the instantiated bodies themselves should be cached as part of
stdlib's own per-build `.bc`. Leaning toward caching stdlib like any
other module and treating obligations as the trigger set that makes a
skipped module's needs explicit.

**Two obligation flavors, one set.** There are two independently-codegen'd
instantiation kinds, both captured at a single choke point and serialized
into the *same* sorted sidecar set, distinguished by their key shape:

- **Class templates** — captured at `CajetaClass::instantiate` (wrapping
  `instantiateInternal`), keyed by `inst->toCanonical()`, e.g.
  `cajeta.lang.stream.ArrayStream<int32>`. This key is *identical by
  construction* to the `getStructureToModule` registry key (both are the
  `instCanonical = qName->toCanonical() + buildArgSuffix(args)` string), so
  replay resolves an obligation to its owning module by direct map lookup —
  no key reconciliation is needed. (Primitive args canonicalize to their
  bare name, e.g. `int32`, not `cajeta.int32`; both keys agree on that.)
- **Method templates** — captured at `Method::instantiateMethodTemplate`
  (wrapping `instantiateMethodTemplateInternal`), keyed by
  `inst->getMapKey(false)`, e.g.
  `cajeta.lang.stream.Stream<int32>::map((int32) -> #int32)<int32>`. A
  method-template instantiation lands its body in the *host* class's module
  via `host->addMethod` — a side effect entirely separate from the class
  instantiation, so replaying the class obligation does **not** re-create
  it. The `::` host/method separator distinguishes a method obligation from
  a class one at replay. The host-class instantiation (`Stream<int32>`) is
  itself recorded as a class obligation, so it is a prerequisite the driver
  must replay first.

  *Known cosmetic dup:* the same method instantiation can appear twice —
  once before and once after `this` is inserted into the formal list
  (`map((int32) -> …)` vs. `map(pointer,(int32) -> …)`) — because capture
  fires on the cached re-entry during the codegen loop's later passes. Both
  replay to the same `(host, name, type-args)` instantiation, which is
  idempotent, so the duplicate collapses harmlessly; the value-param
  signature in the key only matters to disambiguate same-name/same-type-arg
  method-template overloads (vanishingly rare).

## Determinism prerequisites

Module-granular caching is only as trustworthy as the byte-stability of
a `.bc`. One known defect formerly blocked "byte-identical IR":

- `CajetaModule.cpp:106` constructs each module with
  `setSourceFileName(sourcePath)` — the **absolute** path, which would
  bake a machine-specific string into every module. **This is now
  scrubbed:** `canonicalizeSourceFileName()` (`CajetaModule.cpp:159`,
  called from `Compiler.cpp:540`) rewrites the embedded name via
  `remapSourcePath()`, honoring an explicit `--debug-prefix-map`
  (`flags.debugPrefixMap`) and otherwise falling back to a
  `sourceRoot`-relative path, so the embedded name is machine-
  independent even when the compiler is driven directly. The fix this
  section originally proposed has landed.

Other determinism surfaces to audit before trusting the cache: symbol
iteration order during emit, any timestamp/host embedding, and seed
propagation (`--seed`, `--source-date-epoch` are already plumbed into
`CompilerFlags`). These are verification tasks, not redesigns.

## Build-tool ↔ compiler protocol

The build tool owns the dirty-set decision; the compiler obeys it
across the process boundary. The wire format is a **cache-manifest
file** passed as `--cache-manifest=<path>` (chosen over repeated
per-source flags: it scales past argv limits and is self-evidently
generated, not a user knob). The manifest names, per source, the
interface + `.bc` slots to load (clean) or write (dirty), plus the
recorded obligations for clean modules. The compiler writes freshly
produced artifacts directly into the slots the manifest names
(compiler-direct populate — one I/O, no copy).

Full manifest schema and field-by-field semantics live in the plan
(Phase 4), since they evolve with the implementation.

## Alternatives considered

- **Whole-artifact cache only.** Digest the entire source set; if
  unchanged, skip the build and reuse the prior artifact. Sound and
  trivial, but coarse — gives "no-op rebuild is instant" and nothing
  finer. Worth shipping as a *fallback* / first increment (plan Phase
  0) while the module-granular engine is built, but it does not satisfy
  goal (1).
- **Naive per-source `.bc` reuse.** Rejected above — unsound under
  whole-program codegen + stdlib template instantiation.
- **Full query-engine rewrite (Salsa/rustc-style demand-driven
  compilation).** The "correct" long-term shape, but a ground-up
  compiler restructuring far beyond this effort. The interface +
  obligations model gets most of the win without rewriting the driver
  as a query graph. Revisit if module granularity proves too coarse.

## Open questions

- **I1 — Interface cache now or later?** Start with always-reparse +
  codegen cache (sound, simpler), or build the serialized interface
  cache up front? *Lean:* defer the interface cache; prove the
  codegen-skip + obligation-replay path first.
- **I2 — Obligations as names vs. cached bodies?** *Lean:* cache stdlib
  as a module like any other; obligations are the trigger set.
- **I3 — Granularity.** Module-level only, or finer (per-class /
  per-method)? *Lean:* module-level for v1; the digest + `.bc` are
  already module-shaped.
- **I4 — Cross-build flavor/target coupling.** The discriminator must
  fold in flavor, target, profile, toolchain identity (Phase 14
  `toolchainIdentity` already exists). Confirm no shared-state leak
  across discriminators.
- **I5 — Verification depth.** Exact set of non-determinism sources in
  the emit path beyond `setSourceFileName` — needs a focused audit
  (plan Phase 1).
