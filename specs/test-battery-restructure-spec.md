# test-battery-restructure — stress split, overlap folds, 90% coverage

## 1. Definition

Opened 2026-08-10 from the per-test coverage analysis
(`tools/coverage/ANALYSIS-2026-08-10.md`, data `.coverage/`). Measured: 5,868
tests; 81% hold zero unique compiler lines; the zero-unique tests consume 92%
of suite time; 879 tests retain 99.5% of covered lines; 197 of 455 compiler
files are untouched. Developer directives (verbatim intent):

- **1.1 Stress split.** "Move the stress tests out of the sweep and into
  stress tests. We should exercise these every so often, but not as part of
  the development cycle."
- **1.2 Overlap folds.** "Optimize the overlaps, fold as necessary, identify
  where overlap exists and eliminate the overlap."
- **1.3 Coverage.** "Provide coverage for areas not covered. We should shoot
  for 90% coverage."

## 2. Stress split (§1.1)

- **2.1** A stress test is one whose repetitions/scale exist to probe load,
  not functionality: the measured whales with zero unique lines
  (ConcurrentCompileTests.stressManyConcurrentCompiles, NumpyOps interop at
  885s, benchScaleStaysBounded, thousand-key round trips, GradAll/Fuse scale
  loops) plus anything name- or shape-identified (stress/bench/Scale/
  Thousand/churn).
- **2.2** Mechanism: `test/stress_filter.txt` (one `Suite.test` per line).
  The sweep EXCLUDES these by default; `./cajeta_tests.sh stress` runs ONLY
  them. No test is renamed or deleted — membership is data, reviewable in one
  file.
- **2.3** Where a stress test is the only holder of a functional path (e.g.
  HashMap resize needs ≥2 resizes), a CAPPED functional twin stays in the
  sweep (the n=40 probe shape) and the full-scale version moves to stress.
- **2.4** Cadence: stress battery runs pre-release and on demand — not per
  development cycle.

## 3. Overlap folds (§1.2)

- **3.1** Fold within suite families whose members share an identical or
  near-identical footprint (the measured clusters): the nucleo table family
  (Pushdown/ZoneMap/BPlusIndex/GroupBy/NullSemantics/Rolling/Resample/
  SortJoin/FilterSelect/TableArrow/TableCore — ~18.8k-line shared sets),
  FuseAllocation+FuseSugar (byte-identical sets), CarameloSpatialIndexDevice
  (~20 × 9-line tests).
- **3.2** The fold pattern is `stringElementModeMatrix`: one compiled program
  carrying the family's assertions, replacing N single-assertion compiles.
  Assertions are PRESERVED — line coverage is not behavior; nothing is
  dropped solely for adding no lines.
- **3.2b Use-case discipline (developer directive, 2026-08-10).** No test
  exists "just to test": each unit of functionality has a purpose — a
  use-case — and a test must trace to one. Where multiple use-cases overlap,
  COMBINE them into one program and let the NAME declare what is tested: the
  convention is `<subject><UseCase>Matrix` / `<subject><UseCaseA>And<UseCaseB>`
  (e.g. `stringElementModeMatrix` = every String element store mode;
  `stringKeyResizeRehashRoundTrip` = keys survive rehash across resize).
  A reader must be able to tell FROM THE NAME which use-cases a folded test
  carries; a test whose name cannot say what purpose it serves is a candidate
  for folding or removal.
- **3.3** Acceptance per fold: the folded test(s) cover ≥ the union of the
  replaced tests' lines (diff via `.coverage/index`), all replaced assertions
  present, suite time reduced by the measured redundancy.
- **3.4** The routine gate becomes: covering set (879) + pinned
  assertion-bearing tests + folded families; the full battery stays available
  (nightly/pre-release).

## 4. Coverage to 90% (§1.3)

- **4.1** Denominator first: the index records covered lines only. Extend it
  to record each file's executable-line total (gcov already emits it) so
  "90%" is measured, not asserted. Baseline % unknown today.
- **4.2** Blind spot before new tests: instrument the CLI (`cajeta` target in
  build-cov, subprocess GCOV_PREFIX pass-through) so buildtool/CLI-driven
  suites attribute their real coverage; re-measure before writing tests for
  those areas.
- **4.3** New harnesses for the genuinely untouched subsystems, largest
  first: debugger stack (DAP server + DebugController/TypeTable/
  ValueInspector, ~2,700 lines), ObligationReplay (encode the documented D1
  3-command repro), SynthesizedMockClass, CajetaJitHost (confirm
  reachability first), remaining worst-covered files from `report.txt`.
- **4.4** Device-gated files (OptixAccel) count against the target only on
  runners with the device; excluded from the 90% denominator elsewhere.
- **4.5** Target: ≥90% of executable lines in `src/cajeta/` on the standard
  runner, measured by the extended index over the full battery.

## 5. Non-goals

- **5.1** Deleting zero-unique tests wholesale — folds preserve assertions.
- **5.2** Chasing 90% through the generated ANTLR front end or headers-only
  inline noise; the metric is src/cajeta executable lines as indexed.

## 6. Acceptance

- **6.1** Sweep excludes stress by default; `stress` target runs them; CI
  cadence documented.
- **6.2** Routine gate wall time reduced ≥50% vs the 2026-08-10 baseline at
  equal-or-better line coverage.
- **6.3** Measured coverage ≥90% per §4.5.
