# Whole programs: the Benchmarks Game

This page runs three Computer Language Benchmarks Game problems at their canonical sizes, with the published checksums verified. They are mandelbrot, fannkuch-redux and spectral-norm. These are the rows you can cross-check against the public tables.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/clbg-mandelbrot` | mandelbrot at N = 16000, PBM checksum | the CLBG C++, Rust, Go, Java and Python entries | checksum |
| `lang/clbg-fannkuch-redux` | fannkuch-redux at N = 12 | same | checksum and max flips |
| `lang/clbg-spectral-norm` | spectral-norm at N = 5500 | same | value to 9 digits |

Sources and sizes follow the Benchmarks Game's problem pages. The competitor programs are the game's own entries, built with the flags the game records.

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

## Standing

Mandelbrot is a win at 0.18x of the scalar C++ program, 6x ahead of Rust and Go. Spectral norm is 0.77x of C++, fannkuch-redux 0.99x of Go and within 8% of Rust and C++. The three Benchmarks Game programs are at or ahead of parity.

## Reproduce

```sh
cd bench
./bench.sh --bench clbg-mandelbrot               # Cajeta
./bench.sh --bench clbg-mandelbrot --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
