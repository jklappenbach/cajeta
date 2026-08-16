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
