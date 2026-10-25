# Whole programs: the Benchmarks Game

Three Computer Language Benchmarks Game problems at their canonical sizes, with the published checksums verified: mandelbrot, fannkuch-redux and spectral-norm. These are the rows a reader can cross-check against the public tables.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/clbg-mandelbrot` | mandelbrot at N = 16000, PBM checksum | the CLBG C++, Rust, Go, Java and Python entries | checksum |
| `lang/clbg-fannkuch-redux` | fannkuch-redux at N = 12 | same | checksum and max flips |
| `lang/clbg-spectral-norm` | spectral-norm at N = 5500 | same | value to 9 digits |

Sources and sizes follow the Benchmarks Game's problem pages; the competitor programs are the game's own entries, built with the flags the game records.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/clbg-mandelbrot
metric: time
```

```bench
id: lang/clbg-fannkuch-redux
metric: time
```

```bench
id: lang/clbg-spectral-norm
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench clbg-mandelbrot               # Cajeta
./bench.sh --bench clbg-mandelbrot --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
