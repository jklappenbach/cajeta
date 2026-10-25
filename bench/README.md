# Benchmarks

How Cajeta performs, measured against the languages it competes with and
the systems its libraries take on, laid out the way the
[stdlib reference](../docs/stdlib/README.md) is: one page per package, one
page per class or capability that can be measured. Each page says what is
measured, against what, how it is checked, and shows the chart of the last
published run for the machine selected at the top of the page.

**Status.** The catalog, the harness and the competitor programs are in
place; the first published run waits for a quiet box. Until then every
chart says "not yet measured" and no page carries a standing. The numbers
will arrive as one commit of CSV files per machine.

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

Per package, the geometric mean of Cajeta's time against the best competitor
over the package's benchmarks, with wins, losses and skips counted. Written
up after the first run.

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

Phoenix: Ryzen 9 3950X, RTX 4090, 94 GB, Ubuntu 24.04 under WSL2 (CUDA and
Vulkan). Proton: Ryzen AI MAX+ 395 with the Radeon 8060S, 64 GB, Ubuntu
26.04 (HIP and Vulkan). CPU and language rows default to Proton; the
selector at the top of every page switches.

## Running it

```sh
cd bench
./bench.sh --list                 # every benchmark under its catalog path
./bench.sh --bench hashmap-int    # one benchmark, Cajeta only
./bench.sh --area collection      # one package
./bench.sh --competitors ...      # the same, every pinned competitor too
```

The harness is the library `dev.cajeta.bench`; each ecosystem library has
its own `bench/` on the same harness and the same CSV schema, and its rows
are copied here at publish time. Competitor toolchains are provisioned by
`competitors/setup.sh`.
