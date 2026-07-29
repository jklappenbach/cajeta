# Compile-Cache (persistent stdlib prime) — Specification

> Status: **draft for review** (2026-06-25). A performance feature: make the test harness's
> stdlib **prime** persistent so a process reuses a cached artifact instead of recompiling the
> identical stdlib from scratch. Companion plan: `agents/cajeta/compile-cache-plan.md`.
>
> This is a **spec** — requirements + use cases (the *why/what*). The *how* is deferred as
> `> **TBD (plan-time):**` markers and collected in §9. Outline-numbered for addressability.
>
> **The one open fork that gates the design is §5 + [C1]/[C2]** — *what* is cacheable. Read that
> first; the rest is stable regardless of how it's resolved.

## 1. Definition

### 1.1 Purpose
The JIT test harness **primes** the stdlib once per process: parse + typecheck → IR-gen → JIT
native-compile of the *entire* embedded `cajeta.*` stdlib, so a test's embedded snippet can
resolve, typecheck, and JIT-execute against it. The BATCH sweep runs **~581 suite-processes**,
each re-priming the **identical** stdlib from scratch. That redundant prime is the dominant
per-process cost — and the direct cause of the failures that made the full sweep impractical:
the 32-shard run **OOM-killed**, the 24-shard run **thrashed** (24 simultaneous full-stdlib
compiles). This feature makes the prime **persistent and content-addressed**: when nothing that
affects the output changed, a process **loads** the cached prime instead of recompiling.

### 1.2 Scope
- The JIT/test prime path: `test/jit/JitTestHelper.cpp` `StdlibReuseCache::ensurePrimed()`.
- A **persistent, content-addressed** cache, reusing the build-tool `IrCache` + `SourceDigest`
  (`src/cajeta/buildtool/`) rather than a new cache.
- **Cross-process and cross-run** reuse (on-disk; survives between sweep processes and between
  sweeps / CI legs / dev loops).
- **Sound invalidation** — the cache must never serve an artifact that doesn't match the current
  stdlib source + compiler + flags + prelude.

### 1.3 Non-goals
- **The AOT `cajeta build` path** — `IrCache` already serves it; this spec is the *JIT/test* path.
- **Changing test semantics or results.** A cache hit must produce byte-identical behaviour to a
  cold compile.
- **Per-test user-snippet caching** in v1 — the shared **stdlib prime** is the dominant,
  duplicated cost; caching each test's own snippet is a later increment (§5.4, [C5]).
- **Replacing the in-process `StdlibReuseCache`** — that intra-process reuse stays; this adds the
  missing *cross-process* layer beneath it.

### 1.4 The prime, decomposed (this is the whole game)
`ensurePrimed()` today does, in order:
- **(a) Front-end** — `primeCompiler->ensureStdlibModule()`: parse + typecheck the stdlib into the
  Compiler's **in-memory type graph** (`CajetaType`/`CajetaModule`/`CajetaClass`, canonical maps,
  method/field tables). A test snippet *must* have this to resolve `import cajeta.xpu.X` and
  typecheck. **Not a flat artifact — a live C++ object graph.**
- **(b) IR-gen** — `runCodegenPasses(...)`: emit stdlib bodies + static inits into an **LLVM
  module**. Serializable as **bitcode** (`.bc`) — exactly what `IrCache` stores.
- **(c) JIT native-compile** — the ORC JIT lowers that IR to **machine code** on demand.
  Cacheable as **object files** via LLVM's ORC `ObjectCache`.

The three phases have **different cacheability** (b: easy, c: standard-LLVM, a: hard), and the
**front-end (a) is the crux**: the cheapest-to-cache phases (b/c) don't remove (a), so the win
ceiling depends on how much of the prime is (a) vs (b)+(c). **This must be measured before the
mechanism is chosen** (§5, [C1]/[C2]).

## 2. Cache key & invalidation (correctness is the first requirement)
The key reuses `computeCacheDiscriminator(compilerVersion, flags)` + a `SourceDigest`-style
content hash, extended with the **prelude-set**.

- **2.1** As the cache, when the **stdlib source** changes (any embedded `cajeta.*` file), then the
  digest changes and the prior entry is **not** used (recompile + re-store).
- **2.2** As the cache, when an **imported file** changes (transitively), then the digest changes
  (`SourceDigest` already folds sorted transitive-import digests).
- **2.3** As the cache, when the **compiler version** changes, then the discriminator changes
  (different sub-tree) — a newer compiler never loads an older compiler's IR/objects.
- **2.4** As the cache, when **stdlib-affecting flags / codegen options** change (the same set the
  `StdlibReuseCache` already gates reuse on), then the discriminator changes.
- **2.5** As the cache, when the **prelude-set** (`kStdlibRoots`) changes, then the key changes —
  because the prelude determines what is auto-imported and therefore what the prime compiles.
- **2.6** As the cache, when **flag order** differs but the set is identical, then the key is
  **unchanged** (the discriminator sorts) — order never busts the cache.
- **2.7** As a developer, when the key matches but the cached bytes are **corrupt/partial/truncated**
  (e.g. a killed writer), then the entry is treated as a **miss** (verified on load) and recompiled
  — a bad cache entry can never produce a wrong result or a crash.

## 3. Hit / miss lifecycle (the `ensurePrimed()` integration)
- **3.1** As a fresh suite-process, when it primes and the cache **hits**, then it **loads** the
  cached prime artifact and skips the corresponding compile phase(s) — producing a prime
  observably identical to a cold compile, ready to run the suite's tests.
- **3.2** As a fresh suite-process, when the cache **misses**, then it compiles as today **and
  stores** the artifact for subsequent processes/runs.
- **3.3** As one of N concurrent sweep processes, when many miss a **cold** cache at once, then
  stores are **atomic** (temp-file + rename, as `IrCache::store` already does) so a half-written
  entry is never read (a safety net even with §3.6).
- **3.6** As the sweep on a **cold** cache, **before** fanning out the shards, the harness runs a
  single **prime-once** step that populates the cache (one cold compile + store); the shards then
  all start **warm** — eliminating the cold thundering herd (24 simultaneous cold primes that
  OOM/thrash). **Decided 2026-06-25 (prime-once-then-fan-out).**
- **3.4** As a developer, when I run the sweep a **second** time (warm cache), then per-process
  prime cost collapses to a **load**, and the full sweep completes without the OOM/contention that
  blocked it cold (§6).
- **3.5** As a developer, when I want a clean slate, then `cajeta clean` / a documented env var
  wipes the JIT prime cache (reuse `IrCache::wipe`).

## 4. Reuse the build-tool cache infrastructure (don't build a second cache)
- **4.1** As the implementation, when it needs content-addressed storage, then it uses
  **`IrCache`** (`.cajeta/cache/ir/<discriminator>/<digest>.<ext>`; atomic store; LRU evict; wipe).
- **4.2** As the implementation, when it computes the source digest, then it uses
  **`SourceDigestRegistry`** over the stdlib roots (transitive-import aware).
- **4.3** As the implementation, when it computes the discriminator, then it uses
  **`computeCacheDiscriminator`**, feeding compiler-version + the stdlib-affecting flag-set + the
  prelude-set (§2.5).
- **4.4** As the maintainer, when `IrCache` lacks a needed capability (e.g. storing a whole-prime
  artifact vs per-file `.bc`, or a non-`.bc` extension), then `IrCache` is **extended** (it's our
  code), not duplicated.

## 5. What gets cached — the design fork (resolve via measurement)
This is the one decision the rest hangs on. The candidate artifacts, by increasing scope of win
**and** difficulty:

- **5.1 IR cache (skip phase b).** Cache the stdlib **bitcode**; a fresh process still runs the
  front-end (a) and JIT native-compile (c), but loads IR instead of regenerating it, then re-binds
  the loaded functions/globals onto the front-end classes (the `captureReuseBaseline` bindings).
  *Wins codegen; bounded by (a)+(c).*
- **5.2 + object cache (skip phase c).** Add an LLVM ORC **`ObjectCache`** so the JIT loads cached
  **machine code** instead of re-lowering. *Wins codegen + native-compile; bounded by (a).*
- **5.3 Front-end reuse (attack phase a).** The hard part — let a fresh process avoid re-running
  `ensureStdlibModule()`. Options: serialize the type-graph (large effort), or a **prime-server +
  `fork()`-per-test** model (a long-lived primed process forks COW children — shares (a)+(b)+(c)
  with zero serialization, but is a harness-architecture change). *Wins everything; biggest change.*
- **5.4 Per-test snippet cache** — later increment; out of v1 (§1.3).

**Requirement:** v1 must be chosen by **data**, not assumption — instrument the prime to report
the (a)/(b)/(c) split first ([C1]). If (b)+(c) dominate, 5.1+5.2 (the user's `IrCache` direction)
is the right, lowest-risk v1. If (a) dominates, 5.1/5.2 can't deliver and 5.3 (fork) is required.

- **5.5** As the maintainer, when the prime phase-split is measured, then the v1 mechanism is the
  one that caches the **dominant** phase(s) at acceptable risk.
- **5.6** As the implementation, whatever is cached, when a cache hit is used, then the resulting
  prime is **verified equivalent** to a cold prime (the existing `CAJETA_STDLIB_VERIFY` pristine
  check, §spec uses it as the oracle) before any test runs on it.

## 6. Performance acceptance (spec-level)
- **6.1** A **warm** full BATCH sweep's per-process prime cost drops to a **load** (target:
  ≥ the measured (b)+(c) fraction eliminated for 5.1/5.2; near-total for 5.3).
- **6.2** The **warm** full sweep **completes** at the standard shard count **without OOM** and in
  a fraction of the cold wall-clock — i.e. the cache turns the previously-impractical full sweep
  into a routine gate.
- **6.3** **Cross-run:** a second sweep (no source/compiler/flag change) is warm end-to-end.
- **6.4** **Cold first run** is no slower than today (cache miss = compile + one atomic store).

## 7. Correctness acceptance (spec-level)
- **7.1** A cache hit yields **byte-identical** test behaviour to `--no-cache` / a wiped cache
  (differential check on a representative suite set).
- **7.2** Every key input (§2.1–2.6) demonstrably busts the cache; a corrupt entry (§2.7) is a miss.
- **7.3** The full test suite passes **identically** warm vs cold (no result drift from caching).

## 8. Deliverables
- A persistent prime cache wired into `ensurePrimed()`, backed by extended `IrCache`/`SourceDigest`.
- A measurement of the prime phase-split that justified the chosen mechanism (§5.5).
- Key-correctness + warm/cold-equivalence tests (§7); a documented wipe/disable control.
- Evidence: a warm full sweep completing without OOM, and a cold-vs-warm timing delta.

## 9. Open questions (resolve at plan time)
- **[C1]** *(gating)* The prime phase-split — measure (a) front-end vs (b) IR-gen vs (c) native.
  Drives §5. **Do this first.**
- **[C2]** Front-end reuse: can a fresh process skip `ensureStdlibModule()` at all without a
  fork/serialize? If not, 5.1/5.2's ceiling is the front-end cost.
- **[C3]** Prelude-set canonicalization into the discriminator (§2.5).
- **[C4]** *(RESOLVED 2026-06-25 — prime-once-then-fan-out, §3.6)* On a cold cache the harness runs
  one prime-once step to populate the cache before fanning out shards, so the shards start warm; no
  thundering herd. (Atomic store §3.3 remains as a safety net.)
- **[C5]** Per-test snippet caching (§5.4) — scope for a follow-up.
- **[C6]** Whether the prime artifact is one whole-stdlib blob or per-file `.bc` reassembled
  (IrCache is per-file today; the prime is one module).
- **[C7]** The post-merge **multi-suite-in-one-process hang** (cross-suite `StdlibReuseCache`
  reuse) lives in this exact code — fix or account for it while here.
