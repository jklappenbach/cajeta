# 23 — Code coverage

Coverage for Cajeta is `cajeta-coco`, reached through the
`dev.cajeta.coverage` build-tool plugin. It measures what ran, and then does
the thing a coverage percentage cannot: it says **why** a line is uncovered.

Uncovered code has two completely different causes — statically unreachable
(the fix is `git rm`) and reachable but untested (the fix is a test) — and
conflating them is what makes coverage reports generate busywork. coco
separates them by intersecting the compiler's own cross-reference index with
the run's probe data. On top of that it reports per-test attribution, a CRAP
risk ranking, and mutation results.

Everything below is demonstrated by a project you can run:
[cajeta-coco's `samples/tour`](https://github.com/jklappenbach/cajeta-coco/blob/main/samples/tour/README.md), which contains one
class per finding.

---

## Toolchain floor: cajeta 0.22.2

The plugin ships as a `.cja` and is **AOT-compiled by the toolchain that runs
it** (see "What the plugin is" below). That makes the compiler part of coco's
runtime, not just its build: cajeta **0.21.0 miscompiles coco's file reads** —
an intrinsic `#`-return losing its title across a return boundary — so every
verb throws before doing any work:

```
cajeta: uncaught exception (value=0x3)
  at cajeta.coco.plugin.Pipeline.moduleFiles(...)
```

Publishing a fixed plugin cannot help; the bytes are already correct and the
compile of them is not.

**0.22.2** raises the floor for a second reason. coco lowers IR and reads
bitcode with `cajeta lower` / `cajeta disasm` rather than a separately
installed `llc` / `llvm-dis`. Before that it needed an LLVM matching the
compiler *exactly*, which installing a cajeta package does not give you: the
package ships LLVM linked into the compiler, not the LLVM command-line tools,
so `llc` resolved to whatever the distro packaged and could not parse the IR
cajeta emits. That failure surfaced as `error: unterminated attribute group`
from LLVM's parser, naming nothing that would lead you here.

`cajeta --version` first.

## Quick start

```jsonc
// cajeta.json
"settings": {
    "plugins-allowed-capabilities": ["process", "filesystem"]
},

"plugins": {
    "dev.cajeta.coverage": {
        "version": "0.4.*",
        "config": {
            "src":   "src/main/cajeta",
            "entry": "com.example.Tests.main",
            "out":   "build/coco",
            "min":   80
        }
    }
},

"tasks": {
    "cover": {
        "description": "Instrument, run the suite, and report",
        "actions": [
            { "action": "cajeta.coverage.instrument", "id": "ci" },
            { "action": "cajeta.coverage.report",     "id": "cov" }
        ],
        "outputs": { "percent": "${cov.percent}" }
    }
}
```

```bash
$ cajeta cover
```

Two things about that task are not arbitrary:

- **Do not name it `coverage`.** `cajeta coverage` is a built-in subcommand
  (it manages the exclude list), and a task by that name needs
  `cajeta -- coverage` to reach. `cover`, `check`, `verify` all work plainly.
- **`plugins-allowed-capabilities` is required.** The plugin declares
  `process` (it drives the compiler) and `filesystem` (it writes reports). A
  plugin cannot widen its own reach: the consuming project allowlists what
  plugins may hold, and a plugin declaring anything outside it is refused at
  load time. Without the allowlist you get:

  ```
  cajeta build: plugins.dev.cajeta.coverage: declares capability 'process'
  which is not in the consumer's plugins-allowed-capabilities allowlist
  ```

---

## What the plugin is, and what it does

`dev.cajeta.coverage` is a thin adapter; the engine is `dev.cajeta.coco`,
which it depends on. The build tool AOT-compiles the plugin from its `.cja`
once and caches the binary under `~/.olla/dev.cajeta.coverage/<version>/bin/`,
keyed on the artifact hash — so the first `cover` run after a plugin upgrade
pays a compile, and later ones do not.

It ships **three** actions:

| Action | What it does |
|---|---|
| `cajeta.coverage.instrument` | The whole measured pipeline: compile, instrument, link, run the suite, merge per-test profiles. Fails the task if the suite fails. |
| `cajeta.coverage.report` | Render HTML, lcov, SARIF and the CRAP ranking from the run's artifacts, then apply the `min` gate. |
| `cajeta.coverage.mutate` | Mutation-test a completed run: swap one `icmp` predicate at a time, relink, re-run, and apply the `min-score` gate. |

The split matters: **instrument once, report many times.** coco persists the
probe map beside the instrumented IR, so re-rendering — a different view, a
different threshold, the IDE re-reading the run — never rebuilds anything.
`cajeta.coverage.report` on its own is always cheap.

### Why the first run is slow

A Cajeta front-end invocation costs roughly 80 seconds regardless of project
size, and `instrument` runs two of them:

- `--emit=exe`, to harvest a linkable stdlib object that already carries the
  synthesized C `main`, plus `__cajeta_tls.o`;
- `--emit=ir --emit-xref=…`, to get the per-class `.ll` coco instruments and
  the cross-reference index reachability needs.

Everything after that is coco's own: instrumenting a module takes about
0.1 s, `llc` about 0.03 s, the link about 0.2 s. That is what makes mutation
testing viable at all — a mutant is one module re-lowered and one link, well
under a second, rather than a fresh 80-second compile.

---

## Configuration reference

Everything in `config` is the **default parameter layer** for both actions; a
task may override any of it per-action.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `src` | string | — (required) | Source root to measure, relative to the project. |
| `entry` | string | — (required) | The method whose return dumps the profile, **dotted**: `pkg.Class.method`. Note this is *not* the `::` form used by `settings.build.entry-method`. |
| `out` | string | `build/coco` | Where every artifact lands, relative to the project. |
| `exclude` | string[] | `[]` | Path fragments left **uninstrumented**. Substring match against the source path. |
| `classpath` | string | derived | Dependency archives, comma-separated. Defaults to the project's resolved dependencies; set it only to override. |
| `profile` | string | `""` | `@Profile` for both front-end passes. |
| `min` | int | `0` (off) | Line-percentage gate on `report`. Below it, the action fails the task. |
| `warn` | int | `0` (off) | Line-percentage level ABOVE `min`. Below it, `report` records a warning finding and the task still SUCCEEDS. Must not be below `min`. |
| `min-score` | int | `0` (off) | Mutation-score gate on `mutate`. Same semantics as `min`. |

### `${...}` in these values

Substitution reaches `settings` and `plugins` when the manifest loads, and a
task's action params when the action runs — so a version written once under
`properties` reaches both a dependency and a plugin config value:

```jsonc
"properties": { "unit-version": "0.2.0" },
"settings":   { "dependencies": { "dev.cajeta.unit": "${unit-version}" } }
```

It deliberately does **not** rewrite `tasks` at load time. Action params carry
late-bound references a property table cannot answer — `${ci.path}` is another
action's output, `${params.profile}` an invocation argument — and those resolve
against the task context, which layers them over these same properties.

### `entry` — what the profile is a profile *of*

coco wires the entry method so the probe table is sized on the way in and the
profile is dumped on the way out. It must therefore **return normally**; a
suite that calls `exit()` writes no profile, and coco says so rather than
reporting zero coverage.

With `dev.cajeta.unit`, the entry is usually a one-liner over reflective
discovery:

```cajeta
public static int32 main(String[] args) { return Runner.runAll(); }
```

### `exclude` — measurement, not the link

An excluded path is **not instrumented**. It is still compiled and still
linked: excluding a package must never change which program runs. Excluded code
leaves both the numerator and the denominator, so opting out does not round the
percentage up.

Two forms. The **typed** one is what `cajeta coverage ignore` writes and what
you should use:

```jsonc
"exclude": [
    { "kind": "package", "pattern": "com.example.tests.*",
      "reason": "the suite itself — its lines are executed by definition" },
    { "kind": "file",    "pattern": "**Generated.cajeta",
      "reason": "machine-generated; covered through integration tests" },
    { "kind": "symbol",  "pattern": "com.example.Foo.getCount",
      "reason": "trivial accessor, exercised by every caller" }
]
```

| Kind | Pattern | Matched against | Granularity |
|---|---|---|---|
| `file` | glob — `*`, `**`, `?` | source path | whole module |
| `package` | `a.b.c` (that package) or `a.b.c.*` (and subpackages) | source path, dots → slashes | whole module |
| `symbol` | glob over `owner.method` | each function | **one function** |

`symbol` is per-function because a module holds many symbols; the others skip
the module's probes entirely. Either way the module is still compiled and
linked.

`reason` is mandatory and is checked against the same generic list the CLI
refuses to write (`wip`, `todo`, `skip`, `fixme`, `tbd`, empty). It shows up on
every diff that touches the list, so it has to justify itself.

The **bare-string** form still works and means `kind=file` with no reason — but
it is matched as a **substring**, not a glob, and warns once per run. That
difference is deliberate: a glob is anchored, so re-reading `"src/test/"` as one
would match nothing and silently un-exclude every project still using the old
spelling.

Two exclusions are worth making in nearly every project — your tests, and the
test framework:

```jsonc
{ "kind": "package", "pattern": "com.example.tests.*", "reason": "..." },
{ "kind": "package", "pattern": "dev.cajeta.unit.*",  "reason": "somebody else's tested code" }
```

Excluding your tests matters for a reason beyond the percentage: an
instrumented test body owns lines nothing else touches, so *every* test looks
like it contributes unique coverage and the per-test redundancy signal is
destroyed.

### `classpath` — derived, and overridable

Leave it unset. The build tool sends the project's resolved dependency
archives to the plugin as `context.classpath` — the same comma-joined string
it gives the compiler as `--classpath` — and the plugin uses it whenever the
task names none. A project that declares `dev.cajeta.unit` gets it on the
instrumenting compile automatically.

Set `classpath` explicitly only to override that: an archive outside the
dependency graph, or a pinned build you want measured instead of the resolved
one. An explicit value always wins.

> Before the protocol carried it, a plugin that compiled the consumer's
> sources could see the toolchain and its own artifacts but not the
> consumer's dependencies, so instrumenting a project that used a test
> framework died on `CAJETA_ERROR_UNRESOLVED_TYPE: unknown type 'Runner'` and
> the workaround was to hand-write `~/.olla/…` paths into the task and keep
> them in step with the manifest by hand.

### `min` and `warn` — the gate, and the level below it

`cajeta.coverage.report` compares the integer part of the line percentage
against `min` and fails the task when it falls short — JaCoCo `check`
semantics, so 79.9% fails a `min` of 80:

```
cajeta cover: task 'cover' actions[1] (cajeta.coverage.report):
coverage 12.0% is below min 55%
```

Leave it at `0` while you are finding your number, then set it to what you
have and ratchet.

**`warn` is the level that makes the ratchet work.** A gate that only ever
fails gets raised once and then avoided: the day it fires, the fix is
expensive and the pressure is to lower the number. Set `warn` above `min` and
you see the drift while it is still cheap:

```jsonc
"config": {
    "min":  75,   // below this the build FAILS
    "warn": 80    // below this it is REPORTED and the build passes
}
```

| Coverage | Result |
|---|---|
| below `min` | `error` finding — **task fails** |
| between `min` and `warn` | `warning` finding — **task succeeds**, drift recorded |
| at or above `warn` | no finding |

Both land as findings rather than as a bespoke pass/fail, so the number
reaches the diagnostic stream where a CI consumer reads it directly instead of
inferring it from an exit code. Under `--diag-format=json` they are ordinary
diagnostics attributed to `dev.cajeta.coverage`; in text mode they print in the
compiler's own form, so an IDE makes them clickable:

```
dev.cajeta.coverage: error: coverage 12.0% is below min 55% [coverage-min]
```

Setting `warn` BELOW `min` is rejected rather than accepted, because such a
warning could never fire without the error firing too — the level you set to
see drift early would never be reached.

Only `min` set is the configuration every project has today, and it behaves
exactly as it always did. Only `warn` set reports drift and never fails, which
is a legitimate place to start when a project is not ready to gate.

### Managing the exclude list from the CLI

`cajeta coverage` (the built-in subcommand) edits the exclude configuration in
place, preserving JSONC comments and indentation:

```bash
$ cajeta coverage list
$ cajeta coverage ignore --kind=file --pattern='**/*_generated.cajeta' \
                         --reason='machine-generated; covered via integration'
$ cajeta coverage remove --pattern='**/*_generated.cajeta'
```

---

## Where the results are

Everything lands under `out` (default `build/coco`):

| Path | Format | What it is |
|---|---|---|
| `coverage.html` | HTML | The report: per-file percentages, the dead-vs-untested split, the CRAP ranking. |
| `annotated.html` | HTML | Your source, line by line, red and green. |
| `lcov.info` | lcov tracefile | The interop export — Codecov, `genhtml`, diff-coverage bots. |
| `coverage.sarif` | SARIF 2.1.0 | For code-scanning surfaces that ingest SARIF. |
| `sites.tsv` | `coco-sites v1` | The probe table: id → file, line, owner, method, kind. |
| `crap.tsv` | `coco-crap v1` | The risk ranking, worst first, with its inputs. |
| `attribution.tsv` | TSV | Which test covered which line. Present only when a recognised runner was on the classpath. |
| `mutation.tsv` | `coco-mutation v1` | Mutation verdicts. Written by `cajeta.coverage.mutate`. |
| `link.tsv` | `coco-link v1` | The link line `instrument` used, in order. `mutate` replays it with one object swapped. |
| `xref.json` | xref | The compiler's cross-reference index — what reachability is computed from. |
| `run/coco.profile` | `coco-profile v1` | Hit counts for the whole run. |
| `run/coco.merged.profile` | `coco-profile v1` | The attribution-merged profile. **This is the one to read** when it exists. |
| `run/coco-test-NNNN.profile` | `coco-profile v1` | One test's probe delta. Never read one of these as the run — it would report a single test's coverage as the whole suite's. |

`sites.tsv` and the profile are a **pair**: probe ids in a profile are
positional against the site table, so a profile read against a different run's
table silently attributes hits to the wrong lines. Tools that consume these
refuse to guess when the pair is broken, and so should yours.

The two formats carrying a version marker (`coco-sites v1`, `coco-profile v1`)
are a published interface with a conformance fixture, documented in
cajeta-coco's `docs/formats.md`. An unrecognised version is refused, never
parsed optimistically.

---

## Mutation testing

Coverage says a line ran. It cannot say anything ***asserted*** on it. A test
that calls a function and checks nothing gives a green gutter and no
verification whatsoever.

coco answers that by mutating the program and re-running the suite. Its
operator set is `icmp` predicate swaps — `sgt↔sge`, `slt↔sle`, `ugt↔uge`,
`ult↔ule` (boundary) and `eq↔ne` (negation) — chosen because they touch one
token, can never invalidate the module, and target exactly the off-by-one
that boundary-blind tests fail to look at.

Three verdicts, and the third is why the score is honest:

| Verdict | Meaning | The fix |
|---|---|---|
| `killed` | The suite noticed. | None; this is the suite working. |
| `SURVIVED` | The behavior changed and every test still passed. | An **assertion**. |
| `skipped-uncovered` | The mutated compare never executed. | A **test** — and it is excluded from the score rather than counted against the suite, because an unexecuted mutant cannot be killed. |

Mutation runs over a completed run, from cajeta-coco's driver:

```bash
$ cajeta mutate
coco: baseline exit 0
coco: mutation score: 2/3 killed (9 skipped as uncovered)
coco: SURVIVING mutants — these behavior changes went unnoticed by the suite:
  tour/coco/Shipping.ll:25  sge->sgt in tour.coco.Shipping::rateCents(subtotalCents:int64)

Task 'mutate' outputs:
  killed = 2   survived = 1   skipped = 9   score = 66.6
```

Wire it as its own task — it runs **over** a completed coverage run, never
instead of one, because it needs that run's site table, profile and recorded
link line:

```jsonc
"mutate": {
    "actions": [ { "action": "cajeta.coverage.mutate", "id": "mut" } ],
    "outputs": { "score": "${mut.score}", "results": "${mut.mutation-path}" }
}
```

`min-score` gates it the way `min` gates coverage. Results land in
`mutation.tsv`, headed `coco-mutation v1`.

`mutate` mutates only what was **instrumented**, derived from `sites.tsv`
rather than from a restated `exclude`: an excluded module contributed no probe
rows, so the site table is the instrumented set. That matters because mutating
a test's own assertion produces a survivor that means nothing — a suite cannot
kill a change to itself.

A mutant is one `llc` plus one relink plus one suite run. The relink replays
the link line `instrument` recorded in `link.tsv`, with exactly one object
swapped, so a mutant is the measured program and not a near-miss of it.

> **Fixed, and worth knowing about.** With per-test attribution enabled, a
> build whose suite FAILED used to die with SIGSEGV instead of reporting the
> failure — measured 3/3, and 3/3 clean with the attribution hook removed from
> the same objects. The fault was in `dev.cajeta.unit`'s `Runner.runOne`, which
> kept `msg = e.message` — a borrow of the caught exception's message — past
> the catch scope that owns it; the hook's allocation and file write turned
> that latent use-after-free into a fault. **cajeta-unit 0.2.3** copies the
> message; pin at or above it when you enable attribution. A green suite never
> executes that path, which is why it first appeared as "mutation kills produce
> SIGSEGV". After the fix a killed mutant dies with a `✗ FAIL` line and exit 1,
> so the verdict and the signal finally agree.

---

## In the IDE

The IntelliJ plugin renders coverage two ways, and the split is deliberate.

**IntelliJ's own coverage subsystem** owns the commodity layer: gutters, line
highlighting, the Coverage tool window, per-directory rollups. Nothing is
reimplemented — coco is registered as a coverage engine, so it looks and
behaves like every other coverage tool in the IDE.

- **Run with Coverage** on a Cajeta task configuration runs the task's
  instrument action and loads the result. The plugin reads `cajeta.json` to
  find which task instruments and where the artifacts land; a project with no
  coverage configured is told what to add rather than failing obscurely.
- **Load Cajeta Coverage**, in the platform's Coverage menu, loads a run that
  already happened. The manifest already says where the artifacts are, so
  there is nothing to browse for.
- Re-loading never re-runs. Both entry points share one loader.

**The coco tool window** — registered as **Cajeta Coverage**, on the right —
carries what IntelliJ's coverage model (lines, hit counts, branches)
structurally cannot express. One window, four tabs:

| Tab | Shows | Reads |
|---|---|---|
| **Dead Code** | Every uncovered method, classified `DELETION CANDIDATE`, `NEEDS A TEST` or `UNDETERMINED`, each with the reason stated. Jump to source, and Safe Delete on a deletion candidate. | `sites.tsv` + profile + `xref.json` |
| **Tests** | Per-test attribution: what each test covered, and how much of it it uniquely covered. A test with zero unique lines is called out as a redundancy candidate. | `attribution.tsv` |
| **Risk** | The CRAP ranking, worst first, each entry annotated `complexity N, M% line coverage → CRAP S`. | `crap.tsv` |
| **Mutants** | Surviving mutants only, with location and the mutation applied. A killed mutant is the suite working; an uncovered one is already reported by three other views. | `mutation.tsv` |

A tab whose data was not collected says so — `not available`, `not
collected` — rather than showing an empty list. An empty list means "nothing
found", which is a different claim.

### Classification, and why it refuses to guess

The Dead Code tab's verdict is a conjunction, and both halves matter:

- Reachability is computed from the entry point **plus every method the
  profile shows executed**. Seeding from coverage is what keeps
  reflection- and DI-invoked code out of the deletion list: a static graph
  cannot see those call sites, but the profile can see that the method ran.
- A method on a class with *any* executed method is never called dead. Its
  class demonstrably exists, and compile-time DI and reflection reach members
  through synthesized paths no static graph records.

When reachability cannot be determined at all — a missing or stale index —
everything is `UNDETERMINED` with that reason, rather than every method being
reported as dead.

### Staleness

Results are never silently shown against changed source. Freshness is tracked
**per file**, not per project: the common case is one file edited out of many,
and invalidating the whole run for that would throw away results that are
still perfectly good. Editing a measured file marks that file's results stale
and says so; the rest stay live.

---

## Interop

`lcov.info` is the export for everything outside the IDE — Codecov,
Coveralls, `genhtml`, diff-coverage bots in CI. It is a *derived* format and
expresses none of coco's differentiators: no dead-vs-untested, no attribution,
no risk score, no mutation outcome. That is exactly why the IDE reads coco's
native formats instead. Use lcov to talk to other tools, not to read coco's
own results.

---

## See also

- [cajeta-coco's `samples/tour`](https://github.com/jklappenbach/cajeta-coco/blob/main/samples/tour/README.md) — one class per
  finding, and what each looks like in the IDE.
- [BuildTool.md § Code coverage](../specification/buildtool/BuildTool.md#code-coverage-the-devcajetacoverage-plugin)
  — the plugin in the context of the whole action catalog.
- [05 — Debugging](05-debugging.md) — the sibling tooling chapter.
