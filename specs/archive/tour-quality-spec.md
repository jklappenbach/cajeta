# Tour quality — spec

Reviews of every tour project in the cajeta ecosystem — the stdlib tour and the
tour of each library published to Olla — plus authoring standards and coverage
tooling so a tour is verifiably comprehensive, not vestigial.

## 1. Definition

- 1.1 **Purpose.** A tour is executable best-practice documentation: a runnable
  project that demonstrates a library from its entry points, comprehensively
  enough that a junior developer can learn the library by reading and running
  it. Tours are the "show me" companion to `docs/` reference material.
- 1.2 **Problem.** The stdlib tour is mature (90 files, ~8.1k lines, package
  coverage enforced by `scripts/check-tour-coverage.sh`). The library tours are
  not — several are single files that cannot plausibly cover their library's
  surface:

  | Package (Olla) | Version | Tour | Size |
  |---|---|---|---|
  | (stdlib) | 0.12.0 | `cajeta/samples/tour` | 90 files, 8149 lines |
  | dev.cajeta.codec | 0.7.1 | `cajeta-codec/samples/tour` | 5 files, 243 lines |
  | dev.cajeta.http | 0.1.2 | `cajeta-http/samples/tour` | 1 file, 341 lines |
  | dev.cajeta.logging | 0.6.0 | `cajeta-logging/samples/tour` | 3 files, 141 lines |
  | dev.cajeta.unit | 0.2.0 | `cajeta-unit/samples/tour` | 12 files, 366 lines |
  | dev.cajeta.xgboost | 0.1.0 | `cajeta-xgboost/tour` | 1 file, 244 lines |

  cajeta-codec advertises five formats (protobuf, ion, avro, parquet, orc) in
  243 lines; cajeta-http advertises HTTP/1.1·2·3 + WebSocket + SSE client and
  server in one file. No tour has coverage measurement.
- 1.3 **Scope.** (a) A code review of the stdlib tour in the cajeta repo.
  (b) A code review of the tour in each of the five Olla-published libraries.
  (c) Expansion or rewrite where a review finds the tour non-comprehensive;
  authoring a tour from scratch where one is effectively absent. (d) A
  library-repo variant of the coverage check so comprehensiveness is enforced,
  not asserted.
- 1.4 **Quality bar.** Tours demonstrate the library's full public surface,
  starting from the entry points of library classes. Realistic scenarios over
  toy snippets: unless only a trivial demonstration is possible, each demo
  models a real-world use ("parse this service's access log", not "print a
  string"). As many use cases as needed to establish best practices — including
  error handling, resource lifetime/ownership sigils, and configuration —
  per the no-half-measures policy.
- 1.5 **Non-goals.** Tours are not test suites (cajeta-unit and CI own
  correctness) and not reference docs (`docs/` owns that). cajeta-ml's tour is
  owned by the active cajeta-ml plan and is out of scope here; it inherits this
  spec's standards when it publishes. Unpublished libraries (gossip, cluster,
  robotica, …) are out of scope until they publish.
- 1.6 **Constraints.** All tours must compile and run green against the pinned
  toolchain (cajeta 0.12.0) via each repo's `run-tour.sh` (or equivalent task).
  Tour style follows the stdlib tour's conventions (registered demos, one
  package/topic per file).

## 2. Review methodology

Every tour review evaluates the same five dimensions and produces a written
findings report (gaps enumerated per class/method) before any code changes.

- 2.1 **Coverage** — as a reviewer, when I diff the library's public model
  (`cajeta doc --emit-model-json`) against the tour's imports and calls, then
  every public entry-point class is exercised and uncovered surface is
  enumerated in the findings.
- 2.2 **Correctness** — as a reviewer, when I run the tour against the pinned
  toolchain, then it compiles and runs green; anything red is a finding (tour
  bug or library/compiler defect — defects get their own INDEX row, not a
  workaround in tour code).
- 2.3 **Best practices** — as a reviewer, when I read each demo, then it shows
  the idiomatic pattern: ownership sigils used correctly, errors handled (not
  swallowed), resources closed, configuration surfaced. A demo teaching an
  anti-pattern is a finding even if it runs.
- 2.4 **Pedagogy** — as a junior developer, when I read a tour file top to
  bottom, then I can follow it without prior knowledge of the library: demos
  progress simple → advanced, each states in comments what it demonstrates and
  why the pattern is right.
- 2.5 **Realism** — as a reviewer, when a demo could model a real scenario but
  instead exercises the API with throwaway values, then that is a finding
  (1.4's bar: trivial only when nothing realistic exists).

## 3. Coverage tooling

- 3.1 As a library maintainer, when I run a repo-local coverage check (the
  library analogue of `check-tour-coverage.sh`, driven by
  `cajeta doc --emit-model-json` over the library source vs tour imports),
  then uncovered public classes fail the check with a list.
- 3.2 As a maintainer, when the library's CI runs, then the tour is built, run,
  and coverage-checked on every push — a tour that rots goes red.
- 3.3 As the stdlib maintainer, when the existing `check-tour.sh` /
  `check-tour-coverage.sh` gate runs, it keeps working unchanged (the library
  variant generalizes it; it does not replace it).

## 4. Stdlib tour review (cajeta repo)

- 4.1 As a reviewer, when the methodology of §2 is applied to
  `samples/tour` (90 files), then the findings report covers all five
  dimensions — coverage is already gated (§3.3), so emphasis falls on best
  practices, pedagogy, and realism of the existing demos.
- 4.2 As a developer, when the review finds demos that predate current idiom
  (e.g. pre-sigil-retirement ownership patterns, superseded APIs), then those
  demos are modernized to today's best practice.
- 4.3 As a developer, when the review finds stdlib surface added after the
  tour's last expansion (0.12.0 additions: dense linalg solvers, Stats special
  functions), then demos are added for it.

## 5. Library tour reviews and expansion

One review + remediation per published library, methodology of §2, quality bar
of 1.4, coverage tooling of §3 installed in each repo.

- 5.1 **cajeta-codec** — as a junior developer, when I read the codec tour,
  then each advertised format (protobuf, ion, avro, parquet, orc) has
  realistic round-trip demos: schema definition, encode/decode, streaming
  where the format supports it, and error/corrupt-input handling.
- 5.2 **cajeta-http** — as a junior developer, when I read the http tour, then
  client and server sides are each demonstrated for HTTP/1.1, HTTP/2,
  WebSocket, and SSE — realistic handlers (routing, headers, status codes,
  timeouts, TLS configuration where applicable), not a single echo round trip.
  (HTTP/3 was advertised but is unimplemented in the library; decision
  2026-07-30: de-advertise — the remediation unit drops the claim from the
  library's manifest/README rather than demoing vaporware.)
- 5.3 **cajeta-logging** — as a junior developer, when I read the logging tour,
  then levels, appenders, pattern layouts, and the DI-wired LoggerFactory are
  each demonstrated in a realistic service-logging scenario (per-module
  loggers, file + console appenders, level filtering).
- 5.4 **cajeta-unit** — as a junior developer, when I read the unit tour, then
  @Test discovery, the assertion vocabulary, lifecycle hooks, @Disabled, and
  mocks are each demonstrated by testing a small realistic subject, showing
  test-design best practice (fresh-instance semantics, one behavior per test).
- 5.5 **cajeta-xgboost** — as a junior developer, when I read the xgboost tour,
  then training, prediction, and model round-trip are demonstrated on a
  realistic dataset for both regression and multiclass, including the
  QuantileDMatrix-equivalent data-preparation path and parameter
  configuration.
- 5.6 As a maintainer, when a review concludes a tour is effectively absent or
  unsalvageable (scope 1.3c), then a tour is written from scratch to this
  spec's standards rather than patched.

## 6. Acceptance

- 6.1 As the developer, when this work closes, then every published library
  and the stdlib have: a findings report from the §2 review, a tour meeting
  the 1.4 bar, green `run-tour.sh` on the pinned toolchain, and an installed
  coverage gate (§3) wired into that repo's CI.
