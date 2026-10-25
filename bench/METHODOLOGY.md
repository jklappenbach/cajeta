# Methodology

The rules every number on this site obeys. A row that breaks one is not
published; the fidelity gate (`scripts/verify-fidelity.py`) checks what can
be checked mechanically, and the rest is the discipline of the run.

## Builds

Cajeta is built `--release --opt=O3 --cpu=native`, native ahead-of-time, no
JIT warm-up. Each competitor is built the way its community builds for
speed: C++ at `-O3 -march=native`, Rust with `target-cpu=native` in release,
Go with its default optimizer, Java 25 on the JIT with warm-up iterations
before the measured trials, CPython 3.12 as shipped with NumPy where the
idiomatic program uses it. The exact flags are in every row.

## Measurement

Every benchmark declares its warm-up and trial counts. The row records the
minimum, the median, the mean and the 95th percentile of the trials in
nanoseconds, the throughput in the benchmark's own unit when it has one,
allocated bytes and allocation count during the measured window, peak
resident memory, and the working-set delta. A per-iteration baseline (a
fresh buffer, an unavoidable allocation) is subtracted when the benchmark
declares one, and the row says so.

## Same work

Every language performs the same logical work on the same input: the same
element count, the same dataset by checksum, the same iteration count. The
competitor uses the API the page names, not a faster primitive substituted
by hand. Every result is consumed through the harness sink so no optimizer
can delete or constant-fold the measured work; the smoke tests grep the
release IR of the loop benchmarks to confirm the loop survived.

## Correctness before speed

Every benchmark has a check: a sum, a checksum, a token count, an agreement
rate, a residual. A row whose check fails is `failed`, with the reason, and
is never a bar. For the language-model rows the check is the greedy
agreement rate against llama.cpp on the same prompt ids, and a throughput is
read only beside its printed tokens.

## Skips are data

A benchmark that cannot run on a machine records a `skipped` row with its
reason: a missing toolchain, a dataset not fetched, an unsupported variant, a
memory budget. The chart shows the skip as an empty bar with the reason;
nothing is left blank.

## Devices

GPU clocks are bimodal, so a device row is measured in interleaved rounds:
ours, the competitor, ours, the competitor, and the median is read. A single
pair is never believed. Device time comes from the `KernelTimer` on its
stream; the timer's own resolution and its calibrated scale against the host
clock are published under `xpu/KernelTimer`. Every device row carries the
backend and the driver as reported at run time.

## Machines

Two machines, both published, one shown at a time:

| | Phoenix | Proton |
|---|---|---|
| CPU | AMD Ryzen 9 3950X, 16 cores | AMD Ryzen AI MAX+ 395, 16 cores |
| Memory | 94 GB | 64 GB |
| GPU | NVIDIA GeForce RTX 4090, 24 GB | AMD Radeon 8060S (gfx1151) |
| OS | Ubuntu 24.04 under WSL2; Windows 11 native for some rows | Ubuntu 26.04 |
| Backends | CUDA, Vulkan, CPU | HIP, Vulkan, CPU |

CPU and language rows default to Proton (native Linux, no hypervisor
between the clock and the hardware). CUDA rows are from Phoenix.

## A quiet box

A published run starts only when the load average is under 1, no CI runner
job is active, and the GPU shows zero utilization and no compute processes.
That evidence is recorded in the run's `env.csv`. Every run is announced
before it starts with its start time, its estimated end and the basis of the
estimate, and reported against that estimate when it ends.

## Provenance

Every row carries the compiler version and flags, the competitor language,
library and version, the machine, the operating system, the driver, the
model where there is one, the platform that produced the row, the dataset
name, size and checksum, and the run id. A row that cannot record a version
is a failed row, not an unlabeled number.

## Smoke rows are never published

The smoke tests run every benchmark for one warm-up and three trials to
prove it builds, checks and emits a row. Those rows are tagged `smoke` and
the publish step drops them. No smoke number reaches a chart.

## Where the numbers come from

Suites and sources the workloads mirror: nativejson-benchmark for JSON,
Martin Ankerl's hashmap suite, sort-research-rs, StringZilla and simdutf for
strings and Base64, SMHasher for hashing, the crossbeam-channel harness for
channels, the Computer Language Benchmarks Game for whole programs,
BabelStream and mixbench for devices, the h2oai db-benchmark for dataframes,
TechEmpower's shapes for HTTP, and llama.cpp's own `llama-bench` and
`llama-batched-bench` for language models.
