# Cajeta coverage tour

Every finding `cajeta-coco` can report, one class each — and what each one
looks like in the IDE.

This is a **deliberately imperfect project**. Its suite is green and every
assertion in it holds; that is the point. A green suite is exactly the
situation where coverage analysis has something to say, and this project is
shaped so it says all of it at once:

| Class | Finding | The fix |
|---|---|---|
| [`Pricing`](src/tour/coco/Pricing.cajeta) | **none** — the control | — |
| [`Shipping`](src/tour/coco/Shipping.cajeta) | covered, mutants survive | an **assertion** |
| [`Coupon`](src/tour/coco/Coupon.cajeta) | uncovered, reachable | a **test** |
| [`LegacyPricing`](src/tour/coco/LegacyPricing.cajeta) | uncovered, unreachable | `git rm` |
| [`TaxTable`](src/tour/coco/TaxTable.cajeta) | top of the risk queue | a test, **first** |
| [`CouponTests`](src/tour/coco/tests/CouponTests.cajeta) | a test with no unique coverage | a question worth asking |

The control matters as much as the other five. A tour of findings is worthless
without one class that produces none — it is what proves the rest are findings
rather than noise.

## Run it

```bash
./run-coco.sh          # cajeta cover, then walk the artifacts
```

or the build-tool task on its own:

```bash
cajeta cover           # instrument, run the suite, report
cajeta test            # the suite with no instrumentation (the fast loop)
```

`cover` takes a few minutes the first time: `instrument` runs two front-end
passes (`--emit=exe` for a linkable stdlib object carrying the synthesized C
`main`, `--emit=ir --emit-xref` for the modules coco instruments and the
reachability index), and each costs ~80 s regardless of project size.
Everything after that is coco's own and takes under a second. Re-rendering
never repeats any of it.

The gate (`config.min`) is set to 30, just under this project's real 36.0%,
so `cover` succeeds. Raise it to 40 and re-run to watch it fire:

```
cajeta cover: task 'cover' actions[1] (cajeta.coverage.report):
coverage 36.0% is below min 40%
```

## Layout

```
samples/tour/coco/
├── cajeta.json                       ← the whole configuration, commented
├── run-coco.sh                       ← cover + a walk through the findings
└── src/tour/coco/
    ├── CocoTour.cajeta               ← entry: Runner.runAll()
    ├── Pricing.cajeta                ← the control
    ├── Shipping.cajeta               ← surviving mutant
    ├── Coupon.cajeta                 ← reachable but untested
    ├── LegacyPricing.cajeta          ← dead
    ├── TaxTable.cajeta               ← high CRAP
    └── tests/                        ← excluded from instrumentation
        ├── PricingTests.cajeta
        ├── ShippingTests.cajeta      ← under-asserted, on purpose
        ├── CouponTests.cajeta        ← one redundant test
        └── TaxTableTests.cajeta
```

The tests are **excluded from instrumentation** (`config.exclude` in
`cajeta.json`) for two reasons. Their lines are executed by definition, so
leaving them in inflates the percentage with code that cannot fail to be
covered. Less obvious: an instrumented test body owns lines nothing else
touches, so *every* test would look like it contributes unique coverage and
the redundancy signal `CouponTests` exists to produce would be destroyed.

Excluding does **not** remove code from the program — the modules are still
compiled and still linked. Only the probes are withheld.

## The findings, one at a time

### 1. Covered, but not verified — `Shipping`

`ShippingTests` calls `rateCents` twice; both assertions pass; every line is
green. And the threshold's inclusivity is untested, because neither input is
the threshold value. coco swaps the `>=` for a `>`, relinks, reruns — and the
suite still passes. **SURVIVED.**

Coverage cannot see this. Execution is not verification, and a green line
makes no claim about what was asserted.

`ShippingTests` carries the fix commented out rather than deleted: uncomment
`theThresholdItselfShipsFree`, re-run, and the survivor disappears.

> **IDE:** green gutter, plus a row in the coco tool window's **Mutants** tab.
> A covered line whose mutants survive is a different problem from an
> uncovered line, and is shown as one.

### 2. Uncovered and reachable — `Coupon.isExpired`

Nothing in the suite reaches it, so it is red. But its class is demonstrably
live — `applyTo` runs under two tests — and coco refuses to call anything on a
live class dead: compile-time DI and reflection reach members through
synthesized paths no static call graph records.

> **IDE:** red gutter, and a **Dead Code** row classified `NEEDS A TEST` with
> the reason stated rather than left to infer.

### 3. Uncovered and unreachable — `LegacyPricing`

Nothing names this class. No test calls it, no production method calls it, the
cross-reference index has no path to it from the entry point or from any
executed method, and no method on it ran — so there is no evidence of a
reflective caller either.

An lcov report shows this in exactly the same red as `Coupon.isExpired`. That
equivalence is the busywork machine: it demands tests for code whose correct
treatment is deletion, and buries the real gaps in the noise. Separating the
two is coco's reason to exist.

> **IDE:** a **Dead Code** row classified `DELETION CANDIDATE`, with Safe
> Delete available on it.

### 4. Where to start — `TaxTable.rateBasisPoints`

`Coupon.isExpired` and `TaxTable.rateBasisPoints` are both uncovered, and a
flat list ranks them as equal work. They are not equal work. CRAP says so in
one number:

```
CRAP(m) = complexity(m)^2 * (1 - coverage(m))^3 + complexity(m)
```

Complexity squared against the coverage gap cubed: complex-and-untested rises,
simple-and-untested sinks, complex-but-tested is fine. The conventional
threshold is 30. `Pricing.discountCents` sits near the floor; the rate table
is far above it.

> **IDE:** the **Risk** tab, worst first, each entry reading
> `complexity N, M% line coverage → CRAP S` so the ranking is explicable
> rather than an oracle.

### 5. A test that buys nothing — `CouponTests`

`Coupon.applyTo` is straight-line, so both of its tests run exactly the same
lines. The second one's unique-coverage set is empty.

That is not automatically a reason to delete it — a second input can still be
worth asserting — but it is the question worth asking, and per-test
attribution is the only thing that can raise it.

Attribution exists because coco patches `dev.cajeta.unit`'s `Runner` in the
bitcode it links, so each test dumps its own probe delta. The framework is
unmodified. Without a recognised runner on the classpath the run still works
and the tab says the data was **not collected** — which is a different claim
from an empty result.

From `attribution.tsv`:

```
# test tour.coco.tests.CouponTests::appliesTheDiscount               covered=3 unique=2
# test tour.coco.tests.CouponTests::appliesTheDiscountToAnotherAmount covered=1 unique=0
```

**Zero unique coverage does not mean delete**, and this project proves it in
the same file: `PricingTests::discountAppliesAtTheThreshold` also reports
`unique=0` — and it is the single most valuable test here, the only one that
kills `Pricing`'s mutant. Attribution raises the question; mutation answers it.

> **IDE:** the **Tests** tab. Select a test to see the lines it uniquely
> covers; one with none reads `N lines, none unique — redundancy candidate`.

## In the IDE

Open this directory in IntelliJ IDEA with the Cajeta plugin.

- **Run with Coverage** on the `cover` task configuration runs the instrument
  action and loads the result. Gutters, line highlighting, the Coverage tool
  window and per-directory rollups are IntelliJ's own — coco is registered as
  a coverage engine, not reimplemented on top of one.
- **Load Cajeta Coverage** (in the platform's Coverage menu) loads a run that
  already happened. `cajeta.json` already says where the artifacts are, so
  there is nothing to browse for. Loading never re-runs.
- **View ▸ Tool Windows ▸ Cajeta Coverage** carries the four views
  IntelliJ's coverage model — lines, hit counts, branches — structurally
  cannot express: Dead Code, Tests, Risk, Mutants. It only appears for a
  project whose manifest configures coco, which this one does.
- **Edit a measured file** after a run and its results are marked stale, per
  file rather than per project: the common case is one file changed out of
  many, and invalidating everything would throw away results that are still
  good.

## Artifacts

Everything lands under `build/coco/` (gitignored):

| Path | What it is |
|---|---|
| `coverage.html` | The report — percentages, dead-vs-untested, the ranking |
| `annotated.html` | Your source, line by line, red and green |
| `sites.tsv` | `coco-sites v1` — the probe table |
| `run/coco.merged.profile` | `coco-profile v1` — hit counts, attribution-merged |
| `run/coco-test-NNNN.profile` | One test's probe delta |
| `attribution.tsv` | Which test covered which line |
| `crap.tsv` | `coco-crap v1` — the risk ranking |
| `mutation.tsv` | Mutation verdicts (written by `coco mutate`) |
| `xref.json` | The cross-reference index reachability is computed from |
| `lcov.info`, `coverage.sarif` | Interop exports for CI |

`sites.tsv` and a profile are a **pair** — probe ids are positional against
the table — so a profile read against another run's table silently attributes
hits to the wrong lines. Tools that consume these refuse to guess.

## Mutation

```bash
cajeta mutate          # runs OVER the last `cover`; never instead of one
```

```
coco: baseline exit 0
coco: mutation score: 2/3 killed (9 skipped as uncovered)
coco: SURVIVING mutants — these behavior changes went unnoticed by the suite:
  tour/coco/Shipping.ll:25  sge->sgt in tour.coco.Shipping::rateCents(subtotalCents:int64)
```

Read that against `Pricing`: the *same* `sge->sgt` swap, and it is **killed**,
because `PricingTests` asserts the boundary. One line of test code is the whole
difference between the two classes.

coco's operator set is `icmp` predicate swaps — `sgt↔sge`, `slt↔sle`,
`ugt↔uge`, `ult↔ule` and `eq↔ne`. They touch one token, can never invalidate
the module, and target exactly the off-by-one a boundary-blind test fails to
look at. Verdicts are three-way: `killed`, `SURVIVED`, and
`skipped-uncovered` — the last is excluded from the score rather than counted
against the suite, because an unexecuted compare cannot be killed.

A mutant is one `llc` plus one relink plus one suite run: the relink replays
the link line `cover` recorded in `link.tsv` with exactly one object swapped,
so a mutant is the measured program rather than a near-miss of it. Only
instrumented modules are mutated — the set comes from `sites.tsv`, so the test
package this project excludes is never mutated. (A suite cannot kill a change
to its own assertions; before that gate existed, every one of this project's
nine test-assertion mutants was reported as a survivor.)

## Known noise

`Runner.runAll()` discovers `@Test` methods reflectively across the whole
binary, and `dev.cajeta.unit` 0.2.0 ships its own selftest inside the archive
— so `dev.cajeta.unit.selftest.discovery.ExampleTest` appears in the run
report alongside this project's tests. It is excluded from *measurement*
(`dev/cajeta/unit/` is in `config.exclude`), so it affects no number here —
its three tests show `covered=0 unique=0` in `attribution.tsv`.

Separately, and now **fixed**: both killed mutants used to die by
**segfaulting** rather than by a failed assertion.

It was not the mutation and not the probes. Measured on identical objects with
only coco's per-test attribution hook swapped in and out, 3/3 each way: hooked
→ SIGSEGV at the first failing test; unhooked → both failures reported, exit 1.
`addr2line` put the fault inside `dev.cajeta.unit`'s `Runner.runOne`, walking
its own drop chain on the path where an exception was caught. `runOne` kept
`msg = e.message` — a **borrow** of the caught exception's message — past the
catch scope that owns `e`; the hook's allocation and file write turned that
latent use-after-free into a fault.

Fixed in **cajeta-unit 0.2.3**, which copies the message instead of borrowing
it. This project pins that version as its floor. Verified: a coverage run over
a suite with one deliberately-failing assertion now reports `11 passed,
1 failed`, merges all 12 per-test profiles, and returns a suite-failed error —
where before it wrote no profile at all.

Confirmed on the other side too: a killed mutant now dies the way it should —
`✗ FAIL … expected <1000> but was <0>`, exit 1 — instead of SIGSEGV. The
verdict was always right (the exit code moved either way), but now the signal
means what it says.

The lesson generalises past this bug: a green suite never executes the failure
path, so nothing about a passing run could have found it. It took a *consumer*
that allocates inside the framework's own code to make it observable.

## See also

- [Guide: 23 — Code coverage](../../../docs/guide/23-code-coverage.md) — the
  full configuration reference: every setting, where results land, how to
  invoke a pass.
- [BuildTool.md § Code coverage](../../../docs/specification/buildtool/BuildTool.md)
  — the plugin in the context of the action catalog.
- The other tour sub-projects: [`xpu/`](../xpu/README.md),
  [`ifx/`](../ifx/README.md), [`tls/`](../tls/README.md).
