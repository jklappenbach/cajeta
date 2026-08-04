# allclasses-keep-all — reflective test discovery forced keep-all linking (defect)

Carried on the focus stack since the ml-arc session (2026-07-31):
`warning: [reflection-forces-keep-all] allClasses()` fired in every ml /
xgboost test-binary build, and the lean linker retained the whole class
registry.

## 1. Definition

Two stacked problems, one visible and one latent:

- 1.1 **Visible** — cajeta-unit's `Runner.runAll()` discovers `@Test`
  methods by enumerating `Class.allClasses()` and filtering per method. An
  unbounded enumerator can't be narrowed, so every lean link of every test
  binary degraded to keep-all. The warning was accurate; the API surface
  offered no bounded way to say "classes with a `@Test` method".
- 1.2 **Latent, worse** — a dependency's reflection sites live in method
  bodies the CONSUMER never re-codegens (classpath ingest is
  signature-only; the authoritative bitcode rides the archive), so the
  consumer's keep-set classifier never saw them at all. The ml/xgboost
  builds only linked correctly because their own TestMains happened to call
  `allClasses()` directly. Remove that line and a lean link would have
  silently stripped the test classes — Runner would have discovered
  nothing and reported green on zero tests.

## 2. Requirements / use cases

- 2.1 A bounded registry enumerator for method-level annotations: keep
  exactly the classes declaring a matching method.
- 2.2 A dependency's reflection demands must reach the consumer's keep-set
  computation — bounded sites keep narrowly, unbounded ones degrade to
  keep-all loudly with the warning attributed to the dependency. Never a
  silent strip.
- 2.3 cajeta-unit's Runner discovers through the bounded form; test
  binaries link lean with exactly the test classes kept.

## 3. RESOLVED 2026-08-04

- `Class.classesWithMethodAnnotated(String)` (+ token form `<@A>()`) in the
  stdlib; classified as `ReflSite::MethodAnnotated` (literal/token bounds,
  non-literal degrades loudly); keep-set resolution keeps classes with a
  matching method annotation (2.1).
- `.cja` archives carry `meta/reflection-keep.v1` — this build's
  code-driven reflection sites (build-mode forces like `--debug-info=full`
  are deliberately NOT exported) — merged into the consumer's accumulator
  at classpath ingest, forceall reasons prefixed `dependency <name>:`
  (2.2).
- cajeta-unit `Runner.runAll()` switched to
  `classesWithMethodAnnotated("code.Test")` — the literal at the call site
  is what the classifier bounds on. Self-suite green (19/0/1 + discovery
  3/0/1); its archive summary is exactly `methodannotated Test`.
  **Committed locally in cajeta-unit, NOT pushed**: its CI pins
  cajeta v0.15.0, which lacks the new enumerator — push together with the
  CI pin bump when the next cajeta release ships (2.3).

Pins: `ReflectionTests.classesWithMethodAnnotated{Filters,TokenForm}`
(runtime semantics, method-level only) and
`ReflectionKeepMethodAnnotatedTests` (lean keep-set narrows + provenance;
allClasses control still loud; dep summary round-trip both wide-loud and
bounded-narrow via keepset-json).
