# Benchmarks

These pages show how Cajeta performs, measured against the languages it
competes with and the systems its libraries take on. They are laid out the
way the [stdlib reference](../docs/stdlib/README.md) is, with one page per
package and one page per class or capability that can be measured. Each page
says what is measured, against what, and how it is checked, and it shows the
chart of the last published run for the machine selected at the top of the
page.

The first published run is proton's, 2026-11-09, on cajeta 0.53.2: the
compiler catalog against Rust, C++, Go, Java and Python, the HIP C++ and
PyTorch device competitors, dev.cajeta.ml against scikit-learn and PyTorch,
and XGBoost. The llm, http and primavera libraries and the Phoenix (CUDA)
rows are not in it yet. Every page below carries its standing from that run.

## How to read a page

Every page has the same six sections: what is measured and why that
workload; the benchmarks table (id, workload and size, what it is measured
against, the correctness check); the results, one chart per benchmark with
languages as bars and a table under each; the standing, written after the
run; how to reproduce; and where the class, its source and the benchmark
source live. [Methodology](METHODOLOGY.md) states the rules every number
obeys. [Exemptions](EXEMPTIONS.md) lists the stdlib pages with no benchmark
and why.

## Standings

```bench-standings
```

Each package shows the geometric mean of Cajeta's time against the best
competitor over the package's benchmarks, with wins, losses and skips
counted. Parity is 1; above it Cajeta is slower.

On proton, Cajeta is at or ahead of parity where the work is a loop, a
syscall or a GPU kernel: counted loops, direct calls, arrays, value structs,
the Benchmarks Game programs, file and socket throughput, process pipes, the
schema encoder, atomics, and the xpu package on the Radeon 8060S (1.2x, with
f64 and f16 GEMM ahead of rocBLAS). It is behind on small heap objects (13x to
21x), locks and channels (8x to 29x), strings and streams (13x to 171x), and
the cpu tensor paths that math, nucleo and the ml cpu rows run on (35x, 142x
and 19x), where every element is read through an out-of-line call
(benchmark-reference 22.2.8). The toolchain's cold paths fill an empty cache
(31x). Each package page names its rows, and the cause where one is
known.

## The language and the stdlib

| Package | |
|---|---|
| [Language and cajeta.lang](lang/README.md) | Loops, exceptions, allocation, dispatch, strings, streams, SIMD, the Benchmarks Game |
| [cajeta.collection](collection/README.md) | Lists, maps, sets, trees, caches, sorting |
| [cajeta.concurrent](concurrent/README.md) | Atomics, locks, channels, fibers and tasks |
| [cajeta.time](time/README.md) | Dates, times, zones, formatting |
| [cajeta.hash](hash/README.md) | XXH3, SipHash, the default hasher |
| [cajeta.codec](codec/README.md) | JSON, CSV, Base64, Base32, DEFLATE |
| [cajeta.io](io/README.md) | Buffers, files, sockets, the reactor |
| [cajeta.math](math/README.md) | Tensors, linear algebra, FFT, random, `.npy`, geometry |
| [cajeta.nucleo](nucleo/README.md) | The dataframe, autodiff, fusion and transforms |
| [cajeta.wire](wire/README.md) | Wire codecs and zero-copy views |
| [cajeta.search](search/README.md) | Edit distance, fuzzy matching, n-grams |
| [cajeta.process](process/README.md) | Spawning and piping |
| [cajeta.reflect](reflect/README.md) | Runtime type information |
| [cajeta.aot](aot/README.md) | The dependency-injection graph |
| [cajeta.xpu](xpu/README.md) | Device bandwidth, roofline, GEMM, one kernel on every backend |
| [cajeta.gfx](gfx/README.md) | Texture sampling |
| [Toolchain](toolchain/README.md) | Compile time, start-up, binary size, build tool, notebook kernel, debugger |

## The libraries

| Library | Against | |
|---|---|---|
| [dev.cajeta.llm](llm/README.md) | llama.cpp | Prefill and decode tokens/s by OS, driver, model and platform |
| [dev.cajeta.ml](ml/README.md) | scikit-learn, PyTorch, XGBoost | Classical estimators, neural models, boosted trees |
| [cajeta-http](http/README.md) | Go, axum, Netty, Node, uvicorn | Serving, WebSocket, SSE, TLS, client, start-up |
| [primavera](primavera/README.md) | Spring Boot, Quarkus, Micronaut, ASP.NET Core, FastAPI | The service shape, templates, start-up, build, DI |

## Machines

Phoenix is a Ryzen 9 3950X with an RTX 4090 and 94 GB, on Ubuntu 24.04
under WSL2, with CUDA and Vulkan. Proton is a Ryzen AI MAX+ 395 with the
Radeon 8060S and 64 GB, on Ubuntu 26.04, with HIP and Vulkan. CPU and
language rows default to Proton, and the selector at the top of every page
switches.

## Running it

```sh
cd bench
./bench.sh --list                 # every benchmark under its catalog path
./bench.sh --bench hashmap-int    # one benchmark, Cajeta only
./bench.sh --area collection      # one package
./bench.sh --competitors ...      # the same, every pinned competitor too
```

The harness is the library `dev.cajeta.bench`. Each ecosystem library has
its own `bench/` on the same harness and the same CSV schema, and its rows
are copied here at publish time. Competitor toolchains are provisioned by
`competitors/setup.sh`.
