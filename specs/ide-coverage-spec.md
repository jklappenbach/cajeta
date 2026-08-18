# ide-coverage — coco coverage in the IDE (spec)

> Status: **draft, pending approval**. Authored with the **design** skill.
> The actionable *how* lives in `agents/ide-coverage-plan.md`.
>
> **Spans two repos.** The format half lands in `cajeta-coco`; everything else
> lands in `ide-plugins/idea` here. Documented in one place because the format is
> the seam between them — the same reason `gpu-probe.yml` lives in this repo
> while its sources live in `cajeta-xgboost`.
>
> Third sibling to [`cajeta-profiler`](cajeta-profiler-spec.md) and
> [`memory-tooling`](memory-tooling-spec.md), and the smallest of the three:
> coco already does the hard part.

---

## 1. Definition

### 1.1 Purpose
Show `cajeta-coco`'s coverage, reachability, risk and mutation results inside the
IDE — gutters and percentages through IntelliJ's own coverage subsystem, and the
analysis coco is actually built for through a dedicated view.

### 1.2 The problem it solves
coco produces coverage, dead-code classification, CRAP ranking, per-test
attribution and mutation results, and renders them to HTML. Nothing surfaces any
of it where the code is edited. A developer reads a report in a browser, then
switches to the editor and reconstructs from memory which lines were red.

The commodity half of that — red and green lines — is what every coverage tool
has had for two decades. The half worth building for is coco's: **uncovered code
has two completely different causes**, statically unreachable (delete it) and
reachable but untested (write a test), and conflating them is what makes coverage
reports generate busywork.

### 1.3 Scope
1. **A published format contract** — coco's two native formats documented as an
   interface, with a compatibility rule and a conformance fixture.
2. **A `CoverageEngine` implementation** so IntelliJ renders gutters, line
   highlighting, the Coverage tool window and per-directory rollups natively.
3. **Run with Coverage** wired to coco's pipeline.
4. **A coco view** for what IntelliJ's coverage model cannot express: dead vs
   untested, per-test attribution, the CRAP queue, mutation survivors.
5. **Staleness handling**, so results are never silently shown against changed
   source.

### 1.4 Non-goals
1. **Reimplementing coverage rendering.** IntelliJ's coverage subsystem owns
   gutters, highlighting and rollups; this consumes it.
2. **Consuming `lcov.info`.** It is a derived export and cannot express any of
   coco's differentiators (§1.5.2). It remains coco's interop path for CI,
   Codecov, `genhtml` and diff-coverage bots.
3. **Moving coco into the IDE.** coco is a build-time engine, as JaCoCo is; the
   IDE is a consumer of its output, as IDEA's JaCoCo support is.
4. **Instrumentation changes.** coco owns instrumentation entirely.
5. **A second IntelliJ plugin.** This is part of the existing Cajeta plugin, which
   already owns the file type, PSI, navigation and the xref index a coverage view
   depends on.

### 1.5 Constraints

**1.5.1 The formats are the seam.** coco's `coco-sites v1` and `coco-profile v1`
are already version-stamped. Once the IDE reads them they are a contract across
two repos on independent release cycles, and must be treated as published rather
than as internal details that happen to hit disk.

**1.5.2 IntelliJ's coverage model is lines, hit counts and branches.** It has no
representation for statically-unreachable, per-test attribution, risk score or
mutation outcome. Those require a separate surface; there is no way to express
them through the coverage engine.

**1.5.3 Instrument once, report many times.** coco persists the probe map beside
the instrumented IR specifically so reporting is decoupled from instrumenting.
The IDE inherits that: re-rendering, switching to per-test attribution, or
changing a threshold must never require a rebuild.

**1.5.4 The plugin targets IntelliJ IDEA Community.** No CLion-only API.

**1.5.5 Naming.** `dev.cajeta.coco` (library) and `dev.cajeta.coverage`
(build-tool plugin) are taken. IDE-side identifiers must be distinct from both.

---

## 2. The format contract

coco's half. Independent of all plugin work and landable on its own.

- **2.1** When either native format is emitted, it carries a version marker —
  already true of `coco-sites v1` and `coco-profile v1`.
- **2.2** When a reader encounters a version it does not know, it refuses the file
  and says so, rather than parsing what it recognizes and silently dropping the rest.
- **2.3** When the formats change incompatibly, the version is incremented.
- **2.4** When a consumer is written, a conformance fixture — a known-good
  sites/profile pair with its expected interpretation — is available to test against.
- **2.5** When the formats are documented, the documentation states which fields
  are stable and what a consumer may assume.
- **2.6** When `attribution.tsv` is consumed, it is covered by the same contract.

## 3. Coverage engine integration

The commodity layer, delivered through IntelliJ's own subsystem so the rendering
is native rather than reimplemented.

- **3.1** When a coco run's output is loaded, covered and uncovered lines are
  marked in the editor gutter of the corresponding `.cajeta` file.
- **3.2** When a coverage suite is active, the Coverage tool window lists files and
  directories with their percentages.
- **3.3** When coverage is displayed, line, branch and function metrics are all
  represented, matching what coco measured.
- **3.4** When a line carries several probes, its displayed hit count is the
  maximum over them, never the sum — summing would invent executions.
- **3.5** When a branch arm was never evaluated, it is shown as unevaluated rather
  than as zero-hit.
- **3.6** When compiler-inserted guard branches exist, they are excluded, because
  coco excludes them from measurement.
- **3.7** When a coverage suite is closed, all markings are removed.
- **3.8** When more than one run is available, suites can be selected between.

## 4. Running coverage from the IDE

- **4.1** When Run with Coverage is invoked on a Cajeta run configuration, coco's
  pipeline executes and the results load automatically on completion.
- **4.2** When the run fails, the failure is reported with coco's output rather
  than silently producing no coverage.
- **4.3** When coco is not available to the project, the action explains what is
  missing rather than failing obscurely.
- **4.4** When a run completes, its artifacts are retained so results can be
  re-examined without re-running.
- **4.5** When results already exist on disk, they can be loaded without a run
  (§1.5.3).

## 5. Staleness

Coverage measured against source that has since changed is worse than no
coverage: it is confidently wrong. The plugin already solves this shape of
problem for the xref index and should follow that precedent.

- **5.1** When source has changed since the coverage run, the display indicates
  that results are stale.
- **5.2** When a file's coverage cannot be trusted because that file changed, its
  markings are suppressed or flagged rather than drawn against shifted lines.
- **5.3** When staleness is detected, re-running is offered.
- **5.4** When results are loaded, their age and origin are discoverable.

## 6. The coco view

Everything §1.5.2 rules out of the coverage engine. This is the reason to build
rather than point an lcov viewer at the export.

### 6.1 Dead vs untested
coco's headline capability: intersecting static reachability from the xref index
with dynamic coverage separates code that needs a test from code that needs
deleting.

- **6.1.1** When uncovered code is shown, it is classified as statically
  unreachable or as reachable-but-untested.
- **6.1.2** When code is statically unreachable, that is presented as a deletion
  candidate, not as a coverage gap.
- **6.1.3** When code is reachable but untested, the classification is stated so a
  developer knows a test is the fix.
- **6.1.4** When a classification is shown, navigating to the code is one action.
- **6.1.5** When reachability cannot be determined, that is stated rather than
  defaulting to either classification.

### 6.2 Per-test attribution
- **6.2.1** When a covered line is inspected, the tests that exercised it can be
  listed.
- **6.2.2** When a test is selected, the lines it uniquely covers can be shown.
- **6.2.3** When a test contributes no unique coverage, it is identifiable as a
  redundancy candidate.
- **6.2.4** When attribution data was not collected for a run, the view says so
  rather than showing an empty result.

### 6.3 Risk ranking
- **6.3.1** When methods are ranked by CRAP score, the ranking is shown worst
  first, turning an open-ended hunt into an ordered queue.
- **6.3.2** When a ranked entry is selected, navigating to the method is one action.
- **6.3.3** When a score is shown, its inputs — complexity and coverage — are
  visible, so the number is explicable rather than opaque.

### 6.4 Mutation results
- **6.4.1** When mutation results exist, surviving mutants are listed with their
  location and the mutation applied.
- **6.4.2** When a survivor is selected, navigating to the mutated site is one
  action.
- **6.4.3** When a line is covered but its mutants survive, that is distinguishable
  from an uncovered line — execution without verification is a different problem
  from no execution.

## 7. Performance

- **7.1** When a large project's results are loaded, the editor stays responsive.
- **7.2** When results are re-rendered with a different view or threshold, no
  rebuild or re-run occurs (§1.5.3).
- **7.3** When results are parsed, parsing happens off the UI thread.
- **7.4** When coverage is not in use, there is no cost to ordinary editing.

---

## 8. Resolved decisions

- **8.1 Adopt `CoverageEngine`.** Decided 2026-08-17. The alternative — painting
  gutters directly and putting everything in one custom window — was rejected in
  favour of the native UX: Run with Coverage, the Coverage tool window and
  per-directory rollups behave as users expect. Two surfaces result either way,
  because of §1.5.2.
- **8.2 Read the native formats, not `lcov.info`.** LCOV is a derived rendering
  and drops every differentiator (§1.4.2).
- **8.3 The IDE code lives in the Cajeta plugin, not in coco.** coco has no
  Kotlin, Gradle or IntelliJ Platform toolchain, and a coverage view cannot stand
  alone — it needs the file type, PSI, navigation and xref index the language
  plugin owns. This follows the Java precedent exactly: JaCoCo is not an IntelliJ
  plugin; IDEA's JaCoCo support ships with IDEA's Java support.
- **8.4 The format is the product boundary.** What decouples the two repos is the
  published format, not the location of the reader (§2).

## 9. Open questions

- **9.1** Whether the coco view is one tool window with tabs or several. One window
  keeps related analysis together; separate windows suit different workflows.
- **9.2** Whether dead-code candidates should offer a delete action, or only
  navigation. coco ships `delete_tests.py` for the test side, so there is
  precedent for acting rather than only reporting — but deleting source from a
  coverage view is a large gun.
- **9.3** Whether mutation results warrant gutter markers of their own, or belong
  only in the view. A covered-but-mutation-surviving line is arguably the most
  actionable marker a coverage tool can draw.
- **9.4** Whether the conformance fixture lives in coco, in the plugin's test
  resources, or is shared. Shared is correct in principle and awkward across two
  repos in practice.
