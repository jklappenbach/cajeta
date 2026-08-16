# Kernel time-to-first-result

What a notebook user waits for between pressing Shift-Enter on the first cell
and seeing a result. Measured through `jupyter_client` — the library Notebook
and Lab use — against the installed `cajeta` binary, so the numbers are the
product's and not a test harness's.

## Why this exists

The jupyter-kernel plan carried a first-cell figure of "~5s" taken from a
`CMAKE_BUILD_TYPE=Debug`, `-g` build of `cajeta_test`. It was wrong in both
directions, and the direction that mattered was hidden: `cajeta_test` primes
the stdlib **once per process** and every test reuses it, so the suite reports
per-test costs that no user ever sees. A real kernel primes once per **launch**.

## Reproduce

```sh
python3 measure.py --binary /usr/lib/cajeta/cajeta --repeat 2
```

`--scenario <name>` restricts the run; `--deps-project <dir>` picks the
dependency-bearing project. Requires `jupyter_client`.

The script asserts `implementation == "cajeta"` before timing anything. Its
first version did not, and measured IPython: `jupyter_client` 8 ignores
`KernelManager.kernel_cmd` when the manager has no kernelspec and silently
launches the default kernel.

## Measured — 2026-08-15, cajeta 0.19.1 (64c81479), Release, repeat=2 medians

| scenario | startup | cell 1 | cell 2 | vs no-project |
|---|---|---|---|---|
| no-project | 0.24s | **54.19s** | 0.03s | — |
| project-no-deps | 0.23s | **53.45s** | 0.04s | −0.75s |
| project-with-deps | 0.23s | **255.66s** | 0.08s | **+201.46s** |

`project-with-deps` is `cajeta-timeseries`, one dependency (`dev.cajeta.ml
0.10.0`), resolving offline from the sibling checkout in 6.6ms — so the 201s
is compilation, not resolution or download.

Startup is the kernel answering `kernel_info`. It is fast and says nothing:
`KernelProtocol` builds its session lazily on the first `execute_request`, so
the whole cost of standing up a JIT session lands inside cell 1 while the
frontend shows a running cell and nothing else.

### Reading it

**54s is the floor, and it is not the kernel's.** A three-line hello-world
through `cajeta jit-run` takes 45s, of which 34s elapses before the user's file
is parsed:

```
 0.00s              [jit] collect
34.43s  (+34.43s)   [jit] parse 1/1 demo/Hello.cajeta
34.43s  (+ 0.00s)   [jit] codegen
41.76s  (+ 7.33s)   [jit] finalize
47.00s  (+ 5.23s)   entry returned 42
```

That 34s is the stdlib prime — parsing and prototyping 444 embedded sources —
and every `cajeta` invocation pays it: `build`, `test`, `jit-run`, `kernel`,
every lint pass. See `specs/compile-cache-spec.md`, whose Unit 1 measured the
split and concluded the front end dominates, so IR/object caching alone cannot
remove it. That plan is parked.

**project-no-deps matching no-project is the control, and it passed.** An empty
classpath keeps the resident stdlib path (`KernelSession` takes the reuse core
only when there are no archives), so these two must agree; −0.75s is noise. A
gap here would have been a defect rather than a cost.

**A dependency costs 201s on top.** A classpath session cannot use the resident
stdlib — the baseline is captured before any archive exists — so it rebuilds
the stdlib from scratch *and* compiles the dependency's sources. One
mid-sized library takes the wait from under a minute to over four.

**Cell 2 is 0.03–0.08s in every scenario.** Nothing about the steady state is
slow; the entire problem is standing the session up.

## Not a gate

No thresholds, no pass/fail. Numbers get committed here and reviewed.
