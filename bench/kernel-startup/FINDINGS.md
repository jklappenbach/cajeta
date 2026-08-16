# Where cold-start time goes

Measured 2026-08-15 on cajeta 0.19.1 (`64c81479`), **Release**, via
`bench/kernel-startup/measure.py` and `CAJETA_PRIME_TIMING=1`. Every number
here is from the shipped binary, not the test harness — the distinction is
load-bearing and is what made this invisible for months (see *How this hid*).

## The headline

| scenario | cell 1, today | cell 1, with SLL parsing |
|---|---|---|
| no project | 54.2 s | **19.2 s** |
| project, no dependencies | 53.5 s | **19.3 s** |
| project, one dependency | 255.7 s | **147.1 s** |

Cell 2 is 0.02–0.08 s in every case. Nothing about the steady state is slow;
the entire problem is standing the session up. Startup-to-`kernel_info` is
0.24 s and says nothing — the session is built lazily on the first
`execute_request`, so the whole cost lands inside cell 1 while the frontend
shows a running cell and nothing else.

## The full breakdown

A three-line hello-world through `cajeta jit-run`, default (full-LL) parsing:

```
[prime] prescan loop:            14330 ms over 349 files
[prime] parse loop:              14761 ms over 349 files
[prime] parse (444 sources)      36359 ms
[prime] buildPendingPrototypes     611 ms
[prime] Class<?> instantiation       0 ms
[prime] drainLazyStdlib              0 ms
                          jit-run total 42.7 s
```

And with one dependency (`cajeta-timeseries` → `dev.cajeta.ml 0.10.0`),
adding the classpath ingest:

```
[ingest] prescan dep sources     14965 ms
[ingest] drain lazy stdlib      114377 ms
[ingest] parse dep sources       18738 ms
[ingest] buildPendingPrototypes      4 ms
                                       cell 1 = 231 s
```

### 1. The eager prime is ANTLR prediction, and nothing else

```
[prescan] 350 files: lex 85 ms, parse 14329 ms, visit 43 ms
```

99.1% of the prescan pass is `parser.compilationUnit()`. Lexing is 0.6%; our
own visitor is 0.3%. No `PredictionMode` is set anywhere in the tree, so ANTLR
runs its default full-LL adaptive prediction.

Forcing `PredictionMode::SLL` (`CAJETA_PRESCAN_SLL=1 CAJETA_PARSE_SLL=1` —
a **measurement spike, not a correct implementation**, see below):

| | full LL | SLL | |
|---|---|---|---|
| prescan loop | 14330 ms | 1503 ms | 9.5× |
| parse loop | 14761 ms | 2573 ms | 5.7× |
| **total prime** | **36359 ms** | **4920 ms** | **7.4×** |
| hello-world, end to end | 42.7 s | 17.1 s | 2.5× |

Every eager file is walked by ANTLR **twice** — a prescan pass that registers
class names, then the real parse. Under LL that duplication costs 14 s; under
SLL it costs 1.5 s, which reorders the plan: generating the prescan table at
build time looked like a 17.6 s win before this measurement and is worth ~1.5 s
after it.

### 2. The dependency cost is generic instantiation, not parsing

The lazy-stdlib drain barely moves under SLL — 114 s → 92 s — because it is
not parsing text:

```
[drain] 8 packages / 80 files: prescan 0 ms, parse 42858 ms,
                               buildPendingPrototypes 48270 ms
```

* **536 ms per file** against the eager loop's 7.4 ms per file under SLL — 72×.
* **48 s of `buildPendingPrototypes` for 8 packages**, against 0.6 s for all
  349 eager files.

These are the generic-heavy packages (`cajeta.math` and friends), pulled in
because the dependency's signatures name them — cajeta-ml's `GradTape` has
fields typed `ArrayList<Tensor<E>>`, so reading that signature instantiates
`cajeta.math`. The cost is template instantiation and class layout, and it
dwarfs everything else in a dependency project.

It also means the eager/lazy stdlib split stops paying the moment a real
dependency is present. It does not avoid the work; it moves *when* the work is
paid, and the lazy route appears to be the far more expensive way to arrive
(92 s for 8 packages / 80 files, against 4.9 s for 349 eager files).

### 3. What is left after parsing

Under SLL, `project-with-deps` cell 1 = 151 s:

| | |
|---|---|
| eager stdlib prime | 4.8 s |
| prescan dep sources | 1.4 s |
| **lazy drain (instantiation)** | **92.2 s** |
| parse dep sources | 4.7 s |
| codegen + JIT (144 dep modules + the cell) | ~48 s |

and `no-project` cell 1 = 19.2 s: 4.8 s prime, ~14 s codegen + JIT.

### 4. Dependencies already ship compiled, and it is discarded

`dev.cajeta.ml-0.10.0.cja` holds 144 `class_bitcode` entries beside 144
`class_source` entries. `ingestClasspath` reads the **source** ones and
re-parses + re-codegens all 144. This is worth part of the ~48 s codegen line
above — real, but not the lever it looked like before the split was measured.

## How this hid

The test harness primes the stdlib **once per process** and amortizes it over
~4000 tests, so the suite reports ~5 s per test. A real `cajeta` invocation
primes once per **launch**. The phase instrumentation that would have shown
this lived in `test/jit/JitTestHelper.cpp` — inside the harness that hides it.
It is now in `Compiler.cpp`, where the cost actually is.

The earlier `compile-cache` measurement (2026-06-25) recorded
`front_end 12.6 s / ir_gen 6.3 s / total ≈19 s` and concluded IR caching could
not remove the front end. That conclusion holds and is now sharper: IR
generation has left the prime entirely (0.6 s of 37 s), so the front end is not
merely dominant, it is the whole thing.

## Validation of (1), two-stage parsing — 2026-08-15

Implemented behind `CAJETA_TWO_STAGE_PARSE=1` (`6edaef5f`, `941d8ec8`).

* **The grammar is SLL-clean on the stdlib**: `[two-stage] 700 parses: 700
  SLL, 0 fell back to LL`. Prime 29.7 s → 4.2 s (Release), 4.5 s in the Debug
  test binary.
* **Routine gate under the flag: 1435 passed, 0 failed**, 4 skipped, 1 timed
  out. The timeout is `TableCoreTests.frameSchemaErrorDoesNotPoisonNextCompile`
  and it is not a regression — re-measured alone it takes **225 s without**
  two-stage and **136 s with**, so it exceeds the 120 s shard budget either way
  and two-stage makes it 40% faster.
* Full battery (`FULL=1`) under the flag is the gate on flipping the default.

Two things learned while validating, both worth keeping:

The aggregate `[two-stage]` counter is a static destructor, and `cajeta_test`
exits without running static destructors — so a battery run reported nothing at
all from it. Each fallback now prints as it happens, naming its input.

And an expected-syntax-error test **falls back by design**: bail throws on the
first error, so malformed input always reaches stage 2, which is how it gets
its diagnostics. Fallbacks in a battery log are therefore expected and correct.
The verdict is pass/fail parity; a fallback on VALID input is the signal.

## The path

Ordered by measured value, not by appeal:

1. **Two-stage SLL parsing.** 7.4× on the prime, 2.5× end to end, toolchain
   wide — every `build`, `test`, `run`, `kernel` and lint pass. Must be
   **two-stage**: SLL with `BailErrorStrategy`, re-parsing under LL on failure.
   Plain SLL can mis-parse an ambiguity instead of erroring, which is why the
   spike above is gated behind an env var and explicitly marked incorrect.
   Needs a full battery run under it to find where the grammar is not
   SLL-clean.
2. **The lazy-drain instantiation cost.** 92 s for 8 packages, and the largest
   remaining line in any dependency project. 536 ms/file and 48 s of prototype
   building want a profile before a fix is chosen; the eager/lazy split itself
   is a candidate for removal rather than tuning.
3. **Codegen + JIT.** ~14 s for a trivial cell becomes the floor once (1)
   lands, and has not been broken down yet.
4. **Front-end state cache or fork.** The remaining ~5 s prime. This is
   `compile-cache`'s parked territory and the only thing that reaches a
   sub-second start.
5. **Archive bitcode for dependency definitions.** Part of the ~48 s codegen
   line in a dependency project.

Against a target of 1–2 s for an empty project: (1) alone lands at ~19 s, so
(1) + (3) + (4) are all required. No single change gets there.

## Dependency ingest, re-measured 2026-08-16

`cajeta build` on a one-dependency project (`dev.cajeta.ml 0.10.0`), Release,
two cold runs at load ~4 (another clone's battery held ~3 of 32 cores):

| phase | run 1 | run 2 | share of ingest |
|---|---|---|---|
| prescan dep sources | 1.5 s | 1.4 s | 1.5% |
| **drain lazy stdlib** | **89.0 s** | **91.5 s** | **93-94%** |
| parse dep sources | 4.2 s | 4.9 s | 4.5% |
| buildPendingPrototypes | 0.004 s | 0.004 s | — |
| ingest total | 94.7 s | 97.8 s | |
| build total | 122.2 s | 126.4 s | |

**No regression since 2026-08-15.** The drain's parse line was 42,858 ms then and
42,785 / 43,204 ms now — within 1%. An earlier run of this benchmark showed the
drain at 158 s and was reported as a 44 s regression; that run was taken at load
27 and is void. Check `uptime` before believing a benchmark here.

**What this means for 7.2.10 — it was dropped.** Reading `class_bitcode` would
address the dependency's own codegen, but that codegen is gated:
`linkClasspathDeps = (emitMode == Obj || Exe)`. `emitMode` defaults to `IR` and
neither KernelSession nor the JIT host sets it, so the notebook path never
re-codegens dependency bodies. A library build does not either — measured
`emitMode=2 linkClasspathDeps=0`, 1,164 dep methods present, none re-derived.
Only `--emit=obj/exe` takes that branch, so on first-cell latency the fix returns
zero.

The 89-91 s drain is stdlib packages instantiated because the dependency's
SIGNATURES name them — `cajeta.math` via `GradTape`'s `ArrayList<Tensor<E>>`.
Those are stdlib classes, so no dependency archive can supply them. That cost
belongs to 7.2.9.

Separately: `EntryKind::ClassBitcode` is written and read by nothing. Every
`.cja` ships one compiled class per source that no consumer opens.

## Template instantiation is ANTLR re-walking (2026-08-16)

Instrumented `instantiateInternal`, quiet box, Release:

| | eager (all 444) | drain (dependency) |
|---|---|---|
| `visitClassBody` (tree walk) | 52,373 ms | 43,924 ms |
| `generatePrototype` (LLVM lowering) | 104 ms | 101 ms |
| instantiations (cache misses) | 160 | 160 |

**Instantiation is ~100% tree re-walking.** LLVM lowering is noise. Every
instantiation re-walks the template's whole parse tree through `visitClassBody`,
so cost tracks source size: `cajeta.math.Tensor` is 5,004 lines / 225 methods and
is re-walked per distinct type argument.

**There is no lazy-vs-eager penalty.** Total walk is comparable (52 s eager,
44 s drain) — the eager route walks slightly MORE. What differs is where it
lands: eager instantiates inline during parsing (19 s inside the deferred
drain), the drain defers nearly everything (42 s inside it). Per-call "worst
instantiation" figures (9.3 s drained vs 2.3 s eager) measure that
concentration, not a cost difference.

### Hypotheses killed by measurement — do not re-run these

1. *The `buildPendingPrototypes` fixpoint re-scans the whole canonicalMap.* It
   does, but that scan is **2 ms of 43,956 ms**. Iterating a pending set saves
   nothing.
2. *The drain leaves `instantiationReuseTarget` set, so nested instantiations
   bypass the cache.* It is consumed immediately; the code says why.
3. *The drain bypasses cache entries that existed.* Counted: `bypass` equals the
   number of drained instantiations exactly (one each, by design), and misses are
   identical on both routes (160 = 160).

### What this means for 7.2.9

The item is scoped as caching + scheduling (artifact cache, front-end state
cache, shrink the eager set, prescan table at build time). None of those touch
~45 s of repeated tree walking. The lever is to stop re-walking a template body
per instantiation — memoize the walked body per template, or lower once and
substitute. That is route-independent and is the dominant cost in both
measurements.

Note the prescan-table lever the item calls "the cheap half, ~17.6 s" is now
worth ~4 s: two-stage parsing already took the prescan loop from 17,605 ms to
3,942 ms.

## Full phase accounting, one dependency (2026-08-16)

`CAJETA_PRIME_TIMING=1`, release binary, `project-with-deps`
(`cajeta-timeseries`), cell 1 = 144.3 s:

    [prime]  stdlib total                       5,838 ms    4%
    [ingest] prescan dep sources                1,365 ms
    [ingest] drain lazy stdlib                 86,308 ms   60%
               parse            80 files/8 pkgs 39,284 ms
               buildPendingPrototypes           46,072 ms
               ([defer] walk=49,796 proto=110 insts=160)
    [ingest] parse dep sources                  4,210 ms
    [ingest] buildPendingPrototypes                 4 ms
    unaccounted (IR emit + JIT + session)     ~46,600 ms   32%

**One dependency drags in 8 lazy stdlib packages.** `cajeta-timeseries`
references `Tensor`, so all of `cajeta.math` is parsed and fully instantiated,
whether or not the cell touches it. That drain is 60% of first-cell latency.

The stdlib prime is NOT the problem — 5.8 s of 144.3 s, and its own
`drainLazyStdlib` phase is 0 ms. An earlier read of this table that omitted the
`[ingest]` tag concluded the lazy-stdlib cost had evaporated; it had not, it is
in the ingest.

### Open: the drain parses 55x slower per file than the prime

    [prime] parse loop   3,108 ms / 349 files =   8.9 ms/file
    [drain] parse       39,284 ms /  80 files = 491.0 ms/file

`cajeta.math` files are large (`Tensor` is 5,004 lines) and the prime's own
worst file is 821 ms, so part of the gap is content. Unexplained on the average.
Check before redesigning anything: does the drain re-parse, parse once per
referencing package, or miss the two-stage SLL fast path the prime gets?

### The drain's parse is three files, and it is not parsing

Per-file, same run (`[drain]` top entries of 40,845 ms over 80 files):

    30,743 ms  cajeta/nucleo/column/DynCol.cajeta      735 lines
     5,791 ms  cajeta/math/Ewise.cajeta              1,157 lines
     2,910 ms  cajeta/nucleo/sparse/CsrMatrix.cajeta   344 lines
        36 ms  cajeta/math/Tensor.cajeta             5,004 lines

Top three are 97% of it. `DynCol` alone is 21% of the whole 144 s cell.

NOT the parser. Zero `[two-stage]` fallbacks — the drain is SLL-clean. And
`Tensor` is 6.8x the lines of `DynCol` at 1/850th the cost, so cost does not
track source size.

`parseSource` is parse + body walk, and a TEMPLATE defers its walk to
instantiation. `Tensor` is a template (156 generic decls); `DynCol` is a plain
`public class` whose 21 fields are each a distinct instantiation
(`Column<int8..float64>`, `NullableColumn<int8..float64>`, `StringColumn`).
Those instantiate during field resolution, inside `parseSource`.

Cost is superlinear in distinct instantiations per concrete class:

    DynCol      20 insts   30,743 ms
    Ewise       15 insts    5,791 ms
    CsrMatrix    1 inst      2,910 ms
    RebindSlot   0 insts       519 ms
    Tensor      35 insts        36 ms   (template — deferred)

1.3x the instantiations for 5.3x the cost between Ewise and DynCol. Two points
do not fit an exponent; it is not linear.

This is also NOT the 7.2.9 template body walk: process-wide `walk` (49,796 ms)
exceeds the drain's `buildPendingPrototypes` (46,072 ms) by 3.7 s, not by the
30 s `DynCol` would contribute if it were walking template bodies.

NEXT: synthesize a concrete class with N distinct `Column<T>` fields for
N = 5/10/15/20 and time `parseSource`. That fits the exponent and says whether
the fix is in the instantiation cache or in the field-resolution path itself.

### ROOT CAUSE: instantiation re-parses synthesized source in full LL

`perf` on a 6-line repro (`import cajeta.nucleo.column.DynCol;` and nothing
else, 55 s to build, 39 s of it drain):

    11.10%  ParserATNSimulator::closure_
     3.48%  SingletonPredictionContext::equals
     3.37%  PredictionModeClass::getConflictingAltSubsets
     ...    14 more ATN symbols            ~38% total
    ~19%    malloc/free (ATNConfig / PredictionContext churn)

~57% of the process is ANTLR ATN prediction. But `parseSource`'s own
`compilationUnit()` call is 4 ms for `DynCol` against a 25,436 ms walk:

    DynCol  25,440 ms = antlr     4 ms + walk 25,436 ms
    Ewise   13,526 ms = antlr     5 ms + walk 13,521 ms
    Tensor      33 ms = antlr    16 ms + walk     17 ms

The ATN work is inside the WALK because `TemplateInstantiator` instantiates by
synthesizing source text and re-parsing it — `ANTLRInputStream` + `CajetaLexer`
+ `parser.compilationUnit()` at three sites (611-616, 750-755, 1231-1236).

**All three call `compilationUnit()` directly and bypass
`parseCompilationUnitTwoStage`**, which is `static` in `Compiler.cpp` and not
reachable from that translation unit. Every instantiation re-parse therefore
runs in full LL. This is why two-stage parsing took the prime 36.4 s -> 4.9 s
and did nothing for the dependency case.

Explains what misled the investigation: zero `[two-stage]` fallbacks (these
parses never enter two-stage); `Tensor` parsing in 36 ms while costing 10 s to
instantiate; and a synthetic template measuring 1.2 ms/method against a real
45-73 ms (trivial bodies synthesize trivial text, so re-parse cost tracks body
complexity, not method count).

TWO FIXES, cheap first:
(a) route the three sites through the two-stage helper (needs it exposed).
    If SLL pays off as it did for the prime, this is most of the 39 s.
(b) do not re-parse at all — substitute into the already-parsed tree.

### After (a): two-stage on synthesized units (2026-08-16)

    scenario           2026-08-15    now      vs session start
    no-project            20.25s   14.09s     54.2s  -74%
    project-no-deps       19.97s   14.13s     53.5s  -74%
    project-with-deps    147.18s   50.31s    255.7s  -80%

    [ingest] drain lazy stdlib   86,308 ms ->  3,015 ms   28.6x
    [ingest] cumulative          91,888 ms ->  4,743 ms   19.4x

Full battery green and 3,043s -> 2,109s (-31%).

**(b) is not worth building.** The synth counter sizes it directly: across the
whole drain, synthetic parsing is now ~283 ms (DynCol 210 ms over 42 parses,
Ewise 42 ms over 22). Memoizing the tree could recover maybe 250 ms of a
3,015 ms drain, in exchange for sharing a mutable parse tree across
instantiations -- a miscompile-class risk against a sub-second win. Drop it.

### The unaccounted term is now the whole problem

    project-with-deps cell 1        50.31 s
      [ingest] total                 4.74 s
      [prime] stdlib                ~5.8 s
      unaccounted (IR emit/JIT)     ~40 s     ~80%

It was ~46.6 s before (a) and barely moved, which confirms it is neither parse
nor instantiation. Nothing has instrumented it yet. It is now the dominant cost
of a dependency notebook start and should be measured before any further work
on the front end.

### The ~40s is eager codegen of the whole world (2026-08-16)

`[cell]` probes in `KernelSession::execute`, `project-with-deps`, cell 1:

    [cell] codegen fixpoint: 3 iterations over 147 modules, 23394 methods
    [cell] codegen method bodies       27,439 ms   55% of cell 1
    [cell] verify + JIT materialize    16,026 ms   32%
    [cell] reflect thunks + ClassObject     30 ms
    [cell] legalize + demote               141 ms
    [cell] createModule / compile / resolve  0 ms
    [cell] TOTAL                       43,639 ms

With `[ingest]` 5,042 ms and `[prime]` ~5.8 s that accounts for the whole ~50 s.

**A cell computing `20 + 42` generates 23,394 LLVM function bodies** — every
method of the stdlib and of every dependency, over 147 modules, three passes.
Cell 2 runs the same fixpoint in 27 ms, so the cost is first-time generation,
not the scan.

The loop is deliberate: the comment says the stdlib is included because "a cell
that calls into the stdlib needs those bodies to exist or the cell fails to
materialize". But ORC materializes lazily -- the verify path's own comment says
"ORC materializes lazily, so that code is never compiled unless something calls
it". So the kernel eagerly emits IR for a world the JIT would have pulled in on
demand, then spends 16 s verifying and materializing it.

THE LEVER: emit method bodies on demand instead of eagerly. This is the same
"instantiate declarations, not definitions" idea as 7.2.9 but one layer down, at
IR emission, and it is worth ~27 s directly plus most of the 16 s verify (147
modules / 23k functions) against 7.2.9's few hundred ms. It should be reordered
ahead of 7.2.9.

### Why a cell codegens 12,379 method bodies

Per-module breakdown of the fixpoint set (`project-with-deps`, cell 1):

    11,015 methods  cajeta.runtime.__stdlib__     89% of the distinct set
       148 methods  dev.cajeta.ml.grad.GradTape
       139 methods  dev.cajeta.ml.grad.Ops
        70 methods  dev.cajeta.ml.grad.StructKernels
        ...          the whole dependency tree is ~1,300

**It is the stdlib, not dependencies.** The set is the entire world by
construction: `codegenMods()` is every compiler module plus the stdlib. The
stdlib's bodies are emitted lazily BY DESIGN ("emitted lazily, on demand"), and
the kernel defeats that by forcing all of them so a cell cannot fail to
materialize on `cajeta.lang.Object::drop`. Eager emission substitutes for
knowing which bodies a cell will reach. Three passes, because emitting a body
can instantiate a template and add methods.

The stdlib is also in the set TWICE -- `getModules()` already returns it and
`codegenMods()` appends it again (147 entries, 146 distinct). Deduping was
measured and REVERTED: it takes the count 23,394 -> 12,379 and the time
27,319 ms -> 28,014 ms, i.e. nothing, because `generateCode()` is idempotent.
The duplicate is a counting artifact, not a cost. Do not "fix" it for speed.

12,379 bodies in 27.3 s is ~2.2 ms each -- ordinary LLVM cost, paid on a world
the cell never touches. The lever is emitting on demand, not emitting faster.
