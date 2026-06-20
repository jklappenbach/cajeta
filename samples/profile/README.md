# Cajeta profile — cross-language benchmark suite

A sibling of the [`tour`](../tour/README.md): where the tour shows a feature *works*,
`profile` shows how it *competes*. It runs Cajeta against Rust, C++, Java, Python, and Go
on community-recognized workloads and emits a fully reproducible record (versions, flags,
hardware, timing, memory) plus a Cajeta-themed report site.

> **Status:** scaffold (Unit 1). The harness + benchmark registration are in place; the
> benchmark catalog, CSV reporting, memory capture, competitor runners, and the report
> site land in later units. See the spec [`docs/specs/profile-spec.md`](../../docs/specs/profile-spec.md)
> and plan `agents/plans/profile-plan.md`.

This is a standard cajeta project: a `cajeta.json` manifest at the root and sources under
`src/main/cajeta/profile/`. It builds with the **cajeta build tool** — no hand-rolled
compile/link scripts.

```
samples/profile/
├── README.md             ← you are here
├── cajeta.json           ← build-tool manifest (build / run / release / clean)
├── build.sh / build.cmd  ← `cajeta build` → build/profile (native binary)
├── run.sh   / run.cmd    ← `cajeta run`   → build + execute
├── test/
│   └── smoke.sh          ← Unit 1 smoke test (build + lifecycle + --list)
└── src/main/cajeta/profile/
    ├── Profile.cajeta        ← entry point — registers + drives benchmarks
    ├── Benchmark.cajeta      ← base class (setup/run/teardown/checkResult + identity)
    └── ExampleBenchmark.cajeta ← scaffold benchmark proving registration + dispatch
```

## Build and run

The compiler binary is expected at `<repo>/build-cajeta/src/cajeta` (the current
fork-LLVM build — it bundles the TLS shim and is the toolchain the GPU/XPU benchmarks
need). Override with `CAJETA=/path/to/cajeta`. If you haven't built it yet:

```sh
cd <repo>
./setup.sh   # one-time
./build.sh   # incremental
```

Then from this directory, drive everything through the build tool:

```sh
./build.sh          # cajeta build  → build/profile
./build/profile     # run it
./build/profile --list   # list registered benchmarks

./run.sh            # cajeta run    → build + execute in one step
```

| Task | Script | Output |
|---|---|---|
| `build`   | `build.sh` / `build.cmd` | `build/profile` — ELF executable |
| `run`     | `run.sh` / `run.cmd`     | builds `build/profile`, then executes it |
| `release` | `cajeta release`         | optimized `build/profile` |
| `clean`   | `cajeta clean`           | removes `build/` |

## Smoke test

```sh
bash test/smoke.sh   # builds, runs, asserts the lifecycle order + --list output
```

## Adding a benchmark

1. Write a `Benchmark` subclass in the topic subpackage it belongs to (e.g.
   `src/main/cajeta/profile/sort/QuickSortBench.cajeta`, `package profile.sort;`),
   overriding `name()`, `area()`, `setup()`, `run(iterations)`, `teardown()`,
   `checkResult()`.
2. In `Profile.cajeta`, append one line to the registration list:
   ```cajeta
   benchmarks.add(heap QuickSortBench());
   ```

The runner picks it up automatically — no count to bump.
